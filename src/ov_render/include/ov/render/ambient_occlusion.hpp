// Vanilla's smooth lighting: one ambient occlusion level and one light value
// per vertex, from the three blocks that share that corner.
//
// The rule is small and the consequences are not. Each corner of a face has
// three neighbours: the two blocks beside it along the face, and the one
// diagonally across. Ambient occlusion counts how many of them are solid — and
// the special case, that two solid sides darken the corner fully whatever the
// diagonal does, is what produces the crease in an inside corner instead of a
// smooth ramp. Get that special case wrong and every staircase looks inflated.
//
// Light works the same way but averages instead of counting, which is why a
// torch in a corner spills onto the neighbouring faces rather than lighting one
// square.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <array>

namespace ov::render {

/// The three neighbours of one corner of a face.
struct CornerNeighbours {
    /// The two blocks beside the corner, in the face's own plane.
    bool side1{false};
    bool side2{false};
    /// The block diagonally across the corner.
    bool corner{false};
};

/// Ambient occlusion level for one vertex: 3 is open, 0 is fully occluded.
///
/// Two solid sides give 0 regardless of the corner, because the corner block
/// cannot be seen past them. That single line is the difference between a
/// convincing inside corner and a bloated one.
[[nodiscard]] constexpr u8 ao_level(const CornerNeighbours& neighbours) noexcept {
    if (neighbours.side1 && neighbours.side2) {
        return 0;
    }
    const auto occluders = static_cast<u8>(neighbours.side1) + static_cast<u8>(neighbours.side2) +
                           static_cast<u8>(neighbours.corner);
    return static_cast<u8>(3 - occluders);
}

/// The brightness multiplier an ambient occlusion level stands for.
///
/// Four evenly spaced steps ending at 1.0, which is what vanilla's smooth
/// lighting produces for a face with no neighbours.
[[nodiscard]] constexpr f32 ao_brightness(u8 level) noexcept {
    constexpr std::array<f32, 4> kSteps{0.2F, 0.4666667F, 0.7333333F, 1.0F};
    return kSteps[level > 3 ? 3 : level];
}

/// Light levels of the four blocks around a vertex: the block the face looks
/// into, and its three corner neighbours.
struct CornerLight {
    u8 self{0};
    u8 side1{0};
    u8 side2{0};
    u8 corner{0};
};

/// Vanilla's per-vertex light: the average of the four, but only over the ones
/// that can actually carry light there.
///
/// A solid neighbour has light 0 and including it would darken the vertex twice
/// — once through ambient occlusion, once through the average — so it is
/// skipped. When two sides are solid the corner is invisible and is skipped
/// too, which is the same special case as ao_level and has to agree with it.
[[nodiscard]] constexpr u8 smooth_light(const CornerLight&      light,
                                        const CornerNeighbours& neighbours) noexcept {
    u32 total = light.self;
    u32 count = 1;

    if (!neighbours.side1) {
        total += light.side1;
        ++count;
    }
    if (!neighbours.side2) {
        total += light.side2;
        ++count;
    }
    if (!neighbours.corner && !(neighbours.side1 && neighbours.side2)) {
        total += light.corner;
        ++count;
    }
    return static_cast<u8>(total / count);
}

/// Directional shading, the flat multiplier vanilla applies per face before
/// smooth lighting. Measured from the game's own appearance: the top of a
/// block is full brightness, the bottom half, north/south a fifth down and
/// east/west two fifths down.
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
