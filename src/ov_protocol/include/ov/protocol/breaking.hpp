// The packets around breaking a block that the dig itself does not carry: the
// cracks the others see, the arm that swings, the dig with its sequence.
//
// Identified by content on a real 1.20.1 server (scripts/capture_destroy_stage.py,
// docs/provenance/cassage-bloc.md) and not by the archived page, which gives
// other numbers for all three clientbound ids around it:
//
//   * Set Block Destroy Stage is **0x07**: the digger's entity id (VarInt), the
//     position, one signed byte. It goes to the *other* players only — the
//     digger received none of 14 on a stone held for 200 ticks.
//   * The byte is floor(server count * 10), sent each time it changes: 0 on the
//     first tick, past 9 when the client is slow to claim (up to 14 seen), and
//     -1 when the dig is aborted or finished.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/interaction.hpp"
#include "ov/protocol/play.hpp"

#include <optional>
#include <span>
#include <vector>

namespace ov::net {

namespace clientbound {
/// Set Block Destroy Stage.
inline constexpr i32 kSetBlockDestroyStage = 0x07;
}  // namespace clientbound

/// One Set Block Destroy Stage.
struct BlockDestroyStage {
    /// Whose dig it is. A client keeps one crack per breaker: a second dig by
    /// the same player moves the crack rather than adding one.
    i32          entity_id{0};
    WirePosition position{};
    /// As sent. Only 0..9 is drawn; anything else removes the breaker's crack.
    i8 stage{-1};

    friend bool operator==(const BlockDestroyStage&, const BlockDestroyStage&) noexcept = default;
};

[[nodiscard]] std::vector<u8> encode_block_destroy_stage(const BlockDestroyStage& packet);
[[nodiscard]] std::optional<BlockDestroyStage> parse_block_destroy_stage(
    std::span<const u8> payload);

/// Is this a stage a client draws? 0..9 is; -1, and the 10..14 a slow claim
/// runs up, remove the crack.
[[nodiscard]] constexpr bool is_drawn_stage(i32 stage) noexcept {
    return stage >= 0 && stage <= 9;
}

/// The stage the server sends for a count of `progress` blocks: floor(x * 10),
/// saturated to what a signed byte carries.
[[nodiscard]] i8 server_destroy_stage(f32 progress) noexcept;

/// Player Action (serverbound 0x1D), statuses 0 to 2, with its sequence.
///
/// The sequence is what the server acknowledges (Acknowledge Block Change,
/// 0x06); vanilla increments it on every action that can change a block.
[[nodiscard]] std::vector<u8> encode_player_action(i32 status, WirePosition position, u8 face,
                                                   i32 sequence);

/// Swing Arm (serverbound 0x2F): the hand is the whole packet.
[[nodiscard]] std::vector<u8> encode_swing_arm(Hand hand);

}  // namespace ov::net
