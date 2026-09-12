#define OV_LOG_CATEGORY "voxel"

#include "session.hpp"

#include <cmath>

#include "ov/base/log.hpp"

#include <algorithm>
#include <chrono>

namespace ov::demo {

namespace {

[[nodiscard]] constexpr i32 chunk_of(i32 world) noexcept {
    return world >> 4;
}

}  // namespace

Session::Session(const registry::BlockRegistry& blocks, render::BlockModelCache& models,
                 const render::TextureAtlas& atlas, const render::BiomeTints& tints,
                 client::TerrainRenderer& terrain)
    : blocks_(&blocks),
      motion_(blocks),
      models_(&models),
      atlas_(&atlas),
      tints_(&tints),
      terrain_(&terrain) {
    // Resolved once. A name comparison per cell would put a string compare
    // inside the physics tick, which runs over the player's whole box every
    // twentieth of a second.
    if (const auto water = blocks.find_block("minecraft:water")) {
        water_block_ = *water;
        if (const auto lava = blocks.find_block("minecraft:lava")) {
            lava_block_   = *lava;
            fluids_known_ = true;
        }
    }
}

gameplay::FluidSample Session::fluid_at(i32 x, i32 y, i32 z) const {
    if (!fluids_known_) {
        return {};
    }
    const auto state = block_at(x, y, z);
    if (state == registry::kAirState) {
        return {};
    }
    // ── implicit water ── the registry's one answer: the fluid block at its
    // level, or a full source held by a waterlogged block — and by seagrass,
    // kelp and a bubble column, which hold one with no property to say so.
    const registry::BlockRegistry::StateFluid held = blocks_->fluid(state);
    if (held.empty()) {
        return {};
    }
    // A fluid's rendered height is its `amount` over nine. Level 0 is a source
    // and level 1 to 7 are the flowing steps; level 8 and above are falling,
    // and fill their block.
    const f64 amount = held.level >= 8 ? 8.0 : 8.0 - static_cast<f64>(held.level);
    return gameplay::FluidSample{held.is_water() ? gameplay::Fluid::Water : gameplay::Fluid::Lava,
                                 amount / 9.0};
}

bool Session::is_interaction_target(i32 x, i32 y, i32 z) const {
    const auto state = block_at(x, y, z);
    if (state == registry::kAirState) {
        return false;
    }
    if (!fluids_known_) {
        return true;
    }
    const auto block = blocks_->block_of(state);
    return block != water_block_ && block != lava_block_;
}

gameplay::FluidWorld Session::fluids() const {
    const auto lookup = [](void* context, i32 x, i32 y, i32 z) {
        return static_cast<const Session*>(context)->fluid_at(x, y, z);
    };
    return gameplay::FluidWorld{lookup, const_cast<Session*>(this)};
}

const world::Chunk* Session::chunk_at(i32 chunk_x, i32 chunk_z) const {
    const auto found = chunks_.find({chunk_x, chunk_z});
    return found == chunks_.end() ? nullptr : found->second.get();
}

registry::BlockStateId Session::block_at(i32 x, i32 y, i32 z) const {
    const world::Chunk* chunk = chunk_at(chunk_of(x), chunk_of(z));
    if (chunk == nullptr || !chunk->shape().contains_y(y)) {
        return registry::kAirState;
    }
    return chunk->get_block(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15));
}

u32 Session::biome_at(i32 x, i32 y, i32 z) const {
    const world::Chunk* chunk = chunk_at(chunk_of(x), chunk_of(z));
    if (chunk == nullptr || !chunk->shape().contains_y(y)) {
        return 0;
    }
    return chunk->get_biome(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15));
}

gameplay::CollisionWorld Session::collision() const {
    // A function pointer and a context, because ov_gameplay's header must not
    // learn what a Session is.
    const auto lookup = [](void* context, i32 x, i32 y, i32 z) {
        return static_cast<const Session*>(context)->block_at(x, y, z);
    };
    return gameplay::CollisionWorld{*blocks_, lookup, const_cast<Session*>(this), &motion_};
}

render::ChunkNeighbours Session::neighbours_of(i32 chunk_x, i32 chunk_z) const {
    render::ChunkNeighbours neighbours{};
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            neighbours[static_cast<usize>((dz + 1) * 3 + (dx + 1))] =
                chunk_at(chunk_x + dx, chunk_z + dz);
        }
    }
    return neighbours;
}

void Session::mark_dirty(i32 chunk_x, i32 chunk_z, i32 section) {
    const world::Chunk* chunk = chunk_at(chunk_x, chunk_z);
    if (chunk == nullptr) {
        return;
    }
    if (section < 0 || static_cast<usize>(section) >= chunk->shape().section_count()) {
        return;
    }
    const SectionKey key{chunk_x, chunk_z, section};
    if (dirty_set_.insert(key).second) {
        dirty_.push_back(key);
        dirty_sorted_ = false;
    }
}

void Session::mark_column_dirty(i32 chunk_x, i32 chunk_z) {
    const world::Chunk* chunk = chunk_at(chunk_x, chunk_z);
    if (chunk == nullptr) {
        return;
    }
    for (usize index = 0; index < chunk->shape().section_count(); ++index) {
        mark_dirty(chunk_x, chunk_z, static_cast<i32>(index));
    }
}

void Session::apply(netclient::ClientEvents& events) {
    for (auto& chunk : events.loaded) {
        const i32 x = chunk->position().x;
        const i32 z = chunk->position().z;
        chunks_[{x, z}] = std::move(chunk);
        mark_column_dirty(x, z);
        // The four neighbours too. A chunk that arrives next to one already
        // drawn reveals faces that were culled against nothing, and leaves a
        // wall of missing geometry along the seam if they are not redone.
        mark_column_dirty(x - 1, z);
        mark_column_dirty(x + 1, z);
        mark_column_dirty(x, z - 1);
        mark_column_dirty(x, z + 1);
    }
    events.loaded.clear();

    for (const auto& [x, z] : events.unloaded) {
        const world::Chunk* chunk = chunk_at(x, z);
        if (chunk == nullptr) {
            continue;
        }
        for (usize index = 0; index < chunk->shape().section_count(); ++index) {
            release(SectionKey{x, z, static_cast<i32>(index)});
        }
        chunks_.erase({x, z});
        std::erase_if(dirty_, [x, z](const SectionKey& key) {
            return key.chunk_x == x && key.chunk_z == z;
        });
        std::erase_if(dirty_set_, [x, z](const SectionKey& key) {
            return key.chunk_x == x && key.chunk_z == z;
        });
    }

    for (const auto& change : events.changed) {
        const i32     chunk_x = chunk_of(change.x);
        const i32     chunk_z = chunk_of(change.z);
        world::Chunk* chunk   = nullptr;
        if (const auto found = chunks_.find({chunk_x, chunk_z}); found != chunks_.end()) {
            chunk = found->second.get();
        }
        if (chunk == nullptr || !chunk->shape().contains_y(change.y)) {
            continue;
        }
        chunk->set_block(static_cast<usize>(change.x & 15), change.y,
                         static_cast<usize>(change.z & 15), change.state);

        const i32 section = (change.y - chunk->shape().min_y) / 16;
        mark_dirty(chunk_x, chunk_z, section);
        // A block on a section boundary changes what its neighbour draws, and
        // so does one on a chunk boundary. Both are cheap to redo and both are
        // invisible until someone stands in the wrong place.
        if ((change.y & 15) == 0) {
            mark_dirty(chunk_x, chunk_z, section - 1);
        }
        if ((change.y & 15) == 15) {
            mark_dirty(chunk_x, chunk_z, section + 1);
        }
        if ((change.x & 15) == 0) {
            mark_dirty(chunk_x - 1, chunk_z, section);
        }
        if ((change.x & 15) == 15) {
            mark_dirty(chunk_x + 1, chunk_z, section);
        }
        if ((change.z & 15) == 0) {
            mark_dirty(chunk_x, chunk_z - 1, section);
        }
        if ((change.z & 15) == 15) {
            mark_dirty(chunk_x, chunk_z + 1, section);
        }
    }
}

void Session::release(const SectionKey& key) {
    const auto found = slots_.find(key);
    if (found == slots_.end() || !found->second.live) {
        return;
    }
    for (usize layer = 0; layer < found->second.slots.size(); ++layer) {
        const u32 slot = found->second.slots[layer];
        if (slot != ~0U) {
            terrain_->remove_section(slot);
            --resident_;
        }
    }
    slots_.erase(found);
}

void Session::mesh_one(const SectionKey& key) {
    const world::Chunk* chunk = chunk_at(key.chunk_x, key.chunk_z);
    if (chunk == nullptr) {
        return;
    }
    const auto shape    = chunk->shape();
    const i32  origin_y = shape.min_y + key.section * 16;

    release(key);

    const auto neighbours = neighbours_of(key.chunk_x, key.chunk_z);
    const render::ChunkSectionView view(*blocks_, neighbours, key.chunk_x * 16, origin_y,
                                        key.chunk_z * 16, tints_);

    scratch_.clear();
    const auto stats = render::mesh_section(view, *models_, *atlas_, scratch_);
    if (stats.quads == 0) {
        return;
    }

    SectionSlots entry;
    entry.slots.fill(~0U);
    const Vec3f origin{static_cast<f32>(key.chunk_x * 16), static_cast<f32>(origin_y),
                       static_cast<f32>(key.chunk_z * 16)};
    for (usize layer = 0; layer < entry.slots.size(); ++layer) {
        const auto& vertices = scratch_.layers[layer];
        if (vertices.empty()) {
            continue;
        }
        const auto slot = terrain_->add_section(origin, static_cast<render::RenderLayer>(layer),
                                                vertices);
        if (!slot) {
            OV_LOG_WARN("the terrain arena refused a section at {},{}", key.chunk_x, key.chunk_z);
            continue;
        }
        entry.slots[layer] = *slot;
        entry.live         = true;
        ++resident_;
    }
    if (entry.live) {
        slots_[key] = entry;
    }
}

usize Session::mesh_pending(f64 budget_ms, const Vec3d& eye) {
    if (dirty_.empty()) {
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();

    // Nearest first. Distance in whole sections — a chunk and a section are
    // both sixteen blocks — measured from the section the eye is in, and the
    // list re-sorted only when it changed or the eye crossed into another
    // section: a few thousand keys, a fraction of a millisecond, not per
    // section meshed.
    const auto floor_div16 = [](f64 v) {
        return static_cast<i32>(std::floor(v / 16.0));
    };
    const i32 min_y   = chunks_.empty() ? -64 : chunks_.begin()->second->shape().min_y;
    const i32 focus_x = floor_div16(eye.x);
    const i32 focus_z = floor_div16(eye.z);
    const i32 focus_s = floor_div16(eye.y - static_cast<f64>(min_y));
    if (!dirty_sorted_ || focus_x != focus_x_ || focus_z != focus_z_ ||
        focus_s != focus_section_) {
        focus_x_       = focus_x;
        focus_z_       = focus_z;
        focus_section_ = focus_s;
        const auto distance = [&](const SectionKey& key) {
            const i64 dx = key.chunk_x - focus_x;
            const i64 dz = key.chunk_z - focus_z;
            const i64 dy = key.section - focus_s;
            return dx * dx + dy * dy + dz * dz;
        };
        // Furthest first, ties by key so the order is total: the back is
        // the nearest, and pop_back takes it.
        std::ranges::sort(dirty_, [&](const SectionKey& a, const SectionKey& b) {
            const i64 da = distance(a);
            const i64 db = distance(b);
            return da != db ? da > db : b < a;
        });
        dirty_sorted_ = true;
    }

    usize done = 0;
    while (!dirty_.empty()) {
        const SectionKey key = dirty_.back();
        dirty_.pop_back();
        dirty_set_.erase(key);
        mesh_one(key);
        ++done;

        // Checked after at least one, so that progress is always made even
        // when a single section costs more than the whole budget. A budget
        // that can starve is a stall waiting for a slow machine.
        const auto elapsed =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (elapsed >= budget_ms) {
            break;
        }
    }
    return done;
}

}  // namespace ov::demo
