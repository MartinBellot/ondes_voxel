// Catching allocations where they must not happen.
//
// Principle 5 of the project: the server tick allocates nothing in steady
// state. Not as an aspiration — as something checked. A malloc in the tick loop
// is invisible in a profile until the day it is not: the allocator takes a lock,
// or the arena grows, and one tick in a thousand takes 5 ms instead of 0.3, and
// the server stutters in a way that reproduces on nobody's machine.
//
// The rule cannot be enforced by review. Code that allocates rarely looks like
// code that allocates: a std::string built for a log message, a vector that
// outgrows its reserve, a std::function capturing one lambda too many. So the
// tick declares the invariant and the allocator reports it.
//
//     void Server::tick() {
//         const NoAllocScope guard{"server tick"};
//         ...
//     }
//
// Debug builds abort with the offending size and the scope name. Release builds
// compile the whole thing away — the guard is an empty object and the hook is
// never installed.
//
// This deliberately does not replace the allocator. mimalloc arrives when there
// is a profile asking for it; this is about the invariant, which is worth
// having from the first tick rather than the thousandth.
#pragma once

#include "ov/base/platform.hpp"
#include "ov/base/types.hpp"

#include <string_view>

namespace ov {

/// Statistics gathered by the allocation hook. Debug builds only.
struct AllocationStats {
    u64 allocations{0};
    u64 bytes{0};
    /// Allocations that happened inside a NoAllocScope.
    u64 violations{0};
};

#if defined(NDEBUG)

/// Release: an empty object, and the compiler removes it entirely.
class NoAllocScope {
public:
    explicit NoAllocScope(std::string_view = {}) noexcept {}
};

[[nodiscard]] inline AllocationStats allocation_stats() noexcept {
    return {};
}

inline void reset_allocation_stats() noexcept {}

/// Whether the hook is compiled in at all, so tests can skip rather than fail.
inline constexpr bool kAllocationTrackingEnabled = false;

#else

/// True only when operator new is actually replaced.
///
/// A sanitizer build stands aside — ASan's own operator new gives redzones and
/// use-after-free detection, which is worth more than a counter — so the tests
/// have to ask rather than assume.
[[nodiscard]] bool allocation_tracking_active() noexcept;

inline constexpr bool kAllocationTrackingEnabled = true;

namespace detail {

void enter_no_alloc_scope(std::string_view name) noexcept;
void leave_no_alloc_scope() noexcept;

}  // namespace detail

/// Declares that no allocation may happen for the lifetime of this object, on
/// this thread. Nesting is allowed; the innermost name is the one reported.
class NoAllocScope {
public:
    explicit NoAllocScope(std::string_view name) noexcept { detail::enter_no_alloc_scope(name); }

    ~NoAllocScope() { detail::leave_no_alloc_scope(); }

    NoAllocScope(const NoAllocScope&)            = delete;
    NoAllocScope& operator=(const NoAllocScope&) = delete;
};

[[nodiscard]] AllocationStats allocation_stats() noexcept;
void                          reset_allocation_stats() noexcept;

/// Report a violation without aborting. Off by default: a violation is a bug,
/// and stopping at the first one is how it gets fixed rather than accumulated.
/// Tests turn it on so they can assert on the count instead of dying.
void set_abort_on_violation(bool abort) noexcept;

#endif

}  // namespace ov
