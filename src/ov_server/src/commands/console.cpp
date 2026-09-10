#include "console.hpp"

#include "ov/base/thread.hpp"

#if defined(_WIN32)
#include <iostream>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace ov::server::cmd {

ConsoleReader::ConsoleReader(std::function<void(std::string)> on_line)
    : on_line_{std::move(on_line)}, thread_{[this] { run(); }} {}

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

}  // namespace ov::server::cmd
