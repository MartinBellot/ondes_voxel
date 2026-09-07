// Assertions.
//
//   OV_ASSERT  — debug only, compiled out in release. Use for invariants that
//                are the code's own responsibility.
//   OV_ENSURE  — always on. Use where violation means silent corruption:
//                palette bounds, packet lengths, region file offsets.
//   OV_UNREACHABLE — states that control flow cannot arrive here.
//
// A failing check writes file, line, function and a formatted message, then
// aborts. It never throws: these fire on threads where unwinding is meaningless.
#pragma once

#include "ov/base/platform.hpp"

#include <fmt/format.h>

#include <string>
#include <string_view>

namespace ov::detail {

[[noreturn]] void assert_failed(std::string_view expr, std::string_view file, int line,
                                std::string_view function, std::string_view message) noexcept;

template<typename... Args>
[[noreturn]] void assert_failed_fmt(std::string_view expr, std::string_view file, int line,
                                    std::string_view function, fmt::format_string<Args...> fmt_str,
                                    Args&&... args) noexcept {
    std::string message;
    try {
        message = fmt::format(fmt_str, std::forward<Args>(args)...);
    } catch (...) {
        message = "<message formatting failed>";
    }
    assert_failed(expr, file, line, function, message);
}

}  // namespace ov::detail

#define OV_CHECK_IMPL(cond, ...)                                                                  \
    do {                                                                                          \
        if (!(cond)) [[unlikely]] {                                                               \
            ::ov::detail::assert_failed_fmt(#cond, __FILE__, __LINE__, __func__, "" __VA_ARGS__); \
        }                                                                                         \
    } while (false)

/// Always-on invariant. Survives release builds.
#define OV_ENSURE(cond, ...) OV_CHECK_IMPL(cond, __VA_ARGS__)

#if defined(NDEBUG)
#define OV_ASSERT(cond, ...) OV_UNUSED(sizeof(cond))
#define OV_DEBUG_ONLY(...)
#else
#define OV_ASSERT(cond, ...) OV_CHECK_IMPL(cond, __VA_ARGS__)
#define OV_DEBUG_ONLY(...) __VA_ARGS__
#endif

#define OV_UNREACHABLE(...) \
    ::ov::detail::assert_failed_fmt("unreachable", __FILE__, __LINE__, __func__, "" __VA_ARGS__)
