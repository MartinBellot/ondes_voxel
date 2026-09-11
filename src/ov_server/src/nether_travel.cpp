#define OV_LOG_CATEGORY "server"

#include "nether_travel.hpp"

#include "ov/base/log.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/region_writer.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <map>

namespace ov::server {

namespace {

constexpr DimensionInfo kOverworld{
    "minecraft:overworld",
    "minecraft:overworld",
    world::WorldShape::overworld(),
    world::DimensionTraits{false, true},
    1.0,
    319,
    gameplay::kSearchRadiusOverworld,
    "region",
};

constexpr DimensionInfo kNether{
    "minecraft:the_nether",
    "minecraft:the_nether",
    world::WorldShape::nether(),
    world::DimensionTraits{true, false},
    8.0,
    127,
    gameplay::kSearchRadiusNether,
    "DIM-1/region",
};

// ── end ── No portal is ever built there by a search (`portal_top` and
// `search_radius` are the Nether portal's questions, and the End has none).
constexpr DimensionInfo kEnd{
    "minecraft:the_end",
    "minecraft:the_end",
    world::WorldShape::the_end(),
    world::DimensionTraits{false, false},
    1.0,
    255,
    0,
    "DIM1/region",
};

[[nodiscard]] std::filesystem::path region_file(const std::filesystem::path& directory, i32 cx,
                                                i32 cz) {
    return directory / fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5);
}

}  // namespace

const DimensionInfo& dimension_info(DimensionId id) noexcept {
    if (id == DimensionId::End) {  // ── end ──
        return kEnd;
    }
    return id == DimensionId::Nether ? kNether : kOverworld;
}

std::optional<DimensionId> dimension_by_name(std::string_view name) noexcept {
    if (name == kOverworld.name) {
        return DimensionId::Overworld;
    }
    if (name == kNether.name) {
        return DimensionId::Nether;
    }
    if (name == kEnd.name) {  // ── end ──
        return DimensionId::End;
    }
    return std::nullopt;
}

// ── NetherWorld ─────────────────────────────────────────────────────────────

std::unique_ptr<NetherWorld> NetherWorld::open(
    const std::filesystem::path& level_dir, const std::filesystem::path& data_root,
    const registry::BlockRegistry& blocks, const registry::Registries& registries,
    std::span<const std::string_view> codec_biomes, world::ChunkCodecContext codec, i64 seed,
    usize workers, Hooks hooks, DimensionId dimension) {
    std::unique_ptr<NetherWorld> world{new NetherWorld()};
    // ── end ── The level's own settings and region directory.
    const DimensionInfo&   info     = dimension_info(dimension);
    const std::string_view settings = dimension == DimensionId::End ? "end" : "nether";
    world->dimension_               = dimension;
    world->region_dir_ = level_dir / std::string{info.region_dir};
    std::error_code ignored;
    std::filesystem::create_directories(world->region_dir_, ignored);
    world->codec_       = codec;
    world->codec_.shape = info.shape;  // a saved chunk comes back 0..256, not overworld-shaped
    world->hooks_       = std::move(hooks);
    world->generated_ = GeneratedWorld::load(data_root, blocks, registries, codec_biomes, seed,
                                             workers + 1, settings);
    if (!world->generated_) {
        OV_LOG_ERROR("{}: the generator could not be built; the level is unavailable", info.name);
        return nullptr;
    }
    world->source_ = std::make_unique<AsyncChunkSource>(*world->generated_, workers);
    OV_LOG_INFO("{}: regions in {}, {} generation workers", info.name,
                world->region_dir_.string(), workers);
    return world;
}

NetherWorld::~NetherWorld() = default;

DimensionView NetherWorld::view() noexcept {
    return DimensionView{&chunks_, &dirty_, &read_only_, dimension_info(dimension_).shape};
}

world::Chunk* NetherWorld::resident(i32 cx, i32 cz) { return chunks_.find(ChunkPos{cx, cz}); }

std::optional<nbt::Document> NetherWorld::read_from_disk(i32 cx, i32 cz) const {
    const auto region = nbt::RegionFile::open(region_file(region_dir_, cx, cz));
    if (!region) {
        return std::nullopt;
    }
    const auto local_x = static_cast<u32>(cx & 31);
    const auto local_z = static_cast<u32>(cz & 31);
    if (!region->has_chunk(local_x, local_z)) {
        return std::nullopt;
    }
    auto document = region->read_chunk(local_x, local_z);
    if (!document) {
        return std::nullopt;
    }
    return std::move(*document);
}

world::Chunk& NetherWorld::chunk_at(i32 cx, i32 cz) {
    if (world::Chunk* here = resident(cx, cz); here != nullptr) {
        return *here;
    }
    // Disk before the generator, as for the overworld: a saved chunk wins.
    if (const auto document = read_from_disk(cx, cz)) {
        if (world::chunk_data_version(*document) == world::kDataVersion1201) {
            if (auto loaded = world::from_nbt(*document, codec_)) {
                chunks_.publish(ChunkPos{cx, cz}, std::move(*loaded));
                world::Chunk& placed = *resident(cx, cz);
                if (hooks_.relight_loaded) {
                    hooks_.relight_loaded(placed);
                }
                if (hooks_.ticks_loaded) {
                    hooks_.ticks_loaded(*document);
                }
                return placed;
            }
        }
        // On disk and unreadable: served generated and never written back.
        read_only_.insert(chunk_key_of(cx, cz));
    }
    ++synchronous_;
    chunks_.publish(ChunkPos{cx, cz}, generated_->generate(cx, cz));
    world::Chunk& made = *resident(cx, cz);
    if (hooks_.relight_generated) {
        hooks_.relight_generated(made);
    }
    return made;
}

void NetherWorld::tick(i64 tick_count) {
    // Terrain comes home: what the workers finished, unless a chunk from disk
    // or one already built in got there first.
    finished_.clear();
    if (source_->drain(finished_) != 0) {
        for (GeneratedBlock& block : finished_) {
            for (auto& [pos, chunk] : block.chunks) {
                if (chunks_.contains(pos)) {
                    continue;
                }
                // A chunk saved on disk beats a generated one, always.
                if (const auto document = read_from_disk(pos.x, pos.z)) {
                    (void)chunk_at(pos.x, pos.z);
                    continue;
                }
                chunks_.publish(pos, std::move(chunk));
                if (hooks_.relight_generated) {
                    hooks_.relight_generated(*resident(pos.x, pos.z));
                }
            }
        }
    }

    // What the tickets want and the map has not got. A chunk on disk is read
    // here, on the tick thread — microseconds; only terrain goes to the pool.
    chunks_.wanted_chunks(wanted_);
    usize disk_reads = 0;
    for (const ChunkPos pos : wanted_) {
        if (chunks_.contains(pos)) {
            continue;
        }
        if (disk_reads < 8 && std::filesystem::exists(region_file(region_dir_, pos.x, pos.z))) {
            if (read_from_disk(pos.x, pos.z)) {
                ++disk_reads;
                (void)chunk_at(pos.x, pos.z);
                continue;
            }
        }
        (void)source_->request(pos);
    }

    // Chunks nothing wants, every five seconds; a dirty chunk waits for the
    // next save.
    if (tick_count % 100 == 0) {
        to_evict_.clear();
        chunks_.for_each([&](ChunkPos pos, const world::Chunk&) {
            const i64 key = chunk_key_of(pos.x, pos.z);
            if (!chunks_.is_wanted(pos) && !dirty_.contains(key) && !read_only_.contains(key)) {
                to_evict_.push_back(pos);
            }
        });
        for (const ChunkPos pos : to_evict_) {
            (void)chunks_.evict(pos);
        }
        evicted_.insert(evicted_.end(), to_evict_.begin(), to_evict_.end());  // ── persistence ──
    }
}

usize NetherWorld::save(i64 game_time, std::span<const world::ScheduledTick> block_ticks,
                        std::span<const world::ScheduledTick> fluid_ticks) {
    std::map<std::pair<i32, i32>, std::vector<i64>> by_region;
    for (const i64 key : dirty_) {
        if (read_only_.contains(key)) {
            continue;
        }
        const auto cx = static_cast<i32>(key >> 32);
        const auto cz = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
        by_region[{cx >> 5, cz >> 5}].push_back(key);
    }
    usize written = 0;
    for (const auto& [region, keys] : by_region) {
        const auto path =
            region_dir_ / fmt::format("r.{}.{}.mca", region.first, region.second);
        auto writer = nbt::RegionWriter::open_or_empty(path);
        for (const i64 key : keys) {
            const auto   cx   = static_cast<i32>(key >> 32);
            const auto   cz   = static_cast<i32>(static_cast<u32>(key & 0xFFFFFFFF));
            const auto*  held = chunks_.find(ChunkPos{cx, cz});
            if (held == nullptr) {
                continue;
            }
            world::ChunkCodecContext context = codec_;
            context.game_time                = game_time;
            context.block_ticks              = block_ticks;
            context.fluid_ticks              = fluid_ticks;
            writer.set_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31),
                             world::to_nbt(*held, context), 0);
            ++written;
        }
        if (!writer.write(path)) {
            OV_LOG_WARN("nether: could not write {}", path.string());
        }
    }
    dirty_.clear();
    if (written != 0) {
        OV_LOG_INFO("nether: saved {} chunks across {} regions", written, by_region.size());
    }
    return written;
}

// ── PortalTimer ─────────────────────────────────────────────────────────────

bool PortalTimer::tick(bool inside, bool creative) noexcept {
    if (cooldown > 0) {
        --cooldown;
    }
    if (inside && cooldown > 0) {
        // Standing in a portal while cooling down keeps the cooldown full:
        // arriving is not the same as wanting to go back.
        cooldown = gameplay::kPortalCooldown;
        return false;
    }
    if (inside) {
        const i32 wait = creative ? gameplay::kPortalWaitCreative : gameplay::kPortalWaitSurvival;
        if (time++ >= wait) {
            time     = wait;
            cooldown = gameplay::kPortalCooldown;
            return true;
        }
        return false;
    }
    time = std::max(0, time - 4);
    return false;
}

// ── The portal index ────────────────────────────────────────────────────────

void portal_blocks_in(const nbt::Document& chunk, std::vector<BlockPos>& out) {
    const nbt::Tag* x_pos = chunk.root.find("xPos");
    const nbt::Tag* z_pos = chunk.root.find("zPos");
    const nbt::Tag* list  = chunk.root.find("sections");
    if (x_pos == nullptr || z_pos == nullptr || list == nullptr ||
        list->type() != nbt::TagType::List) {
        return;
    }
    const auto cx = static_cast<i32>(x_pos->as_i64());
    const auto cz = static_cast<i32>(z_pos->as_i64());
    for (const nbt::Tag& section : *list->list()) {
        const nbt::Tag* y_tag  = section.find("Y");
        const nbt::Tag* states = section.find("block_states");
        if (y_tag == nullptr || states == nullptr) {
            continue;
        }
        const nbt::Tag* palette = states->find("palette");
        if (palette == nullptr || palette->type() != nbt::TagType::List) {
            continue;
        }
        const auto& entries = *palette->list();
        std::vector<bool> is_portal(entries.size(), false);
        bool              any = false;
        for (usize i = 0; i < entries.size(); ++i) {
            const nbt::Tag* name = entries[i].find("Name");
            if (name != nullptr && name->as_string() == "minecraft:nether_portal") {
                is_portal[i] = true;
                any          = true;
            }
        }
        if (!any) {
            continue;
        }
        const i32 base_y = static_cast<i32>(y_tag->as_i64()) * 16;
        if (entries.size() == 1) {
            for (i32 cell = 0; cell < 4096; ++cell) {
                out.push_back(BlockPos{cx * 16 + (cell & 15), base_y + (cell >> 8),
                                       cz * 16 + ((cell >> 4) & 15)});
            }
            continue;
        }
        const nbt::Tag* data = states->find("data");
        const auto* longs    = data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();
        if (longs == nullptr) {
            continue;
        }
        const usize bits = std::max<usize>(4, static_cast<usize>(std::bit_width(entries.size() - 1)));
        const usize per  = 64 / bits;
        for (usize cell = 0; cell < 4096; ++cell) {
            const usize word = cell / per;
            if (word >= longs->size()) {
                break;
            }
            const auto index = static_cast<usize>(
                (static_cast<u64>((*longs)[word]) >> ((cell % per) * bits)) & ((1ULL << bits) - 1));
            if (index < is_portal.size() && is_portal[index]) {
                const auto c = static_cast<i32>(cell);
                out.push_back(BlockPos{cx * 16 + (c & 15), base_y + (c >> 8), cz * 16 + ((c >> 4) & 15)});
            }
        }
    }
}

void portal_blocks_in(const world::Chunk& chunk, const gameplay::PortalRules& rules,
                      std::vector<BlockPos>& out) {
    const auto shape = chunk.shape();
    const i32  ox    = chunk.position().x * 16;
    const i32  oz    = chunk.position().z * 16;
    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
        // A section whose palette names no portal state holds none: skip its
        // 4096 cells. What makes a 128-block search over resident chunks cost
        // milliseconds rather than a second.
        if (((y - shape.min_y) & 15) == 0) {
            const auto section = static_cast<usize>((y - shape.min_y) >> 4);
            if (section < chunk.sections().size()) {
                const auto palette = chunk.sections()[section].blocks().palette();
                const bool any     = std::ranges::any_of(palette, [&](u16 id) {
                    return rules.is_portal(registry::BlockStateId{id});
                });
                if (!any) {
                    y += 15;
                    continue;
                }
            }
        }
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                const auto state = chunk.get_block(x, y, z);
                if (state != registry::kAirState && rules.is_portal(state)) {
                    out.push_back(BlockPos{ox + static_cast<i32>(x), y, oz + static_cast<i32>(z)});
                }
            }
        }
    }
}

}  // namespace ov::server
