// A second dimension for the server: the Nether's chunks, and the way there.
//
// server.cpp was written for one world: its chunk map, its dirty set and its
// generator are locals, and a few hundred lines reach them by name. Turning
// all of that into a per-dimension structure in one go would rewrite the file
// under four other people's feet. So the overworld keeps its locals, and the
// Nether gets this — the same four things, owned in one place — and server.cpp
// addresses "the world a player is in" through a small `DimensionView` that is
// either the overworld's locals or the Nether's members.
//
// What lives here:
//
//   * `DimensionId` and the fixed facts of the two dimensions the server
//     knows: names on the wire, chunk shape, the rules (`ultrawarm`), the
//     coordinate scale and the logical height a new portal must fit under.
//     The End is refused and named: `dimension_by_name` does not know it.
//   * `NetherWorld`: the Nether's chunk map, its async generator, its region
//     files under `DIM-1/region` exactly where vanilla keeps them, and its
//     dirty set. Not thread-safe — the server calls it under `chunk_mutex`,
//     as it does its own map.
//   * `PortalTimer`: one entity's standing-in-a-portal clock and cooldown.
//   * `portal_blocks_in`: the portal blocks a chunk holds, on disk or in
//     memory — the index vanilla keeps as the `nether_portal` POI, rebuilt
//     here from the chunks themselves at the moment a search needs it.
#pragma once

#include "async_chunk_source.hpp"
#include "generated_world.hpp"
#include "ov/base/types.hpp"
#include "ov/gameplay/nether_portal.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_map.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/world/level.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ov::server {

enum class DimensionId : u8 { Overworld = 0, Nether = 1 };

struct DimensionInfo {
    std::string_view       name;      ///< `minecraft:the_nether`: the level's key
    std::string_view       type;      ///< the `dimension_type` the client is told
    world::WorldShape      shape;
    world::DimensionTraits traits;
    /// Blocks in this dimension per block in the overworld: 1 and 8.
    f64 coordinate_scale{1.0};
    /// The highest y a new portal's frame may reach: the logical height's top.
    /// 127 in the Nether (logical height 128), 319 in the overworld.
    i32 portal_top{319};
    /// How far a destination search reaches here, square, in blocks.
    i32 search_radius{128};
    /// Where its regions live, relative to the level directory.
    std::string_view region_dir;
};

[[nodiscard]] const DimensionInfo& dimension_info(DimensionId id) noexcept;

/// The dimension a saved name means, or nothing for one this server does not
/// have — the End, and anything a datapack adds. Refused by name, never
/// silently turned into the overworld.
[[nodiscard]] std::optional<DimensionId> dimension_by_name(std::string_view name) noexcept;

/// What a block-writing helper needs to know about a dimension's storage.
struct DimensionView {
    world::ChunkMap*         chunks{nullptr};
    std::unordered_set<i64>* dirty{nullptr};
    std::unordered_set<i64>* read_only{nullptr};
    world::WorldShape        shape{world::WorldShape::overworld()};
};

/// The chunk key the server uses everywhere: x high, z low.
[[nodiscard]] constexpr i64 chunk_key_of(i32 cx, i32 cz) noexcept {
    return (static_cast<i64>(cx) << 32) ^ static_cast<u32>(cz);
}

/// The Nether's storage and generation.
class NetherWorld {
public:
    struct Hooks {
        /// Relight a chunk just read or generated. The server's own passes.
        std::function<void(world::Chunk&)> relight_loaded;
        std::function<void(world::Chunk&)> relight_generated;
        /// A chunk read from disk carried pending ticks; hand them over.
        std::function<void(const nbt::Document&)> ticks_loaded;
    };

    /// Build the Nether's generator (`stacks` worldgen stacks) and open its
    /// regions under `level_dir / DIM-1 / region`. Null with the reason logged
    /// when the generator cannot be built.
    [[nodiscard]] static std::unique_ptr<NetherWorld> open(
        const std::filesystem::path& level_dir, const std::filesystem::path& data_root,
        const registry::BlockRegistry& blocks, const registry::Registries& registries,
        std::span<const std::string_view> codec_biomes, world::ChunkCodecContext codec, i64 seed,
        usize workers, Hooks hooks);

    NetherWorld(const NetherWorld&)            = delete;
    NetherWorld& operator=(const NetherWorld&) = delete;
    ~NetherWorld();

    [[nodiscard]] DimensionView view() noexcept;
    [[nodiscard]] world::ChunkMap& chunks() noexcept { return chunks_; }

    /// A resident chunk, or null.
    [[nodiscard]] world::Chunk* resident(i32 cx, i32 cz);

    /// A chunk from memory, then disk, then generated on the calling thread.
    /// For gameplay that needs a block *now* — a portal search, a dig at the
    /// edge of what has streamed. Counted in `synchronous_generations`.
    [[nodiscard]] world::Chunk& chunk_at(i32 cx, i32 cz);

    /// Does a chunk exist on disk? A portal search reads those without keeping
    /// them.
    [[nodiscard]] std::optional<nbt::Document> read_from_disk(i32 cx, i32 cz) const;

    /// Once per tick: publish what the workers finished, ask for what the
    /// tickets want, and every five seconds drop what nothing wants.
    void tick(i64 tick_count);

    void mark_dirty(i32 cx, i32 cz) { dirty_.insert(chunk_key_of(cx, cz)); }

    /// Write every dirty chunk to `DIM-1/region`. `game_time` and the two
    /// tick snapshots are the level's, as for the overworld.
    usize save(i64 game_time, std::span<const world::ScheduledTick> block_ticks,
               std::span<const world::ScheduledTick> fluid_ticks);

    [[nodiscard]] u64 synchronous_generations() const noexcept { return synchronous_; }

private:
    NetherWorld() = default;

    std::filesystem::path             region_dir_;
    world::ChunkCodecContext          codec_{};
    Hooks                             hooks_;
    std::unique_ptr<GeneratedWorld>   generated_;
    std::unique_ptr<AsyncChunkSource> source_;
    world::ChunkMap                   chunks_;
    std::unordered_set<i64>           dirty_;
    std::unordered_set<i64>           read_only_;
    std::vector<GeneratedBlock>       finished_;
    std::vector<ChunkPos>             wanted_;
    std::vector<ChunkPos>             to_evict_;
    u64                               synchronous_{0};
};

/// One entity's portal clock.
///
/// The wiki's numbers: 80 ticks standing in a portal in survival, 1 for an
/// invulnerable player; afterwards 300 ticks of cooldown, refreshed for as long
/// as the entity stays in a portal — so arriving in the other portal does not
/// send it straight back. Leaving the portal walks the clock back four ticks
/// at a time.
struct PortalTimer {
    i32 time{0};
    i32 cooldown{0};

    /// Advance one tick. True when the entity should travel now.
    [[nodiscard]] bool tick(bool inside, bool creative) noexcept;
};

/// The portal blocks in one chunk's NBT, in world coordinates. Reads only the
/// sections whose palette names `minecraft:nether_portal`.
void portal_blocks_in(const nbt::Document& chunk, std::vector<BlockPos>& out);

/// The same for a resident chunk.
void portal_blocks_in(const world::Chunk& chunk, const gameplay::PortalRules& rules,
                      std::vector<BlockPos>& out);

}  // namespace ov::server
