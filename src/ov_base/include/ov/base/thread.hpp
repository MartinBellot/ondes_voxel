// Thread naming and scheduling class.
//
// The macOS scheduling trap, handled here on purpose:
//
//   On Apple Silicon the scheduler will happily migrate a thread that has not
//   declared a quality-of-service class onto an efficiency core under load. For
//   the server tick thread that shows up as 14-17 TPS with no visible cause and
//   no hot spot in a profile — the work is simply running on slower cores.
//
//   Every long-lived thread must therefore call set_thread_role() as its first
//   statement. This is not an optimisation; it is a correctness requirement for
//   the 20 TPS guarantee.
#pragma once

#include "ov/base/platform.hpp"

#include <string_view>

namespace ov {

/// What a thread is for. Maps to QoS classes on macOS, nice levels and
/// scheduling hints elsewhere.
enum class ThreadRole {
    /// Frame-deadline work: rendering, input, presentation.
    Interactive,
    /// The server tick. Must hold 20 Hz; latency-critical, never on E-cores.
    Tick,
    /// Network I/O: responsive, but not on the frame deadline.
    Network,
    /// Job pool workers: worldgen, meshing, lighting, decompression.
    Worker,
    /// Blocking disk I/O.
    Io,
    /// Best effort, yields to everything.
    Background,
};

/// Declare this thread's name and role. Call as the FIRST statement of every
/// long-lived thread. `name` is truncated to the platform limit (15 characters
/// on Linux).
void set_thread_role(std::string_view name, ThreadRole role) noexcept;

/// Name only, without touching scheduling.
void set_thread_name(std::string_view name) noexcept;

/// The name previously set through this API, or an empty view.
[[nodiscard]] std::string_view current_thread_name() noexcept;

/// Number of worker threads to spawn for the job pool.
///
/// Capped at 4 regardless of core count. On unified-memory Apple Silicon the
/// scarce resource is memory bandwidth, not cores: past four workers, meshing
/// throughput goes down rather than up. Raising this cap needs a benchmark, not
/// an opinion.
[[nodiscard]] unsigned recommended_worker_count() noexcept;

}  // namespace ov
