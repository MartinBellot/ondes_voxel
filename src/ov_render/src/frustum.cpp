#include "ov/render/frustum.hpp"

#include <cmath>

namespace ov::render {

namespace {

/// Column-major, so element (row, column) is m[column * 4 + row].
[[nodiscard]] f32 at(const Mat4& matrix, usize row, usize column) noexcept {
    return matrix.m[column * 4 + row];
}

void normalise(std::array<f32, 4>& plane) noexcept {
    const f32 length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (length > 0.0F) {
        const f32 inverse = 1.0F / length;
        plane[0] *= inverse;
        plane[1] *= inverse;
        plane[2] *= inverse;
        plane[3] *= inverse;
    }
}

}  // namespace

Frustum Frustum::from_view_projection(const Mat4& matrix) noexcept {
    Frustum frustum;

    const auto row = [&matrix](usize index) {
        return std::array<f32, 4>{at(matrix, index, 0), at(matrix, index, 1), at(matrix, index, 2),
                                  at(matrix, index, 3)};
    };
    const auto combine = [](const std::array<f32, 4>& a, const std::array<f32, 4>& b, f32 sign) {
        return std::array<f32, 4>{a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2],
                                  a[3] + sign * b[3]};
    };

    const auto row0 = row(0);
    const auto row1 = row(1);
    const auto row2 = row(2);
    const auto row3 = row(3);

    frustum.planes_[0] = combine(row3, row0, 1.0F);   // left
    frustum.planes_[1] = combine(row3, row0, -1.0F);  // right
    frustum.planes_[2] = combine(row3, row1, 1.0F);   // bottom
    frustum.planes_[3] = combine(row3, row1, -1.0F);  // top
    // Vulkan clips depth to 0..1, not -1..1, so the near plane is row 2 alone
    // rather than w + z. Using the OpenGL form here culls everything closer
    // than halfway to the far plane, which looks like the world disappearing
    // when you walk towards it.
    frustum.planes_[4] = row2;                        // near
    frustum.planes_[5] = combine(row3, row2, -1.0F);  // far

    for (auto& plane : frustum.planes_) {
        normalise(plane);
    }
    return frustum;
}

bool Frustum::intersects(Vec3f minimum, Vec3f maximum) const noexcept {
    for (const auto& plane : planes_) {
        // The corner of the box furthest along the plane's normal. If even that
        // one is behind, every corner is, and the box is out.
        const Vec3f positive{plane[0] >= 0.0F ? maximum.x : minimum.x,
                             plane[1] >= 0.0F ? maximum.y : minimum.y,
                             plane[2] >= 0.0F ? maximum.z : minimum.z};

        const f32 distance =
            plane[0] * positive.x + plane[1] * positive.y + plane[2] * positive.z + plane[3];
        if (distance < 0.0F) {
            return false;
        }
    }
    return true;
}

}  // namespace ov::render
