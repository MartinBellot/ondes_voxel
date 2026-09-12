#include "tick_profile.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <numeric>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <sys/resource.h>
#    include <time.h>
#endif

namespace ov::server {

// ── LatencyHistogram ─────────────────────────────────────────────────────────

usize LatencyHistogram::bucket_of(i64 micros) noexcept {
    if (micros <= 0) {
        return 0;
    }
    if (micros < static_cast<i64>(kExactBelow)) {
        return static_cast<usize>(micros);
    }
    const auto value  = static_cast<u64>(micros);
    const auto octave = static_cast<usize>(63 - std::countl_zero(value));  // >= 6
    if (octave >= 6 + kOctaves) {
        return kBucketCount - 1;
    }
    const auto sub = static_cast<usize>((value >> (octave - 5)) & (kSubBuckets - 1));
    return kExactBelow + (octave - 6) * kSubBuckets + sub;
}

i64 LatencyHistogram::bucket_floor(usize bucket) noexcept {
    if (bucket < kExactBelow) {
        return static_cast<i64>(bucket);
    }
    const usize octave = (bucket - kExactBelow) / kSubBuckets + 6;
    const usize sub    = (bucket - kExactBelow) % kSubBuckets;
    return static_cast<i64>((kSubBuckets + sub) << (octave - 5));
}

void LatencyHistogram::record(i64 micros) noexcept {
    micros = std::max<i64>(micros, 0);
    buckets_[bucket_of(micros)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    total_.fetch_add(micros, std::memory_order_relaxed);
    if (micros > kBudgetMicros) {
        over_budget_.fetch_add(1, std::memory_order_relaxed);
    }
    i64 seen = max_.load(std::memory_order_relaxed);
    while (micros > seen && !max_.compare_exchange_weak(seen, micros, std::memory_order_relaxed)) {
    }
}

i64 LatencyHistogram::mean() const noexcept {
    const u64 n = count();
    return n == 0 ? 0 : total() / static_cast<i64>(n);
}

i64 LatencyHistogram::percentile(f64 quantile) const noexcept {
    const u64 n = count();
    if (n == 0) {
        return 0;
    }
    const auto rank = static_cast<u64>(std::clamp(quantile, 0.0, 1.0) * static_cast<f64>(n - 1));
    // The last rank is the maximum, and the maximum is known exactly: a bucket
    // floor would report up to 3 % less than the worst sample actually seen.
    if (rank == n - 1) {
        return max();
    }
    u64 seen = 0;
    for (usize bucket = 0; bucket < kBucketCount; ++bucket) {
        seen += buckets_[bucket].load(std::memory_order_relaxed);
        if (seen > rank) {
            return std::min(bucket_floor(bucket), max());
        }
    }
    return max();
}

ScopedLatency::~ScopedLatency() {
    into_->record(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - started_)
                      .count());
}

// ── Clocks ───────────────────────────────────────────────────────────────────

i64 monotonic_micros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

i64 thread_cpu_micros() noexcept {
#if defined(_WIN32)
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) == 0) {
        return 0;
    }
    const auto to_u64 = [](FILETIME t) {
        return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<i64>((to_u64(kernel) + to_u64(user)) / 10);  // 100 ns units
#elif defined(__APPLE__)
    return static_cast<i64>(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1000);
#else
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return 0;
    }
    return static_cast<i64>(now.tv_sec) * 1'000'000 + static_cast<i64>(now.tv_nsec) / 1000;
#endif
}

// ── TickProfile ──────────────────────────────────────────────────────────────

std::string_view to_string(TickPhase phase) noexcept {
    switch (phase) {
        case TickPhase::Other: return "other";
        case TickPhase::NetworkInput: return "network input";
        case TickPhase::ChunkPublish: return "chunk publish";
        case TickPhase::SpawnArea: return "spawn area";
        case TickPhase::ChunkRequests: return "chunk requests";
        case TickPhase::ChunkEviction: return "chunk eviction";
        case TickPhase::WorldClock: return "world clock";
        case TickPhase::Commands: return "commands";
        case TickPhase::ScheduledTicks: return "scheduled ticks";
        case TickPhase::Relight: return "relight";
        case TickPhase::RandomTicks: return "random ticks";
        case TickPhase::Containers: return "containers";
        case TickPhase::NaturalSpawning: return "natural spawning";
        case TickPhase::Entities: return "entities";
        case TickPhase::GroundItems: return "ground items";
        case TickPhase::Players: return "players";
        case TickPhase::Digs: return "digs";
        case TickPhase::Autosave: return "autosave";
        case TickPhase::ChunkSend: return "chunk send";
        case TickPhase::Screens: return "screens";
        case TickPhase::Count: break;
    }
    return "unnamed";
}

TickProfile::TickProfile() noexcept : at_start_{process_counters()} {}

TickProfile::ProcessCounters TickProfile::process_counters() noexcept {
    ProcessCounters out;
#if !defined(_WIN32)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        out.involuntary_switches = usage.ru_nivcsw;
        out.voluntary_switches   = usage.ru_nvcsw;
        out.major_faults         = usage.ru_majflt;
        out.minor_faults         = usage.ru_minflt;
    }
#endif
    return out;
}

void TickProfile::begin_tick() noexcept {
    wall_this_tick_.fill(0);
    cpu_this_tick_.fill(0);
    ran_this_tick_.fill(false);
    tick_wall_start_ = monotonic_micros();
    tick_cpu_start_  = thread_cpu_micros();
    mark_wall_       = tick_wall_start_;
    mark_cpu_        = tick_cpu_start_;
    current_         = TickPhase::Other;
    ran_this_tick_[static_cast<usize>(TickPhase::Other)] = true;
    open_            = true;
    if (first_wall_ < 0) {
        first_wall_ = tick_wall_start_;
    }
}

void TickProfile::close_phase(i64 wall_now, i64 cpu_now) noexcept {
    const auto index = static_cast<usize>(current_);
    wall_this_tick_[index] += wall_now - mark_wall_;
    cpu_this_tick_[index] += cpu_now - mark_cpu_;
    mark_wall_ = wall_now;
    mark_cpu_  = cpu_now;
}

TickPhase TickProfile::enter(TickPhase phase) noexcept {
    const TickPhase previous = current_;
    if (!open_ || phase == TickPhase::Count) {
        return previous;
    }
    close_phase(monotonic_micros(), thread_cpu_micros());
    current_                                    = phase;
    ran_this_tick_[static_cast<usize>(phase)] = true;
    return previous;
}

void TickProfile::end_tick() noexcept {
    if (!open_) {
        return;
    }
    const i64 wall_now = monotonic_micros();
    const i64 cpu_now  = thread_cpu_micros();
    close_phase(wall_now, cpu_now);
    open_ = false;

    const i64 elapsed = wall_now - tick_wall_start_;
    tick_.record(elapsed);
    tick_cpu_total_ += cpu_now - tick_cpu_start_;
    last_wall_ = wall_now;
    ++iterations_;

    usize heaviest = 0;
    for (usize i = 0; i < kPhaseCount; ++i) {
        if (!ran_this_tick_[i]) {
            continue;
        }
        phases_[i].record(wall_this_tick_[i]);
        cpu_total_[i] += cpu_this_tick_[i];
        if (wall_this_tick_[i] > wall_this_tick_[heaviest]) {
            heaviest = i;
        }
    }
    if (elapsed > LatencyHistogram::kBudgetMicros) {
        ++slow_ticks_;
        ++heaviest_in_slow_[heaviest];
    }
}

std::vector<std::string> TickProfile::report(i64 clock_ticks) const {
    std::vector<std::string> lines;
    if (iterations_ == 0) {
        return lines;
    }

    const f64 seconds =
        first_wall_ >= 0 ? static_cast<f64>(last_wall_ - first_wall_) / 1'000'000.0 : 0.0;
    lines.push_back(fmt::format(
        "tick profile: {} loop iterations, {} clock ticks, {:.1f} s: {:.2f} iterations/s "
        "(the world steps once per iteration; 20 is on time)",
        iterations_, clock_ticks, seconds,
        seconds > 0.0 ? static_cast<f64>(iterations_) / seconds : 0.0));

    const auto cpu_share = [](i64 cpu, i64 wall) {
        return wall <= 0 ? 100.0 : 100.0 * static_cast<f64>(cpu) / static_cast<f64>(wall);
    };
    lines.push_back(fmt::format(
        "tick profile: whole tick p50 {} us, p90 {} us, p99 {} us, max {} us, mean {} us, "
        "{} over 50 ms, tick thread on cpu {:.0f}% of wall",
        tick_.percentile(0.50), tick_.percentile(0.90), tick_.percentile(0.99), tick_.max(),
        tick_.mean(), tick_.over_budget(), cpu_share(tick_cpu_total_, tick_.total())));

    // Heaviest first: the line that explains the run is the one read.
    std::array<usize, kPhaseCount> order{};
    std::iota(order.begin(), order.end(), usize{0});
    std::ranges::sort(order, [&](usize a, usize b) {
        return phases_[a].total() > phases_[b].total();
    });
    for (const usize i : order) {
        const LatencyHistogram& h = phases_[i];
        if (h.count() == 0) {
            continue;
        }
        lines.push_back(fmt::format(
            "tick phase {}: {} runs, p50 {} us, p90 {} us, p99 {} us, max {} us, mean {} us, "
            "total {} ms, cpu {:.0f}%, {} over 50 ms, heaviest in {} of {} slow ticks",
            to_string(static_cast<TickPhase>(i)), h.count(), h.percentile(0.50),
            h.percentile(0.90), h.percentile(0.99), h.max(), h.mean(), h.total() / 1000,
            cpu_share(cpu_total_[i], h.total()), h.over_budget(), heaviest_in_slow_[i],
            slow_ticks_));
    }

    const auto network = [&](std::string_view what, const LatencyHistogram& h) {
        if (h.count() == 0) {
            return;
        }
        lines.push_back(fmt::format(
            "network {}: {} samples, p50 {} us, p90 {} us, p99 {} us, max {} us, mean {} us, "
            "{} over 50 ms",
            what, h.count(), h.percentile(0.50), h.percentile(0.90), h.percentile(0.99), h.max(),
            h.mean(), h.over_budget()));
    };
    network("packet", network_packet);
    network("player lock wait", network_lock_wait);
    network("block edit", block_edit);
    network("queue wait", network_queue_wait);  // ── concurrency ──

#if !defined(_WIN32)
    const ProcessCounters now = process_counters();
    lines.push_back(fmt::format(
        "process: {} involuntary and {} voluntary context switches, {} major and {} minor page "
        "faults since start",
        now.involuntary_switches - at_start_.involuntary_switches,
        now.voluntary_switches - at_start_.voluntary_switches,
        now.major_faults - at_start_.major_faults, now.minor_faults - at_start_.minor_faults));
#else
    lines.emplace_back("process: context switches and page faults are not measured on Windows");
#endif
    return lines;
}

}  // namespace ov::server
