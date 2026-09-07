// Ondes VOXEL dedicated server.
//
// At M0 this ticks an empty world at 20 Hz and does nothing else. That is
// deliberately the first runnable artefact: the tick loop, its scheduling class
// and its behaviour under overload are the foundation everything else stands
// on, and they are far easier to get right on an empty world than on a full one.

#define OV_LOG_CATEGORY "server"

#include "ov/base/alloc_scope.hpp"
#include "ov/base/assert.hpp"
#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"
#include "ov/base/time.hpp"
#include "ov/math/block_pos.hpp"

#include <fmt/format.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int) noexcept {
    // Only async-signal-safe work here: flip a flag and let the tick loop exit
    // on its own so the world is saved rather than truncated.
    g_stop_requested.store(true, std::memory_order_relaxed);
}

struct Options {
    ov::LogLevel log_level = ov::LogLevel::Info;
    bool         show_help = false;
    ov::i64      run_ticks = -1;  // -1 runs until interrupted
};

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg.starts_with("--log-level=")) {
            options.log_level = ov::parse_log_level(arg.substr(12));
        } else if (arg.starts_with("--ticks=")) {
            // Bounded runs make the server usable from tests and CI without a
            // watchdog around it. from_chars, not strtoll: a string_view is not
            // null-terminated and strtoll would read past the end of it.
            const std::string_view value  = arg.substr(8);
            ov::i64                parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec == std::errc{} && ptr == value.data() + value.size() && parsed >= 0) {
                options.run_ticks = parsed;
            } else {
                OV_LOG_WARN("invalid --ticks value '{}', ignoring", value);
            }
        } else {
            OV_LOG_WARN("unknown argument '{}' (try --help)", arg);
        }
    }
    return options;
}

void print_help() {
    fmt::print(
        "Ondes VOXEL dedicated server\n"
        "\n"
        "  --log-level=<trace|debug|info|warn|error|off>   verbosity (default: info)\n"
        "  --ticks=<n>                                     stop after n ticks\n"
        "  --help, -h                                      this message\n"
        "\n"
        "Not an official Minecraft product. Not approved by or associated with Mojang.\n");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ov;

    const Options options = parse_args(argc, argv);
    if (options.show_help) {
        print_help();
        return 0;
    }

    set_log_level(options.log_level);

    // First statement on the thread that must hold 20 Hz. Without this, macOS
    // migrates it to an efficiency core under load and TPS quietly drops to the
    // mid-teens with nothing in a profile to explain it.
    set_thread_role("ov-tick", ThreadRole::Tick);

    OV_LOG_INFO("Ondes VOXEL dedicated server — target Minecraft 1.20.1 (protocol 763)");
    OV_LOG_INFO("workers available: {}", recommended_worker_count());

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    TickClock clock;
    i64       behind_events = 0;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const i32 ticks = clock.advance();

        for (i32 i = 0; i < ticks; ++i) {
            // The world tick lives here. Everything inside must be
            // deterministic, and must not allocate once running: in debug
            // builds this guard aborts on the first allocation, naming it.
            const NoAllocScope no_alloc{"server tick"};
        }

        if (clock.is_behind()) {
            ++behind_events;
            OV_LOG_WARN(
                "can't keep up — is the server overloaded? (running behind, dropped ticks)");
        }

        if (options.run_ticks >= 0 && clock.tick_count() >= options.run_ticks) {
            OV_LOG_INFO("reached --ticks={}, stopping", options.run_ticks);
            break;
        }

        // Sleep until the next tick is due rather than spinning. A spinning
        // tick thread on a laptop is a battery and thermal problem, and on a
        // shared host it steals time from the workers.
        if (const Duration idle = clock.time_until_next_tick(); idle > Duration::zero()) {
            std::this_thread::sleep_for(idle);
        }
    }

    if constexpr (kAllocationTrackingEnabled) {
        const auto stats = allocation_stats();
        OV_LOG_INFO("allocations: {} ({} bytes), tick violations: {}", stats.allocations,
                    stats.bytes, stats.violations);
    }
    OV_LOG_INFO("stopped after {} ticks ({} overload events)", clock.tick_count(), behind_events);
    return 0;
}
