// The order the quads of a translucent section are drawn in.
//
// The translucent layer blends and does not write depth, so what is drawn
// last is on top. Sections were already sorted back to front; the quads inside
// one were drawn in mesh order, which puts the far side of a pane of stained
// glass or a column of water over the near side from half of all directions.
// The game sorts the quads of a translucent section by the distance of their
// centres from the camera, farthest first, and sorts again when the camera has
// moved a block. This is that sort, and the index list it becomes.
//
// No GPU: the order is a unit test, and the terrain renderer owns the buffer.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/terrain_vertex.hpp"

#include <span>
#include <vector>

namespace ov::render {

/// The centre of each quad — four consecutive vertices — in section-local
/// blocks, unpacked from the vertices exactly as the shader places them.
void quad_centres(std::span<const TerrainVertex> vertices, std::vector<Vec3f>& out);

/// Quad numbers ordered farthest first from `eye` (section-local). Ties keep
/// mesh order, so the result does not flicker between two equal orders.
void sort_back_to_front(std::span<const Vec3f> centres, Vec3f eye, std::vector<u32>& order);

/// Six indices a quad, in the order given: the same two triangles as the
/// shared index buffer, 0-1-2 and 0-2-3, quad by quad.
void write_quad_indices(std::span<const u32> order, std::vector<u32>& out);

}  // namespace ov::render
