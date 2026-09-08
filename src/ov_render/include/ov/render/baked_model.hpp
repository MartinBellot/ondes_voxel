// Baking: from a resolved model plus a blockstate rotation, to flat quads.
//
// This is the step vanilla performs once at resource-pack load and never
// again. Everything variable — which element, which rotation, which texture
// region, whether the texture is locked to the world — is resolved here, so
// the mesher's inner loop sees nothing but four positions, four texture
// coordinates and a cull direction.
//
// Emission is per face, from the model. It is deliberately NOT a greedy
// mesher: see docs/PROVENANCE.md § « Modèles de blocs » for why the plan's
// original "greedy mesher + AO" is the wrong tool for a parity goal.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/block_state_model.hpp"
#include "ov/render/model.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace ov::render {

struct BakedVertex {
    /// Block space: 0..1 across the block. Values outside that range are legal
    /// and normal — the format lets an element overhang its own block.
    Vec3f position{};
    /// Sprite space: 0..16 across the sprite, v downward, exactly as the model
    /// file writes it. Mapping onto an atlas is the atlas's business, and
    /// keeping it out of here is what lets the baker be tested without one.
    f32 u{};
    f32 v{};
};

struct BakedQuad {
    /// In winding order, outward normal.
    std::array<BakedVertex, 4> vertices{};
    /// Resolved sprite name, e.g. `minecraft:block/oak_planks`.
    std::string sprite;
    /// Nearest axis to the quad's own normal. Drives directional shading and
    /// which neighbour ambient occlusion samples.
    Direction facing{Direction::Down};
    /// The neighbour that hides this quad, after the blockstate rotation has
    /// been applied to it.
    std::optional<Direction> cullface;
    i32                      tint_index{-1};
    bool                     shade{true};
};

struct BakedModel {
    std::vector<BakedQuad> quads;
    bool                   ambient_occlusion{true};
};

/// Bake one model under one blockstate variant.
[[nodiscard]] BakedModel bake(const Model& model, const ModelVariant& variant);

/// The texture rectangle a face is given when the model omits `uv`, as
/// `[u1, v1, u2, v2]` in 0..16 sprite units.
///
/// Exposed because it is the one rule in the format that the documentation
/// states only as "it automatically generates based on the element's position".
/// The per-face formulas were recovered from vanilla's own models — see
/// docs/PROVENANCE.md — and ov_modelbake re-checks them against every
/// hand-written uv in the assets on every run.
[[nodiscard]] std::array<f32, 4> default_face_uv(const Element& element, Direction direction);

/// Rotate a direction the way a blockstate's x/y tags do.
///
/// The sense is measured, not chosen: `minecraft:furnace` faces north at y=0
/// and east at y=90, and `minecraft:observer` faces north at x=0 and down at
/// x=90. Both fix the same handedness, and both are asserted in the tests.
[[nodiscard]] Direction rotate_direction(Direction direction, i32 x_degrees, i32 y_degrees);

}  // namespace ov::render
