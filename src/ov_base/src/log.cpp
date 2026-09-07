#include "ov/base/log.hpp"

#include "ov/base/thread.hpp"

#include <fmt/chrono.h>
#include <fmt/color.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#if OV_PLATFORM_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ov {
namespace {

std::atomic<LogLevel> g_threshold{LogLevel::Info};

// stderr writes from several threads interleave mid-line without this. The lock
// is uncontended in practice: the tick loop logs almost nothing at Info.
std::mutex g_mutex;
std::FILE* g_file = nullptr;

constexpr std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF  ";
    }
    return "?????";
}

fmt::text_style level_style(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return fg(fmt::terminal_color::bright_black);
        case LogLevel::Debug: return fg(fmt::terminal_color::cyan);
        case LogLevel::Info: return fg(fmt::terminal_color::green);
        case LogLevel::Warn: return fg(fmt::terminal_color::yellow);
        case LogLevel::Error: return fg(fmt::terminal_color::red) | fmt::emphasis::bold;
        case LogLevel::Off: return {};
    }
    return {};
}

bool stderr_is_tty() noexcept {
    static const bool tty = [] {
#if OV_PLATFORM_WINDOWS
        return _isatty(_fileno(stderr)) != 0;
#else
        return isatty(fileno(stderr)) != 0;
#endif
    }();
    return tty;
}

}  // namespace

namespace detail {

LogLevel log_threshold() noexcept {
    return g_threshold.load(std::memory_order_relaxed);
}

void log_write(LogLevel level, std::string_view category, std::string_view message) noexcept {
    const auto  timestamp = std::chrono::system_clock::now();
    const auto  thread    = current_thread_name();
    std::string line;
    try {
        line = fmt::format("[{:%H:%M:%S}] [{}/{}] [{}] {}\n",
                           std::chrono::floor<std::chrono::milliseconds>(timestamp), thread,
                           level_name(level), category, message);
    } catch (...) {
        return;
    }

    const std::scoped_lock lock{g_mutex};
    if (stderr_is_tty()) {
        fmt::print(stderr, level_style(level), "{}", line);
    } else {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
    if (g_file != nullptr) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fflush(g_file);
    }
}

}  // namespace detail

void set_log_level(LogLevel level) noexcept {
    g_threshold.store(level, std::memory_order_relaxed);
}

LogLevel parse_log_level(std::string_view name) noexcept {
    if (name == "trace")
        return LogLevel::Trace;
    if (name == "debug")
        return LogLevel::Debug;
    if (name == "info")
        return LogLevel::Info;
    if (name == "warn")
        return LogLevel::Warn;
    if (name == "error")
        return LogLevel::Error;
    if (name == "off")
        return LogLevel::Off;
    return LogLevel::Info;
}

bool set_log_file(std::string_view path) noexcept {
    const std::scoped_lock lock{g_mutex};
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (path.empty()) {
        return true;
    }
    const std::string zpath{path};
    g_file = std::fopen(zpath.c_str(), "a");
    return g_file != nullptr;
}

}  // namespace ov
