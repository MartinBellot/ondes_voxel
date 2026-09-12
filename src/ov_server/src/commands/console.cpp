#include "console.hpp"

#include "ov/base/thread.hpp"

#include <algorithm>
#include <cstdio>

#if defined(_WIN32)
#include <iostream>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ov::server::cmd {

CompletedLine complete_line(std::string_view line, const ConsoleSuggestions& s) {
    CompletedLine out{std::string{line}, {}};
    if (s.matches.empty() || s.start > line.size()) {
        return out;
    }
    std::string replacement = s.matches.front();
    if (s.matches.size() > 1) {
        for (const std::string& m : s.matches) {
            usize common = 0;
            while (common < replacement.size() && common < m.size() && replacement[common] == m[common]) {
                ++common;
            }
            replacement.resize(common);
        }
        out.listing = s.matches;
    }
    const usize length = std::min(s.length, line.size() - s.start);
    // Never shorten what was typed: a common prefix shorter than the word
    // (the suggestions may match case-insensitively) leaves the line alone.
    if (replacement.size() < length) {
        return out;
    }
    out.line = std::string{line.substr(0, s.start)} + replacement +
               std::string{line.substr(s.start + length)};
    if (s.matches.size() == 1 && out.line.size() == s.start + replacement.size()) {
        out.line += ' ';
    }
    return out;
}

ConsoleReader::ConsoleReader(std::function<void(std::string)> on_line, Completer complete)
    : on_line_{std::move(on_line)}, complete_{std::move(complete)}, thread_{[this] { run(); }} {}

ConsoleReader::~ConsoleReader() {
    stop_.store(true, std::memory_order_relaxed);
#if defined(_WIN32)
    // std::getline cannot be interrupted; the thread is left to end with the
    // process rather than blocking the shutdown on a line nobody will type.
    thread_.detach();
#else
    thread_.join();
#endif
}

void ConsoleReader::run() {
    set_thread_role("ov-console", ThreadRole::Io);
#if defined(_WIN32)
    std::string line;
    while (!stop_.load(std::memory_order_relaxed) && std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        on_line_(std::move(line));
    }
#else
    if (complete_ && ::isatty(STDIN_FILENO) == 1 && ::isatty(STDOUT_FILENO) == 1) {
        run_terminal();
        return;
    }
    std::string pending;
    char        buffer[1024];
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&fd, 1, 100);
        if (ready <= 0) {
            continue;
        }
        if ((fd.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got <= 0) {
            break;  // stdin closed: nothing more will ever come
        }
        pending.append(buffer, static_cast<std::size_t>(got));
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            on_line_(std::move(line));
        }
    }
#endif
}

void ConsoleReader::run_terminal() {
#if !defined(_WIN32)
    termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) != 0) {
        return;
    }
    termios raw = original;
    // Keys one at a time, not echoed; signals (Ctrl-C) still work.
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    std::string line;
    const auto  redraw = [&] {
        std::fprintf(stdout, "\r\x1b[K> %s", line.c_str());
        std::fflush(stdout);
    };
    bool escape = false;
    redraw();
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        if (::poll(&fd, 1, 100) <= 0 || (fd.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }
        char       c   = 0;
        const auto got = ::read(STDIN_FILENO, &c, 1);
        if (got <= 0) {
            break;
        }
        if (escape) {
            // An escape sequence (arrows, …): skipped up to its final letter.
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                escape = false;
            }
            continue;
        }
        if (c == 0x1B) {
            escape = true;
        } else if (c == '\n' || c == '\r') {
            std::fprintf(stdout, "\r\x1b[K");
            std::fflush(stdout);
            on_line_(line);
            line.clear();
            redraw();
        } else if (c == 0x7F || c == 0x08) {
            if (!line.empty()) {
                line.pop_back();
            }
            redraw();
        } else if (c == 0x04 && line.empty()) {
            break;  // Ctrl-D on an empty line: the end of input
        } else if (c == '\t') {
            const CompletedLine done = complete_line(line, complete_(line));
            if (!done.listing.empty()) {
                std::string list;
                for (const std::string& m : done.listing) {
                    list += (list.empty() ? "" : "  ") + m;
                }
                std::fprintf(stdout, "\r\x1b[K%s\n", list.c_str());
            }
            line = done.line;
            redraw();
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            line.push_back(c);
            redraw();
        }
    }
    std::fprintf(stdout, "\r\x1b[K");
    std::fflush(stdout);
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &original);
#endif
}

}  // namespace ov::server::cmd
