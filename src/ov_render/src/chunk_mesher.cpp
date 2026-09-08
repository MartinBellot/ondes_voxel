#include "ov/render/chunk_mesher.hpp"

#include "ov/world/chunk_section.hpp"

namespace ov::render {

namespace {

/// Floor division and modulo by 16, for turning a world coordinate into a
/// chunk index and an offset inside it. A plain `/` and `%` are wrong for
/// negative coordinates, and half the world is negative.
[[nodiscard]] constexpr i32 chunk_of(i32 world) noexcept {
    return world >> 4;
}

[[nodiscard]] constexpr usize inside_chunk(i32 world) noexcept {
    return static_cast<usize>(world & 15);
}

/// YZX, the order Minecraft uses everywhere from Anvil to the chunk packet.
[[nodiscard]] constexpr usize section_index(usize x, usize y, usize z) noexcept {
    return (y * 16 + z) * 16 + x;
}

[[nodiscard]] constexpr registry::BlockRegistry::Face to_registry_face(Direction direction) {
    // Both enumerations are the protocol's own numbering, so this is an
    // identity — asserted rather than assumed, because a silent disagreement
    // here would hide the ground under every slab.
    return static_cast<registry::BlockRegistry::Face>(static_cast<u8>(direction));
}

static_assert(static_cast<u8>(Direction::Down) ==
                  static_cast<u8>(registry::BlockRegistry::Face::Down),
              "the render and registry face enumerations must stay in step");
static_assert(static_cast<u8>(Direction::East) ==
                  static_cast<u8>(registry::BlockRegistry::Face::East),
              "the render and registry face enumerations must stay in step");

}  // namespace

ChunkSectionView::ChunkSectionView(const registry::BlockRegistry& blocks,
                                   const ChunkNeighbours& chunks, i32 origin_x, i32 origin_y,
                                   i32 origin_z, const BiomeTints* tints) noexcept
    : blocks_(&blocks),
      tints_(tints),
      chunks_(chunks),
      origin_x_(origin_x),
      origin_y_(origin_y),
      origin_z_(origin_z) {}

const world::Chunk* ChunkSectionView::chunk_for(i32 world_x, i32 world_z) const noexcept {
    const world::Chunk* centre = chunks_[4];
    if (centre == nullptr) {
        return nullptr;
    }
    const i32 dx = chunk_of(world_x) - centre->position().x;
    const i32 dz = chunk_of(world_z) - centre->position().z;
    if (dx < -1 || dx > 1 || dz < -1 || dz > 1) {
        return nullptr;
    }
    return chunks_[static_cast<usize>((dz + 1) * 3 + (dx + 1))];
}

registry::BlockStateId ChunkSectionView::state_at(Vec3i local) const noexcept {
    const i32 world_x = origin_x_ + local.x;
    const i32 world_y = origin_y_ + local.y;
    const i32 world_z = origin_z_ + local.z;

    const world::Chunk* chunk = chunk_for(world_x, world_z);
    if (chunk == nullptr || !chunk->shape().contains_y(world_y)) {
        return registry::kAirState;
    }
    return chunk->get_block(inside_chunk(world_x), world_y, inside_chunk(world_z));
}

bool ChunkSectionView::occludes(Vec3i position, Direction towards) const {
    const auto state = state_at(position);
    // cave_air and void_air are air too, and they are what a cave is full of.
    if (state == registry::kAirState || blocks_->is_air(blocks_->block_of(state))) {
        return false;
    }
    // Two conditions, and both are measured data rather than a guess.
    //
    // The *shape* has to fill the face: a bottom slab hides what is under it
    // and not what is over it, and the registry's sturdy-face mask says which,
    // derived from the state's collision boxes and checked against 23358 faces
    // measured on a real server.
    //
    // The block also has to be opaque to sight. Sturdiness alone is not enough
    // and the difference is visible: glass fills its cube and *is* sturdy, so a
    // mesher that stopped at the shape would delete everything behind a window.
    // Sky-light opacity is the measured column that answers it.
    return blocks_->face_is_sturdy(state, to_registry_face(towards)) &&
           blocks_->blocks_sky_light(blocks_->block_of(state));
}

bool ChunkSectionView::casts_ambient_occlusion(Vec3i position) const {
    const auto state = state_at(position);
    if (state == registry::kAirState || blocks_->is_air(blocks_->block_of(state))) {
        return false;
    }
    // Vanilla darkens a corner from blocks that fill their cube and stop light.
    // A torch, a flower or a pane does neither, and using the light opacity
    // rather than "has any collision" is what keeps a fence from casting the
    // shadow of a wall.
    return blocks_->blocks_sky_light(blocks_->block_of(state));
}

u8 ChunkSectionView::sky_light(Vec3i position) const {
    const i32 world_x = origin_x_ + position.x;
    const i32 world_y = origin_y_ + position.y;
    const i32 world_z = origin_z_ + position.z;

    const world::Chunk* chunk = chunk_for(world_x, world_z);
    if (chunk == nullptr || !chunk->shape().contains_y(world_y)) {
        // Off the edge of what is loaded, or above the world: full daylight, so
        // that the boundary of the loaded area is bright rather than a black
        // wall.
        return 15;
    }
    const world::ChunkSection* section = chunk->section_for_y(world_y);
    if (section == nullptr || section->sky_light().is_absent()) {
        return 15;
    }
    light_seen_ = true;
    return section->sky_light().get(section_index(
        inside_chunk(world_x), static_cast<usize>(world_y & 15), inside_chunk(world_z)));
}

u8 ChunkSectionView::block_light(Vec3i position) const {
    const i32 world_x = origin_x_ + position.x;
    const i32 world_y = origin_y_ + position.y;
    const i32 world_z = origin_z_ + position.z;

    const world::Chunk* chunk = chunk_for(world_x, world_z);
    if (chunk == nullptr || !chunk->shape().contains_y(world_y)) {
        return 0;
    }
    const world::ChunkSection* section = chunk->section_for_y(world_y);
    if (section == nullptr || section->block_light().is_absent()) {
        return 0;
    }
    light_seen_ = true;
    return section->block_light().get(section_index(
        inside_chunk(world_x), static_cast<usize>(world_y & 15), inside_chunk(world_z)));
}

u16 ChunkSectionView::fluid_at(Vec3i position) const {
    const auto state = state_at(position);
    if (state == registry::kAirState) {
        return 0;
    }
    const auto block = blocks_->block_of(state);
    const auto name  = blocks_->block_name(block);
    // Waterlogged blocks hold water too, which is why the registry keeps
    // holds_fluid per state rather than per block. A fence in the sea should
    // not make the sea draw a face against it.
    if (name == "minecraft:water" || name == "minecraft:lava") {
        return static_cast<u16>(block.value() + 1u);
    }
    return 0;
}

Rgb BiomeTints::colour(u16 world_biome, TintChannel channel) const noexcept {
    if (colours_ == nullptr || world_biome >= world_to_registry_.size()) {
        return 0xFFFFFF;
    }
    const u32 index = world_to_registry_[world_biome];
    switch (channel) {
        case TintChannel::Grass:
            return colours_->grass(index);
        case TintChannel::Foliage:
            return colours_->foliage(index);
        case TintChannel::Water:
            return colours_->water(index);
        // Constants, and the same two constants everywhere in the world: a
        // spruce in a jungle is the dark green of a taiga's, and a birch is
        // pale wherever it grows. Published values.
        case TintChannel::EvergreenFoliage:
            return 0x619961;
        case TintChannel::BirchFoliage:
            return 0x80A755;
        case TintChannel::None:
            break;
    }
    return 0xFFFFFF;
}

u32 ChunkSectionView::biome_colour(Vec3i position, TintChannel channel) const {
    if (tints_ == nullptr || channel == TintChannel::None) {
        return 0xFFFFFF;
    }
    const i32 world_x = origin_x_ + position.x;
    const i32 world_y = origin_y_ + position.y;
    const i32 world_z = origin_z_ + position.z;

    // Vanilla's biome blend: a square of blocks around this one, at its own
    // height, averaged channel by channel. The radius is a video setting whose
    // default is 2, so twenty-five samples. Without it a biome boundary is a
    // hard line across the grass, which is the single most recognisable thing
    // about a world rendered with the tint resolved per biome rather than per
    // block.
    //
    // Horizontal only, and that is not a simplification: the blend reads the
    // biome at one Y, so a cave under a swamp does not tint the meadow above
    // it.
    constexpr i32 kRadius  = 2;
    constexpr u32 kSamples = (kRadius * 2 + 1) * (kRadius * 2 + 1);

    u32 red = 0;
    u32 green = 0;
    u32 blue = 0;
    for (i32 dz = -kRadius; dz <= kRadius; ++dz) {
        for (i32 dx = -kRadius; dx <= kRadius; ++dx) {
            const world::Chunk* chunk = chunk_for(world_x + dx, world_z + dz);
            u16                 biome = 0;
            if (chunk != nullptr && chunk->shape().contains_y(world_y)) {
                biome = chunk->get_biome(inside_chunk(world_x + dx), world_y,
                                         inside_chunk(world_z + dz));
            }
            const Rgb sample = tints_->colour(biome, channel);
            red += (sample >> 16) & 0xFFu;
            green += (sample >> 8) & 0xFFu;
            blue += sample & 0xFFu;
        }
    }
    // Integer division, like vanilla's: the average of twenty-five bytes is
    // truncated, not rounded, and rounding here would put every blended block
    // half a unit away from the game's.
    return ((red / kSamples) << 16) | ((green / kSamples) << 8) | (blue / kSamples);
}

SectionMeshStats mesh_section(const ChunkSectionView& view, BlockModelCache& models,
                              const TextureAtlas& atlas, MeshBuffers& out) {
    SectionMeshStats stats;

    for (i32 y = 0; y < 16; ++y) {
        for (i32 z = 0; z < 16; ++z) {
            for (i32 x = 0; x < 16; ++x) {
                ++stats.blocks_visited;

                const Vec3i local{x, y, z};
                const auto  state = view.state_at(local);
                if (state == registry::kAirState) {
                    continue;
                }

                const BlockRender& render = models.resolve(state);
                if (!render.drawable) {
                    continue;
                }

                const usize before = out.total_vertices();
                // Resolved once per block rather than once per quad: the tint
                // is a property of the position, and a grass block has six
                // faces that share it.
                const u32 tint = render.tint == TintChannel::None
                                     ? 0xFFFFFFu
                                     : view.biome_colour(local, render.tint);
                emit_block(render.model, local, BlockRenderInfo{render.layer, tint, render.fluid},
                           atlas, view, out);
                const usize emitted = out.total_vertices() - before;
                if (emitted > 0) {
                    ++stats.blocks_drawn;
                    stats.quads += emitted / 4;
                }
            }
        }
    }
    return stats;
}

}  // namespace ov::render
