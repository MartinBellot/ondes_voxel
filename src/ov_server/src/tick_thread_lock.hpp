// What `chunk_mutex` became: not a mutex, a proof.
//
// CLAUDE.md principle 3: the tick thread is the only writer of the world, and
// there is no mutex on the world, ever. The server had one anyway — two,
// `chunk_mutex` and `players_mutex` — because packet handlers ran on the
// network thread and reached into chunks and players from there. Since the
// packets go to the tick thread (inbound_queue.hpp), only one thread ever takes
// either lock, and a lock only one thread takes excludes nobody.
//
// Deleting the ~150 `scoped_lock{chunk_mutex}` lines would have been the
// honest-looking change and the useless one: it rewrites a file every other
// agent edits, and afterwards nothing would notice if a second thread came
// back. This type keeps every one of those lines compiling and gives them a new
// job. `lock()` takes nothing; it **checks** that the caller is the tick thread,
// and a caller that is not is a bug with a name:
//
//   * `Policy::Abort` (Debug builds): stop, naming the thread. A test or a lab
//     session that reaches the world from the wrong thread fails loudly on the
//     first touch instead of racing quietly;
//   * `Policy::Count` (Release builds): count it, for the shutdown report. The
//     number is meant to be zero, and a number is what says so.
//
// Satisfies the standard Lockable requirements, so `std::scoped_lock` over two
// of these, and `std::unique_lock` with `std::try_to_lock`, keep working.
// `try_lock` always succeeds: on the owning thread there is nobody to wait for.
// Re-entry is harmless for the same reason — the deadlocks a non-recursive
// `std::mutex` used to turn into hangs cannot exist any more.
#pragma once

#include "ov/base/types.hpp"

#include <atomic>
#include <thread>

namespace ov::server {

class TickThreadLock {
public:
    enum class Policy : u8 {
        Abort,
        Count,
    };

#if defined(NDEBUG)
    static constexpr Policy kDefaultPolicy = Policy::Count;
#else
    static constexpr Policy kDefaultPolicy = Policy::Abort;
#endif

    /// Owned by the constructing thread. The server builds its locks on the
    /// thread that then runs the tick loop, which is what makes that right.
    explicit TickThreadLock(Policy policy = kDefaultPolicy) noexcept
        : owner_{std::this_thread::get_id()}, policy_{policy} {}

    TickThreadLock(const TickThreadLock&)            = delete;
    TickThreadLock& operator=(const TickThreadLock&) = delete;

    /// Hand ownership to the calling thread. Only before anything is shared.
    void bind_to_current_thread() noexcept {
        owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }

    void lock() noexcept { check(); }
    bool try_lock() noexcept {
        check();
        return true;
    }
    void unlock() noexcept {}

    /// Locks taken from a thread other than the owner. Zero is the claim.
    [[nodiscard]] u64 foreign_locks() const noexcept {
        return foreign_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool owned_by_caller() const noexcept {
        return owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

private:
    void check() noexcept {
        if (owned_by_caller()) [[likely]] {
            return;
        }
        foreign_.fetch_add(1, std::memory_order_relaxed);
        if (policy_ == Policy::Abort) {
            fail();
        }
    }

    [[noreturn]] static void fail() noexcept;

    std::atomic<std::thread::id> owner_;
    Policy                       policy_;
    std::atomic<u64>             foreign_{0};
};

}  // namespace ov::server
