// Frustum culling: deciding which sections are worth drawing at all.
//
// At a render distance of twelve chunks the world holds a few thousand
// sections and the camera can see perhaps a fifth of them. Submitting the rest
// costs a draw call each and produces nothing, and it is the cheapest large
// saving there is — one plane test against a box, on the CPU, before anything
// is bound.
//
// The GPU compute cull the plan mentions comes after this is measured and found
// wanting, not before.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/camera.hpp"

#include <array>

namespace ov::render {

/// The six planes of a view frustum, each as ax + by + cz + d, with the normal
/// pointing inwards so that a positive value means "in front of this plane".
class Frustum {
public:
    /// Extracted from a view-projection matrix by adding and subtracting rows.
    ///
    /// Works for any projection the matrix can express, which matters because
    /// this one is Vulkan's 0..1 depth convention rather than OpenGL's -1..1 —
    /// the near plane is the row on its own, not the sum of two.
    [[nodiscard]] static Frustum from_view_projection(const Mat4& matrix) noexcept;

    /// Is any part of the axis-aligned box inside?
    ///
    /// Conservative: it can say yes for a box that is outside but straddles two
    /// planes' half-spaces. That costs a wasted draw and never a missing one,
    /// which is the right way round for a test that runs thousands of times a
    /// frame.
    [[nodiscard]] bool intersects(Vec3f minimum, Vec3f maximum) const noexcept;

    /// The six planes, for tests.
    [[nodiscard]] const std::array<std::array<f32, 4>, 6>& planes() const noexcept {
        return planes_;
    }

private:
    std::array<std::array<f32, 4>, 6> planes_{};
};

}  // namespace ov::render
