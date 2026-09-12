#include "tick_thread_lock.hpp"

#include "ov/base/assert.hpp"
#include "ov/base/thread.hpp"

namespace ov::server {

void TickThreadLock::fail() noexcept {
    const std::string_view name = current_thread_name();
    OV_UNREACHABLE("the world was reached from thread '{}', which is not the tick thread — "
                   "only the tick thread may touch chunks or players (CLAUDE.md principle 3)",
                   name.empty() ? std::string_view{"unnamed"} : name);
}

}  // namespace ov::server
