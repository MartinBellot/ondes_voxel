#include "ov/render/crack_mesh.hpp"

namespace ov::render {

std::array<f32, 2> crack_uv(Direction facing, Vec3f local) noexcept {
    // u to the right and v downwards as a viewer outside the face sees it,
    // north at the top of the up face.
    switch (facing) {
        case Direction::Up: return {local.x, local.z};
        case Direction::Down: return {local.x, 1.0F - local.z};
        case Direction::North: return {1.0F - local.x, 1.0F - local.y};
        case Direction::South: return {local.x, 1.0F - local.y};
        case Direction::West: return {local.z, 1.0F - local.y};
        case Direction::East: return {1.0F - local.z, 1.0F - local.y};
    }
    return {local.x, local.z};
}

void build_crack_quads(const BakedModel& model, BlockPos pos, std::vector<EntityVertex>& out) {
    const Vec3f origin{static_cast<f32>(pos.x), static_cast<f32>(pos.y), static_cast<f32>(pos.z)};
    for (const BakedQuad& quad : model.quads) {
        for (const BakedVertex& vertex : quad.vertices) {
            const auto   uv = crack_uv(quad.facing, vertex.position);
            EntityVertex out_vertex;
            out_vertex.x      = origin.x + vertex.position.x;
            out_vertex.y      = origin.y + vertex.position.y;
            out_vertex.z      = origin.z + vertex.position.z;
            out_vertex.u      = uv[0];
            out_vertex.v      = uv[1];
            out_vertex.colour = {255, 255, 255, 255};
            out.push_back(out_vertex);
        }
    }
}

}  // namespace ov::render
