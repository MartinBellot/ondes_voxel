#include "ov/base/assert.hpp"

#include <cstdio>
#include <cstdlib>

#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"

namespace ov::detail {

void assert_failed(std::string_view expr, std::string_view file, int line,
                   std::string_view function, std::string_view message) noexcept {
    // Written directly to stderr as well as through the log: an assertion may
    // fire while the logger itself is the thing that is broken.
    std::fprintf(stderr,
                 "\n"
                 "════════════════════════════════════════════════════════════════\n"
                 " ONDES VOXEL — ASSERTION FAILED\n"
                 "════════════════════════════════════════════════════════════════\n"
                 "  check    : %.*s\n"
                 "  message  : %.*s\n"
                 "  location : %.*s:%d\n"
                 "  function : %.*s\n"
                 "  thread   : %.*s\n"
                 "════════════════════════════════════════════════════════════════\n\n",
                 static_cast<int>(expr.size()), expr.data(),
                 static_cast<int>(message.size()), message.data(),
                 static_cast<int>(file.size()), file.data(), line,
                 static_cast<int>(function.size()), function.data(),
                 static_cast<int>(current_thread_name().size()), current_thread_name().data());
    std::fflush(stderr);

#if !defined(NDEBUG)
    OV_DEBUG_BREAK();
#endif
    std::abort();
}

}  // namespace ov::detail
