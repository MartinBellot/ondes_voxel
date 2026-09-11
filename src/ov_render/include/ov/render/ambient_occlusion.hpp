// Vanilla's smooth lighting: a brightness and a light value per corner of a
// face, from the four blocks that share that corner.
//
// Each corner of a face touches four blocks in the layer the face looks into:
// the one straight in front (the centre), the two beside the corner along the
// face's own axes, and the one diagonally across. The corner takes
//
//   brightness = mean of the four blocks' shade brightness
//   light      = sum of the four blocks' light levels, in quarter levels
//
// with two substitutions:
//
//   * a diagonal is hidden only when the blocks one step *beyond* both sides,
//     along the face's normal, are opaque — an inside corner between two
//     single blocks still sees its diagonal (0.6 with it empty, 0.4 with it
//     full), a corner walled two high does not — and a hidden diagonal is
//     replaced by the centre;
//   * a block that carries no light at all (the inside of an opaque block)
//     lends the centre's instead, so an occluder darkens a corner once, through
//     its shade, and not a second time through the average.
//
// A block's shade brightness is 0.2 when its collision shape is a full cube and
// 1.0 otherwise. Mean of four such values: 1.0, 0.8, 0.6 or 0.4 — four steps,
// not the 0.2 .. 1.0 ramp this file used to carry, whose inside corner was half
// as bright as the game's.
//
// None of this is guessed at the level of a number. The values are the game's
// own: scripts/render_parity_oracle.java runs vanilla's AmbientOcclusionFace on
// the faces of a test platform and prints what it computed, and
// test_ambient_occlusion.cpp holds those answers (see
// docs/provenance/rendu-parite.md).
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

namespace ov::render {

/// Shade brightness of a block whose collision shape is a full cube. Anything
/// else — air, a torch, a slab, a fence — is 1.0.
inline constexpr f32 kOccluderShade = 0.2F;

/// What smooth lighting needs to know about one of the four blocks.
struct AoSample {
    /// 0.2 for a full-cube collision shape, 1.0 otherwise.
    f32 shade{1.0F};
    /// Stored light levels, 0..15.
    u8 sky{0};
    u8 block{0};
};

/// One corner of a face, before it is spread over the vertices.
struct CornerLighting {
    f32 brightness{1.0F};
    /// Sum of four levels, 0..60. Kept as the sum rather than rounded to a
    /// level: the lightmap is sampled between texels, and the quarter steps are
    /// what make light fall off smoothly across a face.
    u8 sky_quarters{60};
    u8 block_quarters{0};
};

/// `hidden_by`: when the blocks one step beyond both sides along the face's
/// normal block sight, the sample that stands in for the diagonal; null when
/// the diagonal is seen. Measured on the game's own AmbientOcclusionFace: an L
/// of two single blocks keeps its diagonal, a room's corner does not, and the
/// stand-in is the side *opposite* the corner along the face's first axis —
/// a block put there took the room corner from 0.6 / 10.25 to 0.4 / 10.00
/// (docs/provenance/rendu-parite.md).
[[nodiscard]] constexpr CornerLighting smooth_corner(const AoSample& centre, const AoSample& side1,
                                                     const AoSample& side2, const AoSample& diagonal,
                                                     const AoSample* hidden_by = nullptr) noexcept {
    const AoSample& corner = hidden_by != nullptr ? *hidden_by : diagonal;

    // "No light at all" is the test, on both channels together: a dark cave
    // cell lends the centre's light exactly as a stone block does.
    const auto lit = [&centre](const AoSample& sample) {
        return (sample.sky == 0 && sample.block == 0) ? centre : sample;
    };
    const AoSample a = lit(side1);
    const AoSample b = lit(side2);
    const AoSample c = lit(corner);

    CornerLighting out;
    out.brightness     = (side1.shade + side2.shade + corner.shade + centre.shade) * 0.25F;
    out.sky_quarters   = static_cast<u8>(centre.sky + a.sky + b.sky + c.sky);
    out.block_quarters = static_cast<u8>(centre.block + a.block + b.block + c.block);
    return out;
}

/// Directional shading, the flat multiplier vanilla applies per face on top
/// of smooth lighting. Printed by the game (ClientLevel.getShade) for the
/// overworld: up 1.0, down 0.5, north and south 0.8, east and west 0.6.
[[nodiscard]] constexpr f32 face_shade(Direction direction) noexcept {
    switch (direction) {
        case Direction::Down: return 0.5F;
        case Direction::Up: return 1.0F;
        case Direction::North:
        case Direction::South: return 0.8F;
        case Direction::West:
        case Direction::East: return 0.6F;
    }
    return 1.0F;
}

}  // namespace ov::render
