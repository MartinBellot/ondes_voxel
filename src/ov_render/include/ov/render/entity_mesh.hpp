// Turning a posed model into triangles, on the CPU, in world space.
//
// Why on the CPU. The alternative is a matrix per bone in a storage buffer and
// a vertex shader that walks the hierarchy, which is faster in principle and
// untestable in practice. Here the whole geometry pipeline — the bone
// hierarchy, the box nets, the walk cycle, the reflection — is a function from
// numbers to numbers, so `test_ov_render` can assert that a zombie's rendered
// box is 0.6 by 1.95 blocks instead of a human deciding a screenshot looks
// about right. A hundred mobs cost about half a megabyte of vertex writes per
// frame, and § "Ce que les entités coûtent" of the provenance document says
// what that measured.
//
// ── The reflection ──────────────────────────────────────────────────────────
//
// Model space has −Z forward and +X to the entity's left. Minecraft's world has
// yaw 0 looking towards +Z, and an entity at yaw 0 faces +Z. The map that takes
// one to the other is
//
//     world = feet + ( mx·cos y + mz·sin y ,  my ,  mx·sin y − mz·cos y ) / 16
//
// whose determinant is **−1**. It is a reflection, not a rotation — the same
// one vanilla writes as `scale(-1, -1, 1)` on a y-down model. So a quad wound
// counter-clockwise seen from outside in model space comes out clockwise in the
// world, and this file emits every face in the order that lands it the right
// way round. `test_entity_mesh.cpp` asserts it with a cross product rather than
// with a screenshot, because a back-facing mob is invisible and an invisible
// mob is exactly what this whole module exists to fix.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/entity_model.hpp"
#include "ov/render/entity_pose.hpp"

#include <array>
#include <span>
#include <vector>

namespace ov::render {

/// 24 bytes: position, texture coordinate, colour.
///
/// No normal and no light channel. The shade of a face is folded into the
/// colour here, and so is the lightmap sample for the entity's block, because
/// both are constant across a face and a per-vertex byte is cheaper than a
/// second texture bound in the fragment shader.
struct EntityVertex {
    f32               x{0.0F};
    f32               y{0.0F};
    f32               z{0.0F};
    f32               u{0.0F};
    f32               v{0.0F};
    std::array<u8, 4> colour{};
};

static_assert(sizeof(EntityVertex) == 24, "the entity vertex must stay 24 bytes");

/// Where and how one entity is drawn.
struct EntityPlacement {
    /// The entity's feet, in world coordinates.
    Vec3f position{};
    /// The body's yaw in degrees, Minecraft's convention: 0 faces +Z.
    f32 body_yaw{0.0F};
    /// Uniform scale about the feet. Babies are 0.5 in vanilla; nothing in this
    /// build sends the baby flag yet, so it is always 1.
    f32 scale{1.0F};
    /// Multiplied into every vertex, 0xAARRGGBB, sRGB. Carries the block light
    /// and any per-entity tint; a hurt flash would go here.
    u32 tint{0xFFFFFFFFU};
};

/// The flat shade vanilla gives a face by which way it points.
///
/// The same five numbers the terrain uses. Vanilla lights an entity with two
/// directional lights instead, which is a different look — that difference is
/// named in the provenance document rather than papered over.
[[nodiscard]] f32 face_shade(Vec3f world_normal) noexcept;

/// Append one entity's triangles to `out`.
///
/// `poses` must have one entry per bone; pose_model() produces exactly that.
/// Returns the number of quads appended, which is what the caller reports as
/// the frame's GPU-side cost.
u32 emit_entity(const EntityModel& model, std::span<const BonePose> poses,
                const EntityPlacement& placement, std::vector<EntityVertex>& out);

/// The world-space box the posed model occupies.
///
/// This is the number the parity check compares against the type's measured
/// collision box: a model that is too tall or off-centre shows up here before
/// it shows up on a screen.
void entity_bounds(const EntityModel& model, std::span<const BonePose> poses,
                   const EntityPlacement& placement, Vec3f& min, Vec3f& max);

/// A quad in world space, for anything that is not a box: a dropped item's
/// sprite, a sign's text, a nameplate.
///
/// Corners are given in the order bottom-left, bottom-right, top-right,
/// top-left as seen from the front; the texture coordinates match them. Emitted
/// double-sided is the caller's business — this appends one facing.
void emit_quad(const std::array<Vec3f, 4>& corners, const std::array<f32, 4>& u,
               const std::array<f32, 4>& v, u32 tint, std::vector<EntityVertex>& out);

}  // namespace ov::render
