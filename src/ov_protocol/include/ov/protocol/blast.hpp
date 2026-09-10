// What a client is told about explosions, primed TNT, creepers and falling
// blocks.
//
// Every number here was read off a real 1.20.1 server by
// scripts/measure_tnt_gravity.py capture: a probe client joined the vanilla
// jar, the console summoned or primed the thing next to it, and the payloads
// were written down. Two of them disagree with the archived wiki page that
// CLAUDE.md points to, and both disagreements are recorded where they bite:
//
//   * **Explosion is 0x1D, not 0x1E.** The packet was found by its payload —
//     the first packet whose three leading doubles were the charge's centre —
//     rather than by trusting an id, and it came back as 0x1D.
//   * **Its record list carries the air cells.** A charge on untouched
//     superflat ground sends 676 records on average (32 charges, sd 11) while
//     breaking under a hundred blocks: the list is every cell a ray reached
//     with energy left, not every block destroyed.
//
// The metadata indices are measured the way entity.hpp measured its own: one
// NBT field set on an otherwise identical entity, and the index that moved is
// the answer. Nothing below is from a summary.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace ov::net {

namespace clientbound {
/// Explosion. Measured 0x1D; the archived wiki says 0x1E.
inline constexpr i32 kExplosion = 0x1D;
}  // namespace clientbound

namespace metadata {

/// A primed TNT's fuse, a VarInt, in ticks. Measured: `summon tnt {Fuse:37s}`
/// arrives with index 8 = 37, and the server then re-sends it **every tick** as
/// it counts down — 36, 35, 34 … — which is what makes the client's flashing
/// keep time with the server's fuse.
///
/// Its default is 80, and a default is not sent: a TNT primed by redstone
/// arrives with no index 8 at all and its first update is 79.
inline constexpr u8 kTntFuse = 8;

/// The block a falling block left, as a BlockPos. Measured on a sand block set
/// in mid-air: index 8, type 10, and x equal to the block it fell from.
inline constexpr u8 kFallingBlockStart = 8;

/// A creeper's swell direction, a VarInt: -1 idle, 1 swelling. Measured on an
/// `ignited:1b` creeper, which sends index 16 = 1 on the tick it starts.
inline constexpr u8 kCreeperSwellDir = 16;
/// Charged. Measured with `powered:1b`: index 17, a boolean.
inline constexpr u8 kCreeperPowered = 17;
/// Lit by flint and steel. Measured with `ignited:1b`: index 18, a boolean.
inline constexpr u8 kCreeperIgnited = 18;

}  // namespace metadata

/// A primed TNT's fuse when the metadata carries none. Sending it would be
/// harmless and is not what the game does.
inline constexpr i32 kDefaultTntFuse = 80;

/// One explosion, as the packet carries it.
struct Explosion {
    /// The centre. For a primed TNT that is its position raised by a sixteenth
    /// of its height — captured as y = -59.93874999880791 for a charge standing
    /// at -60, which is `0.98F * 0.0625` widened from a float.
    f64 x{0.0};
    f64 y{0.0};
    f64 z{0.0};
    f32 radius{0.0F};

    /// Offsets from the floor of the centre, one signed byte per axis.
    std::vector<std::array<i8, 3>> records;

    /// Added to the receiving player's own velocity. Per player: the same
    /// explosion sends each viewer a different vector, and zero to one it did
    /// not reach.
    f32 knockback_x{0.0F};
    f32 knockback_y{0.0F};
    f32 knockback_z{0.0F};

    /// Add a cell by absolute position. False, and nothing added, when the cell
    /// is more than a signed byte away from the centre — a packet that wrapped
    /// it would clear a block on the other side of the crater.
    bool add_record(BlockPos cell);
};

[[nodiscard]] std::vector<u8> encode_explosion(const Explosion& explosion);

/// The client's side of it: what our own client and the round-trip test read.
[[nodiscard]] std::optional<Explosion> parse_explosion(std::span<const u8> payload);

}  // namespace ov::net
