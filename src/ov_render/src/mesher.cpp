#include "ov/render/mesher.hpp"

#include "ov/render/ambient_occlusion.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::render {

namespace {

/// The two axes that span a face, for stepping to the neighbours that share a
/// corner with it. Both point along a positive world axis, so a vertex's
/// projection on them is its 0..1 place across the face.
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

[[nodiscard]] Vec3i scale(Vec3i a, i32 by) {
    return {a.x * by, a.y * by, a.z * by};
}

/// Map a quad's 0..16 sprite coordinate into the atlas.
[[nodiscard]] f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

/// Does the quad lie in the plane of its block's own face — a full stone face,
/// the top of a bottom slab does not? Smooth lighting samples the layer in
/// front of the face when it does, and the block's own layer when it does not.
[[nodiscard]] bool on_block_face(const BakedQuad& quad) {
    const Vec3i normal = direction_offset(quad.facing);
    const f32   target = (normal.x + normal.y + normal.z) > 0 ? 1.0F : 0.0F;
    for (const auto& vertex : quad.vertices) {
        const f32 along = project(vertex.position, Vec3i{std::abs(normal.x), std::abs(normal.y),
                                                         std::abs(normal.z)});
        if (std::abs(along - target) > 1.0e-4F) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] AoSample sample_at(const NeighbourhoodView& view, Vec3i at) {
    return AoSample{view.ao_shade(at), view.sky_light(at), view.block_light(at)};
}

[[nodiscard]] u8 quarters(f32 value) {
    return static_cast<u8>(std::clamp(std::lround(value), 0L, static_cast<long>(kMaxLightQuarters)));
}

}  // namespace

NeighbourhoodView::~NeighbourhoodView() = default;

f32 NeighbourhoodView::ao_shade(Vec3i position) const {
    return casts_ambient_occlusion(position) ? kOccluderShade : 1.0F;
}

bool NeighbourhoodView::blocks_view(Vec3i position) const {
    return casts_ambient_occlusion(position);
}

u16 NeighbourhoodView::fluid_at(Vec3i) const {
    return 0;
}

u32 NeighbourhoodView::biome_colour(Vec3i, TintChannel) const {
    return 0xFFFFFF;
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
        // The layer this face is lit from: the one it faces into when it lies
        // on the block's face, the block's own when it is set back from it.
        const Vec3i centre_at = on_block_face(quad)
                                    ? add(block_position, direction_offset(quad.facing))
                                    : block_position;

        // The four corners of the face, (-t,-b), (+t,-b), (+t,+b), (-t,+b),
        // from the 3x3 of blocks around the centre in the face's plane. Nine
        // lookups a face instead of three a vertex.
        std::array<CornerLighting, 4> corners{};
        const bool smooth = model.ambient_occlusion;
        if (smooth) {
            std::array<AoSample, 9> grid{};
            for (i32 dt = -1; dt <= 1; ++dt) {
                for (i32 db = -1; db <= 1; ++db) {
                    const Vec3i at = add(centre_at, add(scale(plane.tangent, dt),
                                                        scale(plane.bitangent, db)));
                    grid[static_cast<usize>((dt + 1) * 3 + (db + 1))] = sample_at(view, at);
                }
            }
            // The four blocks one step beyond each side, along the normal:
            // whether they block sight decides if a corner sees its diagonal.
            const Vec3i normal = direction_offset(quad.facing);
            const auto  beyond = [&](Vec3i axis, i32 sign) {
                return view.blocks_view(add(add(centre_at, scale(axis, sign)), normal));
            };
            const std::array<bool, 2> beyond_t{beyond(plane.tangent, -1), beyond(plane.tangent, 1)};
            const std::array<bool, 2> beyond_b{beyond(plane.bitangent, -1),
                                               beyond(plane.bitangent, 1)};
            constexpr std::array<std::array<i32, 2>, 4> kSigns{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
            for (usize k = 0; k < 4; ++k) {
                const i32  st     = kSigns[k][0];
                const i32  sb     = kSigns[k][1];
                const bool hidden = beyond_t[static_cast<usize>((st + 1) / 2)] &&
                                    beyond_b[static_cast<usize>((sb + 1) / 2)];
                // A hidden diagonal is stood in for by the side opposite the
                // corner along the first axis — measured, see smooth_corner.
                const AoSample& opposite = grid[static_cast<usize>((-st + 1) * 3 + 1)];
                corners[k] = smooth_corner(grid[4], grid[static_cast<usize>((st + 1) * 3 + 1)],
                                           grid[static_cast<usize>(3 + (sb + 1))],
                                           grid[static_cast<usize>((st + 1) * 3 + (sb + 1))],
                                           hidden ? &opposite : nullptr);
            }
        }
        const u8 flat_sky   = static_cast<u8>(view.sky_light(centre_at) * 4);
        const u8 flat_block = static_cast<u8>(view.block_light(centre_at) * 4);

        for (const auto& vertex : quad.vertices) {
            TerrainVertexAttributes attributes;
            attributes.position =
                Vec3f{vertex.position.x + static_cast<f32>(block_position.x) + info.offset.x,
                      vertex.position.y + static_cast<f32>(block_position.y) + info.offset.y,
                      vertex.position.z + static_cast<f32>(block_position.z) + info.offset.z};

            // The quad's 0..16 sprite coordinate, resolved through the
            // sprite's rect into a normalised atlas one. v is downward in both,
            // so there is no flip anywhere in the chain.
            attributes.u = lerp(sprite.u0, sprite.u1, vertex.u / 16.0F);
            attributes.v = lerp(sprite.v0, sprite.v1, vertex.v / 16.0F);

            attributes.facing = quad.facing;
            attributes.shade  = quad.shade;
            attributes.tint_colour = quad.tint_index >= 0 ? info.tint_colour : 0xFFFFFFu;

            if (smooth) {
                // A vertex at a corner of the block's face takes that corner;
                // one inside it (a slab's edge, a fence post) takes the
                // bilinear blend of the four, by where it sits.
                const f32 s = std::clamp(project(vertex.position, plane.tangent), 0.0F, 1.0F);
                const f32 t = std::clamp(project(vertex.position, plane.bitangent), 0.0F, 1.0F);
                const std::array<f32, 4> weight{(1.0F - s) * (1.0F - t), s * (1.0F - t), s * t,
                                                (1.0F - s) * t};
                f32 brightness = 0.0F;
                f32 sky        = 0.0F;
                f32 block      = 0.0F;
                for (usize k = 0; k < 4; ++k) {
                    brightness += weight[k] * corners[k].brightness;
                    sky += weight[k] * static_cast<f32>(corners[k].sky_quarters);
                    block += weight[k] * static_cast<f32>(corners[k].block_quarters);
                }
                attributes.occlusion      = brightness;
                attributes.sky_quarters   = quarters(sky);
                attributes.block_quarters = quarters(block);
            } else {
                // Flat lighting: one sample for the whole face. What vanilla
                // does when a model turns ambient occlusion off.
                attributes.occlusion      = 1.0F;
                attributes.sky_quarters   = flat_sky;
                attributes.block_quarters = flat_block;
            }

            vertices.push_back(pack_vertex(attributes));
        }
    }
}

}  // namespace ov::render
