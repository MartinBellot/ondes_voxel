// The shape a block shows when it is aimed at and when it breaks.
//
// Vanilla has three shapes per state — collision, outline, visual — and only
// the first is in this project's data (BlockRegistry::collision_boxes, measured).
// The outline is what the black box around the aimed block follows and what
// the break particles are spread over. Until it is measured it is derived here,
// and the derivation is named:
//
//   * a state that collides takes its collision boxes, with their tops cut at
//     the top of the block — a fence, a wall and a gate collide up to 1.5 but
//     are outlined to 1;
//   * a state that collides with nothing — a torch, a flower, a rail, a button
//     — takes the bounds of its model, one box. That is right for a torch's
//     stick and too wide for a flower's crossed planes; the difference is in
//     docs/provenance/cassage-bloc.md.
//
// And the outline itself is drawn as vanilla draws a shape's edges: where the
// surface of the *union* folds, so a stair shows its step and no seam where its
// two boxes meet.
#pragma once

#include "ov/math/aabb.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/render/baked_model.hpp"

#include <array>
#include <span>
#include <vector>

namespace ov::render {

/// The boxes of a state's outline, in block space (0..1 across the block).
/// Appends; appends nothing for air or a state with neither boxes nor model.
void outline_boxes(const registry::BlockRegistry& blocks, registry::BlockStateId state,
                   const BakedModel& model, std::vector<AABB>& out);

/// One box around everything in `boxes`, or an empty box for none.
[[nodiscard]] AABB bounds_of(std::span<const AABB> boxes) noexcept;

/// The edges of the union of `boxes`, each as its two ends, merged along their
/// length. Appends.
void shape_edges(std::span<const AABB> boxes, std::vector<std::array<Vec3d, 2>>& out);

}  // namespace ov::render
