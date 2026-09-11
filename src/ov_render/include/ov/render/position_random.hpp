// Which of a block's weighted model alternatives the game draws at a position.
//
// A blockstate may list several models for one state — stone plain and
// mirrored, each turned 0 or 180 degrees; a grass block turned four ways — and
// the game picks one per block, from the block's position, so that a floor of
// stone does not tile. A renderer that always takes the first shows the right
// colours on the wrong texels: that was the dominant error left on every sunlit
// surface of the render parity captures (docs/provenance/rendu-parite.md).
//
// The pick: a 64-bit seed hashed from the position, a 48-bit linear
// congruential generator seeded with it (the JDK's), one 64-bit draw, its low
// 32 bits made non-negative, modulo the total weight, walked through the
// weights. Checked against the real client's captures and against its own
// per-position choice, printed by the oracle's `variants` directive.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <span>

namespace ov::render {

/// The per-position seed of a block.
[[nodiscard]] i64 position_seed(i32 x, i32 y, i32 z) noexcept;

/// The JDK's 48-bit LCG, as far as a weighted pick needs it.
class LegacyLcg {
public:
    explicit LegacyLcg(i64 seed) noexcept;
    [[nodiscard]] i32 next(u32 bits) noexcept;
    [[nodiscard]] i64 next_long() noexcept;

private:
    u64 state_;
};

/// Index into `weights` of the alternative drawn for `seed`. Weights below
/// one count as one, and an empty list gives 0.
[[nodiscard]] u32 pick_weighted(i64 seed, std::span<const i32> weights) noexcept;

/// How a plant is nudged off the centre of its block, per position, so that a
/// meadow does not stand in a grid.
enum class OffsetType : u8 { None, XZ, XYZ };

/// The nudge, in blocks: the position seed taken at y = 0, its low nibble for
/// x and the third for z, each 0..15 mapped onto ±0.25 and held within
/// `max_horizontal`; for XYZ the second nibble for y, mapped onto
/// −max_vertical..0. The game's own offsets, printed by the oracle for 22
/// grass plants, are reproduced exactly (test_position_random.cpp).
[[nodiscard]] Vec3f block_offset(i32 x, i32 z, OffsetType type, f32 max_horizontal,
                                 f32 max_vertical = 0.2F) noexcept;

}  // namespace ov::render
