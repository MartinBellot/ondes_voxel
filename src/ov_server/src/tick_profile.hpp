// What a tick spends its time on, phase by phase — and how much of that time
// the tick thread was actually running.
//
// The server already printed one histogram for the whole tick and one for the
// spawner. That says a tick was slow and cannot say why: the report that
// started this file read "p90 609871 us" and every candidate — lighting,
// worldgen, the spawner, a machine with nine compilers on it — fitted it
// equally well. So every phase of the loop gets its own line, and each line
// carries two clocks:
//
//   * wall time, which is what the player waits for;
//   * the tick thread's own CPU time, which is what the phase *costs*.
//
// A phase whose CPU time is close to its wall time is slow because it does too
// much. A phase whose CPU time is a fraction of its wall time was waiting — for
// a mutex the network thread held, for a core the scheduler gave to somebody
// else, for a page coming back from swap — and optimising its code would buy
// nothing. That ratio is the whole point of the file.
//
// Instrumentation only. The clocks read here never feed a decision, which is
// what keeps principle 5 (no wall clock in the logic) intact. Nothing here
// allocates after construction: the histograms are fixed arrays of counters,
// so a server can run for a month without the profile growing — the old
// per-tick sample vectors could not.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server {

/// A latency histogram in microseconds, with bounded relative error.
///
/// Values under 64 us have a bucket each; above that every power of two is
/// split into 32 buckets, so a percentile read back is at most 1/32 (3.1 %)
/// below the true sample. `max`, `total` and `count` are exact. Recording is
/// thread-safe and lock-free — relaxed atomics, because a sample is a
/// statistic and not a synchronisation point.
class LatencyHistogram {
public:
    /// One tick at 20 Hz. The count above it is kept exactly, because it is
    /// the number the server's health is judged on.
    static constexpr i64   kBudgetMicros = 50'000;
    static constexpr usize kExactBelow   = 64;
    static constexpr usize kSubBuckets   = 32;
    /// 2^6 .. 2^46 us: up to about two years in one sample.
    static constexpr usize kOctaves     = 40;
    static constexpr usize kBucketCount = kExactBelow + kOctaves * kSubBuckets;

    void record(i64 micros) noexcept;

    [[nodiscard]] u64 count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] i64 max() const noexcept { return max_.load(std::memory_order_relaxed); }
    [[nodiscard]] i64 total() const noexcept { return total_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 over_budget() const noexcept {
        return over_budget_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] i64 mean() const noexcept;

    /// The sample at `quantile` in [0, 1], by the same rank rule the server's
    /// other histograms use (`quantile * (count - 1)`), rounded down to its
    /// bucket and never above the exact maximum. 0 when empty.
    [[nodiscard]] i64 percentile(f64 quantile) const noexcept;

    [[nodiscard]] static usize bucket_of(i64 micros) noexcept;
    [[nodiscard]] static i64   bucket_floor(usize bucket) noexcept;

private:
    std::array<std::atomic<u32>, kBucketCount> buckets_{};
    std::atomic<u64>                           count_{0};
    std::atomic<u64>                           over_budget_{0};
    std::atomic<i64>                           total_{0};
    std::atomic<i64>                           max_{0};
};

/// Wall time from construction to destruction, into a histogram. For the
/// network thread, whose handler has a dozen return paths.
class ScopedLatency {
public:
    explicit ScopedLatency(LatencyHistogram& into) noexcept
        : into_{&into}, started_{std::chrono::steady_clock::now()} {}
    ~ScopedLatency();

    ScopedLatency(const ScopedLatency&)            = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;

private:
    LatencyHistogram*                     into_;
    std::chrono::steady_clock::time_point started_;
};

/// The phases of the server loop, in the order it runs them.
enum class TickPhase : u8 {
    /// Anything between two named phases. Should stay near zero; if it does
    /// not, a phase is missing a name.
    Other,
    /// Packets the network thread queued during the last tick, handled here
    /// (inbound_queue.hpp). Packets that arrive while the loop waits for the
    /// next tick are handled then, outside any tick, and only show in the
    /// `network packet` histogram.
    NetworkInput,
    /// Generated chunks moved from the workers into the map.
    ChunkPublish,
    /// Counting the spawn area in, until it is complete.
    SpawnArea,
    /// Asking the generator for what the tickets want.
    ChunkRequests,
    /// Dropping chunks no ticket reaches, every five seconds.
    ChunkEviction,
    /// The Update Time packet and the command-line mobs.
    WorldClock,
    /// The command queue, spawn eggs included.
    Commands,
    /// Fluids and redstone: the two queues and their notifications.
    ScheduledTicks,
    /// Sky and block light recomputed after block writes.
    Relight,
    /// Crops, saplings and leaves.
    RandomTicks,
    /// Hoppers, droppers, dispensers.
    Containers,
    NaturalSpawning,
    /// Mobs, TNT, falling blocks, projectiles, animals.
    Entities,
    /// Item stacks and experience orbs on the ground.
    GroundItems,
    /// Combat gauges, effects, health, hunger, respawn.
    Players,
    /// Survival digs finished on the server's clock.
    Digs,
    Autosave,
    /// Chunk Data packets encoded and sent, eight per player per tick.
    ChunkSend,
    /// Open screens, furnaces nobody watches, keep-alives.
    Screens,
    Count,
};

[[nodiscard]] std::string_view to_string(TickPhase phase) noexcept;

/// The tick thread's profile. `begin_tick`, `enter` and `end_tick` must only
/// ever be called from the tick thread; the three network histograms at the
/// bottom may be filled from any thread.
class TickProfile {
public:
    static constexpr usize kPhaseCount = static_cast<usize>(TickPhase::Count);

    TickProfile() noexcept;

    TickProfile(const TickProfile&)            = delete;
    TickProfile& operator=(const TickProfile&) = delete;

    /// Start a loop iteration. An iteration left without `end_tick` (a
    /// `continue` somewhere) is discarded, never half-counted.
    void begin_tick() noexcept;

    /// Close the current phase and open `phase`. Returns the phase that was
    /// open, so a helper called from several places can hand the time back:
    /// `const auto was = profile.enter(Relight); ...; profile.enter(was);`
    TickPhase enter(TickPhase phase) noexcept;

    void end_tick() noexcept;

    /// The shutdown report, one log line per entry. Allocates, so it is for
    /// the end of the run and never for the tick.
    [[nodiscard]] std::vector<std::string> report(i64 clock_ticks) const;

    [[nodiscard]] const LatencyHistogram& phase(TickPhase which) const noexcept {
        return phases_[static_cast<usize>(which)];
    }
    [[nodiscard]] const LatencyHistogram& tick() const noexcept { return tick_; }
    [[nodiscard]] u64 iterations() const noexcept { return iterations_; }
    [[nodiscard]] u64 slow_ticks() const noexcept { return slow_ticks_; }
    /// In how many ticks over budget this phase was the largest single cost.
    [[nodiscard]] u64 heaviest_in_slow(TickPhase which) const noexcept {
        return heaviest_in_slow_[static_cast<usize>(which)];
    }
    /// Tick-thread CPU time spent in a phase, summed over the run.
    [[nodiscard]] i64 cpu_micros(TickPhase which) const noexcept {
        return cpu_total_[static_cast<usize>(which)];
    }

    /// One Play packet on the network thread, from before it takes the player
    /// lock to its return — the latency a player's dig or step pays there.
    LatencyHistogram network_packet;
    /// Of that, the wait for the player lock alone: time the tick held it.
    LatencyHistogram network_lock_wait;
    /// A block written and broadcast by `set_block_and_broadcast`, from
    /// whichever thread wrote it.
    LatencyHistogram block_edit;
    /// How long a packet waited between the network thread framing it and the
    /// tick thread handling it (inbound_queue.hpp). ── concurrency ──
    LatencyHistogram network_queue_wait;

private:
    void close_phase(i64 wall_now, i64 cpu_now) noexcept;

    std::array<LatencyHistogram, kPhaseCount> phases_;
    LatencyHistogram                          tick_;

    std::array<i64, kPhaseCount>  wall_this_tick_{};
    std::array<i64, kPhaseCount>  cpu_this_tick_{};
    std::array<bool, kPhaseCount> ran_this_tick_{};
    std::array<i64, kPhaseCount>  cpu_total_{};
    std::array<u64, kPhaseCount>  heaviest_in_slow_{};

    TickPhase current_{TickPhase::Other};
    bool      open_{false};
    i64       mark_wall_{0};
    i64       mark_cpu_{0};
    i64       tick_wall_start_{0};
    i64       tick_cpu_start_{0};
    i64       tick_cpu_total_{0};
    i64       first_wall_{-1};
    i64       last_wall_{0};
    u64       iterations_{0};
    u64       slow_ticks_{0};

    /// Process-wide scheduler and paging counters at construction, so the
    /// report can say how often the kernel took the CPU away or went to swap.
    struct ProcessCounters {
        i64 involuntary_switches{0};
        i64 voluntary_switches{0};
        i64 major_faults{0};
        i64 minor_faults{0};
    };
    [[nodiscard]] static ProcessCounters process_counters() noexcept;
    ProcessCounters                      at_start_{};
};

/// Microseconds on a monotonic clock. Instrumentation only.
[[nodiscard]] i64 monotonic_micros() noexcept;

/// CPU time the calling thread has consumed, in microseconds. Resolution is
/// the platform's: a microsecond on macOS and Linux, 15.6 ms on Windows.
[[nodiscard]] i64 thread_cpu_micros() noexcept;

}  // namespace ov::server
