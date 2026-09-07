// Structured logging.
//
// Categories are compile-time string literals so a log site costs one pointer,
// not a string construction. Levels are checked before formatting: a filtered
// OV_LOG_TRACE in the tick loop must not format its arguments.
#pragma once

#include "ov/base/platform.hpp"

#include <fmt/base.h>
#include <fmt/format.h>

#include <string_view>

namespace ov {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

namespace detail {

/// Current threshold. Read on every log site, so it is a plain relaxed atomic.
[[nodiscard]] LogLevel log_threshold() noexcept;

void log_write(LogLevel level, std::string_view category, std::string_view message) noexcept;

}  // namespace detail

/// Set the global threshold. Messages below it are discarded before formatting.
void set_log_level(LogLevel level) noexcept;

/// Parse a level name ("trace", "debug", "info", "warn", "error", "off").
/// Returns LogLevel::Info for unrecognised input.
[[nodiscard]] LogLevel parse_log_level(std::string_view name) noexcept;

/// Route log output to a file in addition to stderr. Passing an empty path
/// stops file logging. Returns false if the file could not be opened.
bool set_log_file(std::string_view path) noexcept;

template<typename... Args>
void log_at(LogLevel level, std::string_view category, fmt::format_string<Args...> fmt_str,
            Args&&... args) noexcept {
    try {
        detail::log_write(level, category, fmt::format(fmt_str, std::forward<Args>(args)...));
    } catch (...) {
        // A logging failure must never take down a tick.
        detail::log_write(LogLevel::Error, "log", "formatting failed");
    }
}

}  // namespace ov

// Each translation unit defines OV_LOG_CATEGORY before including this header,
// or falls back to the module name.
#if !defined(OV_LOG_CATEGORY)
#define OV_LOG_CATEGORY "ov"
#endif

#define OV_LOG_IMPL(level, ...)                                  \
    do {                                                         \
        if (::ov::detail::log_threshold() <= (level)) {          \
            ::ov::log_at((level), OV_LOG_CATEGORY, __VA_ARGS__); \
        }                                                        \
    } while (false)

#define OV_LOG_TRACE(...) OV_LOG_IMPL(::ov::LogLevel::Trace, __VA_ARGS__)
#define OV_LOG_DEBUG(...) OV_LOG_IMPL(::ov::LogLevel::Debug, __VA_ARGS__)
#define OV_LOG_INFO(...) OV_LOG_IMPL(::ov::LogLevel::Info, __VA_ARGS__)
#define OV_LOG_WARN(...) OV_LOG_IMPL(::ov::LogLevel::Warn, __VA_ARGS__)
#define OV_LOG_ERROR(...) OV_LOG_IMPL(::ov::LogLevel::Error, __VA_ARGS__)
