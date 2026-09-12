// The watchdog: a tick that does not end is a crash, not a pause.
//
// `max-tick-time` in server.properties, milliseconds, 60000 by default; a
// negative value turns it off. The tick thread stamps the moment each tick
// starts; a thread of its own looks at the stamp, and when a tick has been
// running longer than the limit it writes a crash report and ends the process
// with status 1 — without saving, since the thread that owns the world is the
// one that is stuck. The words are vanilla's (docs/provenance/serveur-dedie.md):
//
//     A single server tick took 60.00 seconds (should be max 0.05)
//     Considering it to be crashed, server will forcibly shutdown.
//
// Steady time only, and nothing it reads is game state: the world stays
// deterministic whether or not the watchdog runs.
#pragma once

#include "ov/base/types.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ov::server::admin {

/// How late the tick that started at `tick_started_ms` is at `now_ms`, when it
/// is later than `max_tick_ms`; none otherwise, or when the limit is off.
[[nodiscard]] std::optional<i64> watchdog_overdue(i64 tick_started_ms, i64 now_ms,
                                                  i64 max_tick_ms) noexcept;

/// The two lines the server logs before it goes.
[[nodiscard]] std::string watchdog_message(i64 overdue_ms);

/// The crash report's text and its file name, `crash-<date>-server.txt`.
struct CrashReport {
    std::string file_name;
    std::string text;
};
[[nodiscard]] CrashReport watchdog_report(i64 overdue_ms, i64 max_tick_ms, i64 epoch_seconds,
                                          i32 utc_offset_seconds, std::string_view details);

class Watchdog {
public:
    /// `on_overdue` runs on the watchdog's thread, once; it is expected not
    /// to return (the server's writes the report and exits).
    Watchdog(i64 max_tick_ms, std::function<void(i64 overdue_ms)> on_overdue);
    ~Watchdog();

    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    /// From the tick thread, at the start of every tick.
    void tick_started() noexcept;

private:
    void run();

    i64                                 max_tick_ms_;
    std::function<void(i64)>            on_overdue_;
    std::atomic<i64>                    started_ms_;
    std::mutex                          mutex_;
    std::condition_variable             wake_;
    bool                                stop_{false};
    std::thread                         thread_;
};

}  // namespace ov::server::admin
