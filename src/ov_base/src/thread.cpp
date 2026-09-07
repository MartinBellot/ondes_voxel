#include "ov/base/thread.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <string>
#include <thread>

#if OV_PLATFORM_MACOS
#include <pthread.h>
#include <sys/qos.h>
#elif OV_PLATFORM_LINUX
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif OV_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
// windows.h defines min and max as macros, which turns any later std::min<T>
// into std::(...) and produces an error pointing at the call site rather than
// at the include. Every translation unit that reaches windows.h needs this.
#define NOMINMAX
// Only windows.h: it pulls in processthreadsapi.h itself, and including that
// directly invites clang-format to sort it ahead of windows.h, which it needs.
#include <windows.h>
#endif

namespace ov {
namespace {

// Owned per thread so the logger can label lines without a lookup table.
thread_local std::string t_thread_name;

#if OV_PLATFORM_MACOS

qos_class_t to_qos(ThreadRole role) noexcept {
    switch (role) {
        // The tick thread is USER_INTERACTIVE, not USER_INITIATED. Anything
        // lower and the scheduler parks it on an efficiency core under load,
        // which reads as unexplained TPS loss.
        case ThreadRole::Interactive:
        case ThreadRole::Tick: return QOS_CLASS_USER_INTERACTIVE;
        case ThreadRole::Network: return QOS_CLASS_USER_INITIATED;
        case ThreadRole::Worker:
        case ThreadRole::Io: return QOS_CLASS_UTILITY;
        case ThreadRole::Background: return QOS_CLASS_BACKGROUND;
    }
    return QOS_CLASS_DEFAULT;
}

#elif OV_PLATFORM_LINUX

int to_nice(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::Interactive:
        case ThreadRole::Tick: return -5;
        case ThreadRole::Network: return -2;
        case ThreadRole::Worker:
        case ThreadRole::Io: return 0;
        case ThreadRole::Background: return 10;
    }
    return 0;
}

#elif OV_PLATFORM_WINDOWS

int to_priority(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::Interactive:
        case ThreadRole::Tick: return THREAD_PRIORITY_ABOVE_NORMAL;
        case ThreadRole::Network: return THREAD_PRIORITY_NORMAL;
        case ThreadRole::Worker:
        case ThreadRole::Io: return THREAD_PRIORITY_NORMAL;
        case ThreadRole::Background: return THREAD_PRIORITY_BELOW_NORMAL;
    }
    return THREAD_PRIORITY_NORMAL;
}

#endif

}  // namespace

void set_thread_name(std::string_view name) noexcept {
    t_thread_name.assign(name);

#if OV_PLATFORM_MACOS
    // macOS names the calling thread only, and truncates at 64 bytes.
    const std::string truncated{name.substr(0, std::min<std::size_t>(name.size(), 63))};
    ::pthread_setname_np(truncated.c_str());
#elif OV_PLATFORM_LINUX
    // Linux caps thread names at 16 bytes including the terminator.
    const std::string truncated{name.substr(0, std::min<std::size_t>(name.size(), 15))};
    ::pthread_setname_np(::pthread_self(), truncated.c_str());
#elif OV_PLATFORM_WINDOWS
    const std::wstring wide(name.begin(), name.end());
    ::SetThreadDescription(::GetCurrentThread(), wide.c_str());
#endif
}

void set_thread_role(std::string_view name, ThreadRole role) noexcept {
    set_thread_name(name);

#if OV_PLATFORM_MACOS
    if (const int rc = ::pthread_set_qos_class_self_np(to_qos(role), 0); rc != 0) {
        // Not fatal, but it means the 20 TPS guarantee is no longer backed by
        // anything on this machine. Say so rather than degrade silently.
        OV_LOG_WARN("failed to set QoS class for thread '{}' (errno {})", name, rc);
    }
#elif OV_PLATFORM_LINUX
    const auto tid = static_cast< ::id_t>(::syscall(SYS_gettid));
    if (::setpriority(PRIO_PROCESS, tid, to_nice(role)) != 0) {
        // Lowering nice needs privileges; failing is expected and harmless.
        OV_LOG_DEBUG("could not set nice level for thread '{}'", name);
    }
#elif OV_PLATFORM_WINDOWS
    ::SetThreadPriority(::GetCurrentThread(), to_priority(role));
#else
    OV_UNUSED(role);
#endif
}

std::string_view current_thread_name() noexcept {
    return t_thread_name.empty() ? std::string_view{"?"} : std::string_view{t_thread_name};
}

unsigned recommended_worker_count() noexcept {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    // Reserve the render thread and the tick thread, then cap at 4. The cap is
    // a memory-bandwidth limit on unified-memory hardware, not a core count.
    const unsigned available = hw > 2 ? hw - 2 : 1;
    return std::min(available, 4u);
}

}  // namespace ov
