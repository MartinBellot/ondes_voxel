// Turning a posed model into triangles, on the CPU, in world space.
//
// Why on the CPU. The alternative is a matrix per bone in a storage buffer and
// a vertex shader that walks the hierarchy, which is faster in principle and
// untestable in practice. Here the whole geometry pipeline — the bone
// hierarchy, the parts, the walk cycle, the reflection — is a function from
// numbers to numbers, so `test_ov_render` can assert where a zombie's arm is
// instead of a human deciding a screenshot looks about right.
//
// ── The reflection ──────────────────────────────────────────────────────────
//
// Model space has −Z forward and +X to the entity's left. Minecraft's world has
// yaw 0 looking towards +Z, and an entity at yaw 0 faces +Z. The map that takes
// one to the other is
//
//     world = feet + Ry(180° − yaw) · Rz(roll) · Ry(model_yaw) · k · X · (m + origin) / 16
//
// with X = diag(−1, 1, 1): the game's `scale(-1, -1, 1)` on its y-down model,
// seen from this project's y-up one. Its determinant is **−1**. So a quad wound
// counter-clockwise seen from outside in model space comes out clockwise in the
// world, and every face is stored and emitted in the order that lands it the
// right way round. `test_entity_model.cpp` asserts it with a cross product
// rather than with a screenshot, because a back-facing mob is invisible.
//
// ── The light ───────────────────────────────────────────────────────────────
//
// Three colours travel on each vertex because the game combines three things in
// an order a single product cannot express (core shader
// `rendertype_entity_cutout`): the texture times the vertex colour — the layer's
// tint and the face's directional shade — then `mix(overlay, colour, overlay.a)`
// for the red of a hurt mob and the white of a creeper about to go off, and only
// then the lightmap. Folding the lightmap into the first product would tint the
// red flash by the light of the block, which the game does not do.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/entity_model.hpp"
#include "ov/render/entity_pose.hpp"

#include <array>
#include <span>
#include <vector>

namespace ov::render {

/// 32 bytes: position, texture coordinate, three colours.
struct EntityVertex {
    f32 x{0.0F};
    f32 y{0.0F};
    f32 z{0.0F};
    f32 u{0.0F};
    f32 v{0.0F};
    /// Tint times directional shade, RGBA.
    std::array<u8, 4> colour{};
    /// The overlay texel: RGB the colour is pulled towards, A how much of the
    /// original survives. (255, 255, 255, 255) leaves the colour alone.
    std::array<u8, 4> overlay{255, 255, 255, 255};
    /// The lightmap sample at the entity's block, RGB; A unused.
    std::array<u8, 4> light{255, 255, 255, 255};
};

static_assert(sizeof(EntityVertex) == 32, "the entity vertex must stay 32 bytes");

/// The overlay the game uses when nothing is happening: fully the original.
inline constexpr u32 kNoOverlay = 0xFFFFFFFFU;

/// Where on the bound texture a model's sheet lies, in 0..1.
///
/// All entity textures share one atlas (entity_atlas.hpp), so a model's own
/// texture coordinates — texels of its logical sheet — are mapped into this
/// rectangle as they are emitted.
struct UvRect {
    f32 u0{0.0F};
    f32 v0{0.0F};
    f32 u1{1.0F};
    f32 v1{1.0F};
};

/// Where and how one entity is drawn.
struct EntityPlacement {
    /// The entity's feet, in world coordinates.
    Vec3f position{};
    /// The body's yaw in degrees, Minecraft's convention: 0 faces +Z.
    f32 body_yaw{0.0F};
    /// Degrees about the entity's own front-to-back axis, applied before the
    /// yaw: the topple of a dying mob, a minecart's pitch on a slope.
    f32 roll{0.0F};
    /// Degrees about the vertical, applied to the model before the roll. A
    /// boat's model lies across its own direction of travel.
    f32 model_yaw{0.0F};
    /// Degrees about the model's own x axis through its origin, in the game's
    /// sense, before anything else: the dragon's body tilting with its beat.
    f32 model_pitch{0.0F};
    /// When set, a whole linear map (the images of this project's model x, y
    /// and z) and a slide, in model units, applied to the model before
    /// anything else — the end crystal's nested turns. Replaces model_pitch.
    bool                 has_model_basis{false};
    std::array<Vec3f, 3> model_basis{};
    Vec3f                model_offset{};
    /// Uniform scale about the model origin. A baby is half its parent; a cave
    /// spider is not a spider's size.
    f32 scale{1.0F};
    /// Added to every model point before the scale, in model units. Where the
    /// renderer puts the model's origin relative to the entity's feet: 24.016
    /// on y for every living entity (the game's 1.501 blocks).
    Vec3f origin{};
    /// Added after every rotation, in blocks. A minecart turns about a point
    /// 0.375 blocks above its rail, not about the rail.
    f32 lift{0.0F};
    /// Multiplied into the texture, 0xAARRGGBB. A fleece's dye, a collar's.
    u32 tint{0xFFFFFFFFU};
    /// The overlay texel, 0xAARRGGBB — see EntityVertex::overlay.
    u32 overlay{kNoOverlay};
    /// The lightmap sample, 0xRRGGBB.
    u32 light{0xFFFFFFU};
    /// False for the eyes and the energy layers, which the game draws without
    /// directional shade.
    bool shade{true};
    /// The model's sheet on the bound texture.
    UvRect uv{};
    /// Added to the sheet coordinates (0..1) before they are mapped into `uv`:
    /// the charged creeper's scrolling swirl. Only meaningful with a texture of
    /// its own and a repeating sampler.
    f32 uv_scroll_u{0.0F};
    f32 uv_scroll_v{0.0F};
};

/// The directional shade the game gives an entity face, from its world normal.
///
/// Two lights and an ambient term, from the core shader include `light.glsl`:
/// `min(1, (max(0, n·L0) + max(0, n·L1)) × 0.6 + 0.4)`. The two directions are
/// the game's own (docs/provenance/rendu-entites.md says how they were read).
/// This is not the terrain's five values: an entity's side is 0.74 where a
/// block's is 0.8, its bottom 0.4 where a block's is 0.5.
[[nodiscard]] f32 entity_shade(Vec3f world_normal) noexcept;

/// Append one entity's triangles to `out`.
///
/// `poses` must have one entry per bone; pose_model() produces exactly that.
/// Returns the number of quads appended, which is what the caller reports as
/// the frame's GPU-side cost.
u32 emit_entity(const EntityModel& model, std::span<const BonePose> poses,
                const EntityPlacement& placement, std::vector<EntityVertex>& out);

/// The world-space box the posed, placed model occupies.
///
/// This is the number the parity check compares against the type's measured
/// collision box: a model that is too tall or off-centre shows up here before
/// it shows up on a screen.
void entity_bounds(const EntityModel& model, std::span<const BonePose> poses,
                   const EntityPlacement& placement, Vec3f& min, Vec3f& max);

/// A bone's frame in the world, in blocks: its pivot and its three axes.
///
/// What a held item or a carried block is attached to. The axes are the
/// model's +X, +Y, +Z carried through every rotation above the bone and the
/// placement, scaled by the placement's scale.
struct BoneFrame {
    Vec3f origin{};
    Vec3f axis_x{1.0F, 0.0F, 0.0F};
    Vec3f axis_y{0.0F, 1.0F, 0.0F};
    Vec3f axis_z{0.0F, 0.0F, 1.0F};

    /// A point given in model units relative to the bone's pivot, in the world.
    [[nodiscard]] Vec3f at(Vec3f local_units) const noexcept {
        return origin + (axis_x * local_units.x + axis_y * local_units.y + axis_z * local_units.z) *
                            (1.0F / 16.0F);
    }
};

/// The frame of `bone` (an index into model.bones), or nullopt-like false.
[[nodiscard]] bool bone_frame(const EntityModel& model, std::span<const BonePose> poses,
                              const EntityPlacement& placement, i32 bone, BoneFrame& out);

/// A quad in world space, for anything that is not a model: a dropped item's
/// sprite, a name tag, a fire sheet, a block.
///
/// Corners are given in the order bottom-left, bottom-right, top-right,
/// top-left as seen from the front; the texture coordinates match them. Emitted
/// double-sided is the caller's business — this appends one facing. `light` is
/// 0xRRGGBB and `overlay` 0xAARRGGBB, as in EntityPlacement.
void emit_quad(const std::array<Vec3f, 4>& corners, const std::array<f32, 4>& u,
               const std::array<f32, 4>& v, u32 tint, std::vector<EntityVertex>& out,
               u32 light = 0xFFFFFFU, u32 overlay = kNoOverlay);

}  // namespace ov::render
