// The cracks over a block being broken, as geometry.
//
// Vanilla draws the destroy-stage texture over the block's *own model*, not
// over a cube: a slab cracks on its half, a torch on its stick, a fence on its
// post and arms. It also does not take the model's texture coordinates — the
// crack is projected onto each face from the position on that face, so it lies
// across a slab's top the way it would across a full block's, cut where the
// slab ends, and repeats past the edge of an element that overhangs its block.
//
// So: every quad of the block's baked model, moved to the block, with its
// texture coordinates replaced by the projection of its corners onto the face
// it looks towards — the texture upright, as seen from outside the face. The
// orientation of each projection is ours (docs/provenance/cassage-bloc.md
// compares it with the real client); the covering is vanilla's.
//
// Pure geometry: no GPU, testable in test_ov_render.
#pragma once

#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/baked_model.hpp"
#include "ov/render/entity_mesh.hpp"

#include <array>
#include <vector>

namespace ov::render {

/// The crack texture coordinates of a point on a face looking towards
/// `facing`, from the point's position in the block (0..1 across it). Values
/// outside 0..1 are for a repeating sampler.
[[nodiscard]] std::array<f32, 2> crack_uv(Direction facing, Vec3f local) noexcept;

/// Append the crack decal for the block at `pos`: four vertices a quad, in the
/// model's own winding, white and opaque.
void build_crack_quads(const BakedModel& model, BlockPos pos, std::vector<EntityVertex>& out);

}  // namespace ov::render
