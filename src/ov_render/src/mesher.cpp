#include "ov/render/mesher.hpp"

#include "ov/render/ambient_occlusion.hpp"

#include <algorithm>
#include <cmath>

namespace ov::render {

namespace {

/// The two axes that span a face, for stepping to the neighbours that share a
/// corner with it.
struct FacePlane {
    Vec3i tangent;
    Vec3i bitangent;
};

[[nodiscard]] FacePlane plane_of(Direction facing) {
    switch (facing) {
        case Direction::Down:
        case Direction::Up: return {{1, 0, 0}, {0, 0, 1}};
        case Direction::North:
        case Direction::South: return {{1, 0, 0}, {0, 1, 0}};
        case Direction::West:
        case Direction::East: return {{0, 0, 1}, {0, 1, 0}};
    }
    return {{1, 0, 0}, {0, 0, 1}};
}

[[nodiscard]] f32 project(Vec3f value, Vec3i axis) {
    return value.x * static_cast<f32>(axis.x) + value.y * static_cast<f32>(axis.y) +
           value.z * static_cast<f32>(axis.z);
}

[[nodiscard]] Vec3i add(Vec3i a, Vec3i b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

/// Map a quad's 0..16 sprite coordinate into the atlas.
[[nodiscard]] f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

}  // namespace

NeighbourhoodView::~NeighbourhoodView() = default;

u16 NeighbourhoodView::fluid_at(Vec3i) const {
    return 0;
}

std::string_view to_string(RenderLayer layer) noexcept {
    switch (layer) {
        case RenderLayer::Solid: return "solid";
        case RenderLayer::Cutout: return "cutout";
        case RenderLayer::CutoutMipped: return "cutout_mipped";
        case RenderLayer::Translucent: return "translucent";
        case RenderLayer::Count: break;
    }
    return "unknown";
}

usize MeshBuffers::total_vertices() const noexcept {
    usize total = 0;
    for (const auto& layer : layers) {
        total += layer.size();
    }
    return total;
}

void MeshBuffers::clear() {
    for (auto& layer : layers) {
        // Capacity is kept: a section is re-meshed every time a block in it
        // changes, and giving the memory back only to ask for it again is what
        // turns block placement into a stutter.
        layer.clear();
    }
}

std::vector<u32> build_shared_quad_indices(u32 quad_count) {
    std::vector<u32> indices;
    indices.reserve(static_cast<usize>(quad_count) * 6);
    for (u32 quad = 0; quad < quad_count; ++quad) {
        const u32 base = quad * 4;
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
    return indices;
}

void emit_block(const BakedModel& model, Vec3i block_position, const BlockRenderInfo& info,
                const TextureAtlas& atlas, const NeighbourhoodView& view, MeshBuffers& out) {
    auto& vertices = out[info.layer];

    for (const auto& quad : model.quads) {
        // The neighbour that hides this quad. Dropping these is the single
        // biggest reduction there is: the inside of a hill emits nothing.
        if (quad.cullface.has_value()) {
            const Vec3i offset    = direction_offset(*quad.cullface);
            const Vec3i neighbour = add(block_position, offset);
            if (view.occludes(neighbour, opposite(*quad.cullface))) {
                continue;
            }
            // Two boxes of the same fluid share a face that is never seen. The
            // opacity test cannot drop it — water is transparent, so it
            // correctly refuses to hide anything — and without this an ocean
            // draws every internal face of every cell.
            if (info.fluid != 0 && view.fluid_at(neighbour) == info.fluid) {
                continue;
            }
        }

        const SpriteUv  sprite = atlas.uv(quad.sprite);
        const FacePlane plane  = plane_of(quad.facing);
        // The block whose light this face is lit by: the one it faces into.
        const Vec3i lit = add(block_position, direction_offset(quad.facing));

        for (const auto& vertex : quad.vertices) {
            TerrainVertexAttributes attributes;
            attributes.position = Vec3f{vertex.position.x + static_cast<f32>(block_position.x),
                                        vertex.position.y + static_cast<f32>(block_position.y),
                                        vertex.position.z + static_cast<f32>(block_position.z)};

            // The quad's 0..16 sprite coordinate, resolved through the
            // sprite's rect into a normalised atlas one. v is downward in both,
            // so there is no flip anywhere in the chain.
            attributes.u = lerp(sprite.u0, sprite.u1, vertex.u / 16.0F);
            attributes.v = lerp(sprite.v0, sprite.v1, vertex.v / 16.0F);

            attributes.facing = quad.facing;
            attributes.shade  = quad.shade;
            attributes.tint   = quad.tint_index >= 0 ? info.tint : TintChannel::None;

            if (model.ambient_occlusion && quad.shade) {
                // The three blocks sharing this corner: the two along the
                // face's own axes, and the diagonal between them.
                const Vec3f local{vertex.position.x, vertex.position.y, vertex.position.z};
                const Vec3f centred{local.x - 0.5F, local.y - 0.5F, local.z - 0.5F};

                const auto signed_axis = [&centred](Vec3i axis) {
                    const f32 amount = project(centred, axis);
                    const i32 sign   = amount >= 0.0F ? 1 : -1;
                    return Vec3i{axis.x * sign, axis.y * sign, axis.z * sign};
                };
                const Vec3i t = signed_axis(plane.tangent);
                const Vec3i b = signed_axis(plane.bitangent);

                const Vec3i side1_at  = add(lit, t);
                const Vec3i side2_at  = add(lit, b);
                const Vec3i corner_at = add(lit, add(t, b));

                const CornerNeighbours neighbours{view.casts_ambient_occlusion(side1_at),
                                                  view.casts_ambient_occlusion(side2_at),
                                                  view.casts_ambient_occlusion(corner_at)};

                attributes.ao = ao_level(neighbours);
                attributes.sky_light =
                    smooth_light(CornerLight{view.sky_light(lit), view.sky_light(side1_at),
                                             view.sky_light(side2_at), view.sky_light(corner_at)},
                                 neighbours);
                attributes.block_light = smooth_light(
                    CornerLight{view.block_light(lit), view.block_light(side1_at),
                                view.block_light(side2_at), view.block_light(corner_at)},
                    neighbours);
            } else {
                // Flat lighting: one sample for the whole face. What vanilla
                // does when a model turns ambient occlusion off.
                attributes.ao          = 3;
                attributes.sky_light   = view.sky_light(lit);
                attributes.block_light = view.block_light(lit);
            }

            vertices.push_back(pack_vertex(attributes));
        }
    }
}

}  // namespace ov::render
