// The camera, and the 4x4 matrices it produces.
//
// Layering note: the plan files matrices under ov_math, where they belong and
// where they will move. They are here for now because ov_math is being worked
// on in parallel and a renderer needs a view-projection today; nothing outside
// this module depends on them, so the move will be a rename.
//
// The angle convention is Minecraft's, not a graphics textbook's, and that is
// deliberate: yaw and pitch arrive over the wire in exactly this form, and a
// client that converts them at the boundary will get the conversion wrong in
// one direction and not notice until it walks backwards.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <array>

namespace ov::render {

/// Column-major, the layout GLSL expects, so it uploads with a memcpy.
struct Mat4 {
    std::array<f32, 16> m{};

    [[nodiscard]] static Mat4 identity() noexcept;

    [[nodiscard]] Mat4 operator*(const Mat4& other) const noexcept;
};

/// Right-handed perspective for Vulkan's 0..1 depth range.
///
/// No Y flip here. The viewport does it — CommandList::set_viewport passes a
/// negative height — which keeps the matrix a textbook one and stops the flip
/// being applied twice by two people who each thought the other had not.
[[nodiscard]] Mat4 perspective(f32 vertical_fov_radians, f32 aspect, f32 near_plane,
                               f32 far_plane) noexcept;

[[nodiscard]] Mat4 look_along(Vec3f eye, Vec3f forward, Vec3f up) noexcept;

[[nodiscard]] Mat4 translation(Vec3f offset) noexcept;

/// A free camera in Minecraft's angle convention.
///
/// yaw 0 looks towards +Z (south) and increases clockwise seen from above, so
/// yaw 90 looks towards -X (west). pitch is positive downwards. Both are what
/// the protocol sends.
class Camera {
public:
    Vec3f position{0.0F, 0.0F, 0.0F};
    f32   yaw_degrees{0.0F};
    f32   pitch_degrees{0.0F};
    f32   vertical_fov_degrees{70.0F};
    f32   near_plane{0.05F};
    f32   far_plane{512.0F};

    [[nodiscard]] Vec3f forward() const noexcept;
    [[nodiscard]] Vec3f right() const noexcept;

    /// Turn by a mouse delta in pixels. Pitch is clamped just short of
    /// vertical, because at exactly vertical the up vector and the forward
    /// vector are parallel and the view matrix collapses.
    void turn(f32 delta_x, f32 delta_y, f32 sensitivity = 0.15F) noexcept;

    [[nodiscard]] Mat4 view() const noexcept;
    [[nodiscard]] Mat4 projection(f32 aspect) const noexcept;
    [[nodiscard]] Mat4 view_projection(f32 aspect) const noexcept;
};

}  // namespace ov::render
