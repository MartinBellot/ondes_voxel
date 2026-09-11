// The cracks the other players see while someone digs.
//
// Measured on a real 1.20.1 server (scripts/capture_destroy_stage.py,
// docs/provenance/cassage-bloc.md), with a second player four blocks away:
//
//   * the stage is floor(count * 10) of the *server's* count — rate times the
//     ticks since the start, the start's own tick included — and it is sent
//     each time it changes, 0 first, past 9 when the claim is late;
//   * the digger never receives its own;
//   * an abort, and a claim the server believes, are followed by -1;
//   * a claim made too early, which the server then finishes on its own clock,
//     ends on the stage the count reached (10) and **no** -1: the block is
//     gone, and so is the reason to clear it.
//
// Pure state and one decision per tick; the server does the sending.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/breaking.hpp"

#include <optional>

namespace ov::server {

/// How far Set Block Destroy Stage travels. Ours, not measured: the archive
/// says only "a certain radius", and the probe stood four blocks away.
inline constexpr f64 kDestroyStageRadius = 32.0;

/// What the others were last told about one player's dig.
struct DestroyStageState {
    i8                sent{-1};
    net::WirePosition where{};
};

/// The packet to send this tick, if any.
///
/// `counting` is true while the server times a block for this player, whether
/// it waits for the claim or runs the delayed clock; `count` is how much of it
/// is broken by now.
[[nodiscard]] std::optional<net::BlockDestroyStage> next_destroy_stage(
    DestroyStageState& state, i32 entity_id, bool counting, net::WirePosition where,
    f32 count) noexcept;

/// The server broke the block on its own clock: nothing more is sent.
void forget_destroy_stage(DestroyStageState& state) noexcept;

}  // namespace ov::server
