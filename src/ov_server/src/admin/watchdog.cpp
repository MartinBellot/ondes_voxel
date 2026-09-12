#include "watchdog.hpp"

#include "java_compat.hpp"

#include "ov/base/thread.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ov::server::admin {
namespace {

[[nodiscard]] i64 steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::optional<i64> watchdog_overdue(i64 tick_started_ms, i64 now_ms, i64 max_tick_ms) noexcept {
    if (max_tick_ms <= 0) {
        return std::nullopt;
    }
    const i64 took = now_ms - tick_started_ms;
    if (took > max_tick_ms) {
        return took;
    }
    return std::nullopt;
}

std::string watchdog_message(i64 overdue_ms) {
    char line[128];
    std::snprintf(line, sizeof line, "A single server tick took %.2f seconds (should be max 0.05)",
                  static_cast<double>(overdue_ms) / 1000.0);
    return std::string{line} + "\nConsidering it to be crashed, server will forcibly shutdown.";
}

CrashReport watchdog_report(i64 overdue_ms, i64 max_tick_ms, i64 epoch_seconds,
                            i32 utc_offset_seconds, std::string_view details) {
    // "2026-09-11 18:40:12 +0200" → "2026-09-11_18.40.12", the file name's form.
    std::string stamp = format_ban_date(epoch_seconds, utc_offset_seconds).substr(0, 19);
    stamp[10]         = '_';
    stamp[13]         = '.';
    stamp[16]         = '.';
    CrashReport report;
    report.file_name = "crash-" + stamp + "-server.txt";
    char took[64];
    std::snprintf(took, sizeof took, "%.2f", static_cast<double>(overdue_ms) / 1000.0);
    report.text = "---- Minecraft Crash Report ----\n"
                  "// Ondes VOXEL, a reimplementation — not an official Minecraft product.\n"
                  "\n"
                  "Time: " +
                  format_ban_date(epoch_seconds, utc_offset_seconds) +
                  "\n"
                  "Description: Watching Server\n"
                  "\n"
                  "Error: Watchdog\n"
                  "\tA single server tick took " +
                  std::string{took} + " seconds; max-tick-time is " + std::to_string(max_tick_ms) +
                  " ms.\n"
                  "\n"
                  "-- Details --\n" +
                  std::string{details} + "\n";
    return report;
}

Watchdog::Watchdog(i64 max_tick_ms, std::function<void(i64)> on_overdue)
    : max_tick_ms_{max_tick_ms}, on_overdue_{std::move(on_overdue)}, started_ms_{steady_ms()} {
    if (max_tick_ms_ > 0) {
        thread_ = std::thread{[this] { run(); }};
    }
}

Watchdog::~Watchdog() {
    {
        const std::scoped_lock lock{mutex_};
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Watchdog::tick_started() noexcept { started_ms_.store(steady_ms(), std::memory_order_relaxed); }

void Watchdog::run() {
    set_thread_role("ov-watchdog", ThreadRole::Io);
    std::unique_lock lock{mutex_};
    while (!stop_) {
        const i64 now     = steady_ms();
        const i64 started = started_ms_.load(std::memory_order_relaxed);
        if (const auto overdue = watchdog_overdue(started, now, max_tick_ms_)) {
            lock.unlock();
            on_overdue_(*overdue);
            return;
        }
        // Look again when this tick would become overdue, at most a second
        // from now so a stop is never waited on for long.
        const i64 wait = std::clamp<i64>(started + max_tick_ms_ - now + 1, 1, 1000);
        wake_.wait_for(lock, std::chrono::milliseconds{wait});
    }
}

}  // namespace ov::server::admin
