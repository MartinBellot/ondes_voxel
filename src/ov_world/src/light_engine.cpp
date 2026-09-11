#include "ov/world/light_engine.hpp"

#include "ov/world/chunk_section.hpp"
#include "ov/world/light_array.hpp"
#include "ov/world/paletted_container.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace ov::world {
namespace {

enum Kind : u8 { kSky = 0, kBlock = 1 };

// Directions in a fixed order; only "down" is special, for sky light.
enum Dir : u8 { kWest = 0, kEast, kNorth, kSouth, kDown, kUp };
constexpr std::array<std::array<i32, 3>, 6> kOffsets{{
    {-1, 0, 0},
    {1, 0, 0},
    {0, 0, -1},
    {0, 0, 1},
    {0, -1, 0},
    {0, 1, 0},
}};

// One byte per block state: the emission in the low nibble, then two flags.
constexpr u8 kEmissionMask = 0x0F;
constexpr u8 kOpaqueBit    = 0x10;
constexpr u8 kFilterBit    = 0x20;

struct Node {
    i32 x;
    i32 y;
    i32 z;
    u8  level;
};

[[nodiscard]] LightArray& array_of(ChunkSection& section, u8 kind) noexcept {
    return kind == kSky ? section.sky_light() : section.block_light();
}

/// A source that holds one chunk: what `light_chunk` floods inside.
class SingleChunk final : public LightChunkSource {
public:
    explicit SingleChunk(Chunk& chunk) : chunk_{chunk} {}

    Chunk* light_chunk(i32 chunk_x, i32 chunk_z) override {
        const ChunkPos pos = chunk_.position();
        return pos.x == chunk_x && pos.z == chunk_z ? &chunk_ : nullptr;
    }

private:
    Chunk& chunk_;
};

}  // namespace

struct LightEngine::Impl {
    const registry::BlockRegistry* blocks{nullptr};
    LightRules                     rules;
    std::vector<u8>                table;
    bool                           skip_removal{false};

    // Kept between calls so that a tick in steady state allocates nothing:
    // each grows to the largest edit it has seen and stays there.
    std::vector<BlockPos>      pending;
    std::vector<Node>          removal;
    std::vector<Node>          increase;
    std::vector<Node>          reseed;
    std::vector<ChunkSection*> touched;

    // Chunk lookups are a virtual call and a hash probe on the caller's side;
    // a flood asks for the same few chunks hundreds of thousands of times. A
    // direct-mapped cache of 64 slots, emptied at every call because a chunk
    // may be unloaded between two of them. Absent chunks are cached as well.
    struct CacheEntry {
        i32    x{0};
        i32    z{0};
        Chunk* chunk{nullptr};
        bool   filled{false};
    };
    std::array<CacheEntry, 64> cache{};
    LightChunkSource*          source{nullptr};

    struct Cell {
        ChunkSection* section{nullptr};
        usize         index{0};
        i32           max_y{0};
    };

    void begin(LightChunkSource& chunks) {
        source = &chunks;
        for (CacheEntry& entry : cache) {
            entry.filled = false;
        }
        touched.clear();
    }

    Chunk* chunk_at(i32 chunk_x, i32 chunk_z) {
        CacheEntry& entry = cache[static_cast<usize>((chunk_x & 7) | ((chunk_z & 7) << 3))];
        if (!entry.filled || entry.x != chunk_x || entry.z != chunk_z) {
            entry = CacheEntry{chunk_x, chunk_z, source->light_chunk(chunk_x, chunk_z), true};
        }
        return entry.chunk;
    }

    bool locate(i32 x, i32 y, i32 z, Cell& out) {
        Chunk* chunk = chunk_at(x >> 4, z >> 4);
        if (chunk == nullptr) {
            return false;
        }
        ChunkSection* section = chunk->section_for_y(y);
        if (section == nullptr) {
            return false;
        }
        out.section = section;
        out.index   = section_index(static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                    static_cast<usize>(z & 15));
        out.max_y   = chunk->shape().max_y();
        return true;
    }

    [[nodiscard]] u8 info_of_state(u16 state) const noexcept {
        return state < table.size() ? table[state] : kOpaqueBit;
    }

    [[nodiscard]] u8 info_of(const Cell& cell) const noexcept {
        return info_of_state(cell.section->blocks().get(cell.index));
    }

    [[nodiscard]] static u8 get(const Cell& cell, u8 kind) noexcept {
        return array_of(*cell.section, kind).get(cell.index);
    }

    void set(const Cell& cell, u8 kind, u8 value) {
        array_of(*cell.section, kind).set(cell.index, value);
        if (touched.empty() || touched.back() != cell.section) {
            touched.push_back(cell.section);
        }
    }

    /// The level a cell holding `level` gives its neighbour in `dir`, whose
    /// flags are `info`. Nothing enters a block that stops light; sky light at
    /// 15 falls without loss unless the block below filters it.
    [[nodiscard]] u8 through(u8 level, u8 dir, u8 info, u8 kind) const noexcept {
        if ((info & kOpaqueBit) != 0) {
            return 0;
        }
        if (kind == kSky && dir == kDown && level == kMaxLightLevel) {
            const bool filters = rules.filtering_dims_sky && (info & kFilterBit) != 0;
            if (!filters) {
                return kMaxLightLevel;
            }
        }
        return level > 0 ? static_cast<u8>(level - 1) : u8{0};
    }

    /// What a cell holds with no neighbour at all: its emission, or for sky
    /// light at the top of the world, what the open sky above gives it.
    [[nodiscard]] u8 source_level(const Cell& cell, i32 y, u8 info, u8 kind) const noexcept {
        if (kind == kBlock) {
            return static_cast<u8>(info & kEmissionMask);
        }
        return y == cell.max_y ? through(kMaxLightLevel, kDown, info, kSky) : u8{0};
    }

    void run_removal(u8 kind, LightStats& stats) {
        for (usize head = 0; head < removal.size(); ++head) {
            const Node node = removal[head];
            for (u8 dir = 0; dir < 6; ++dir) {
                const i32 x = node.x + kOffsets[dir][0];
                const i32 y = node.y + kOffsets[dir][1];
                const i32 z = node.z + kOffsets[dir][2];
                Cell      cell;
                if (!locate(x, y, z, cell)) {
                    continue;
                }
                const u8 level = get(cell, kind);
                if (level == 0) {
                    continue;
                }
                const u8 info     = info_of(cell);
                const u8 expected = through(node.level, dir, info, kind);
                if (level <= expected) {
                    // Could have come from the darkened cell: darken it too,
                    // and if it gives light of its own, give it back later.
                    set(cell, kind, 0);
                    removal.push_back(Node{x, y, z, level});
                    ++stats.removed;
                    if (const u8 own = source_level(cell, y, info, kind); own > 0) {
                        reseed.push_back(Node{x, y, z, own});
                    }
                } else {
                    // Lit by something else: it fills the hole back in.
                    increase.push_back(Node{x, y, z, 0});
                }
            }
        }
    }

    void run_increase(u8 kind, LightStats& stats) {
        for (usize head = 0; head < increase.size(); ++head) {
            const Node node = increase[head];
            Cell       here;
            if (!locate(node.x, node.y, node.z, here)) {
                continue;
            }
            const u8 level = get(here, kind);
            if (level <= 1) {
                continue;
            }
            for (u8 dir = 0; dir < 6; ++dir) {
                const i32 x = node.x + kOffsets[dir][0];
                const i32 y = node.y + kOffsets[dir][1];
                const i32 z = node.z + kOffsets[dir][2];
                Cell      cell;
                if (!locate(x, y, z, cell)) {
                    continue;
                }
                const u8 given = through(level, dir, info_of(cell), kind);
                if (given == 0 || get(cell, kind) >= given) {
                    continue;
                }
                set(cell, kind, given);
                increase.push_back(Node{x, y, z, 0});
                ++stats.raised;
            }
        }
    }

    void finish() {
        std::ranges::sort(touched);
        const auto [first, last] = std::ranges::unique(touched);
        touched.erase(first, last);
        for (ChunkSection* section : touched) {
            section->sky_light().compact();
            section->block_light().compact();
        }
        touched.clear();
        source = nullptr;
    }

    LightStats propagate(LightChunkSource& chunks) {
        LightStats stats;
        stats.edits = pending.size();
        if (pending.empty()) {
            return stats;
        }
        begin(chunks);
        for (const u8 kind : {kSky, kBlock}) {
            if (kind == kSky && !rules.has_sky) {
                continue;
            }
            removal.clear();
            increase.clear();
            reseed.clear();
            for (const BlockPos pos : pending) {
                Cell cell;
                if (!locate(pos.x, pos.y, pos.z, cell)) {
                    continue;
                }
                const u8 info = info_of(cell);
                if (!skip_removal) {
                    if (const u8 old = get(cell, kind); old > 0) {
                        set(cell, kind, 0);
                        removal.push_back(Node{pos.x, pos.y, pos.z, old});
                        ++stats.removed;
                    }
                }
                if (const u8 own = source_level(cell, pos.y, info, kind); own > 0) {
                    reseed.push_back(Node{pos.x, pos.y, pos.z, own});
                }
                // A cell that let less through before lets more through now:
                // its neighbours are the ones to flood it.
                for (const auto& offset : kOffsets) {
                    increase.push_back(
                        Node{pos.x + offset[0], pos.y + offset[1], pos.z + offset[2], 0});
                }
            }
            run_removal(kind, stats);
            for (const Node& seed : reseed) {
                Cell cell;
                if (locate(seed.x, seed.y, seed.z, cell) && get(cell, kind) < seed.level) {
                    set(cell, kind, seed.level);
                    increase.push_back(seed);
                }
            }
            run_increase(kind, stats);
        }
        pending.clear();
        finish();
        return stats;
    }

    // ── A chunk on its own ──────────────────────────────────────────────────

    void seed_block_light(Chunk& chunk) {
        const WorldShape shape  = chunk.shape();
        const i32        base_x = chunk.position().min_block_x();
        const i32        base_z = chunk.position().min_block_z();
        for (usize i = 0; i < shape.section_count(); ++i) {
            const i32     bottom  = shape.min_y + static_cast<i32>(i) * 16;
            ChunkSection* section = chunk.section_for_y(bottom);
            if (section == nullptr) {
                continue;
            }
            section->block_light() = LightArray{0};
            // A palette naming no emitting state cannot seed anything. A
            // palette may list a state no cell uses any more, which only ever
            // scans a section for nothing.
            const PalettedContainer& container = section->blocks();
            if (container.kind() != PaletteKind::Direct &&
                std::ranges::none_of(container.palette(), [&](u16 value) {
                    return (info_of_state(value) & kEmissionMask) != 0;
                })) {
                continue;
            }
            for (usize index = 0; index < kLightCellCount; ++index) {
                const u8 own = static_cast<u8>(info_of_state(container.get(index)) & kEmissionMask);
                if (own == 0) {
                    continue;
                }
                const i32 x = base_x + static_cast<i32>(index & 15);
                const i32 z = base_z + static_cast<i32>((index >> 4) & 15);
                const i32 y = bottom + static_cast<i32>(index >> 8);
                section->block_light().set(index, own);
                increase.push_back(Node{x, y, z, own});
            }
        }
    }

    void seed_sky_light(Chunk& chunk) {
        const WorldShape shape  = chunk.shape();
        const i32        base_x = chunk.position().min_block_x();
        const i32        base_z = chunk.position().min_block_z();

        // Straight down each column first: 15 until something filters or
        // stops it, then one less per block. `floor` is the lowest y still at
        // 15; the cells below it that are lit at all (under water, under
        // leaves) are the column's tail, rare enough to write one by one.
        std::array<i32, 256> floor{};
        for (usize column = 0; column < 256; ++column) {
            const usize lx    = column & 15;
            const usize lz    = column >> 4;
            u8          level = kMaxLightLevel;
            i32         y     = shape.max_y();
            // Above the world is open sky at 15.
            for (; y >= shape.min_y; --y) {
                const u8 given = through(level, kDown, info_of_state(chunk.get_block(lx, y, lz).value()), kSky);
                if (given < kMaxLightLevel) {
                    break;
                }
                level = given;
            }
            floor[column] = y + 1;
        }

        for (usize i = 0; i < shape.section_count(); ++i) {
            const i32     bottom  = shape.min_y + static_cast<i32>(i) * 16;
            const i32     top     = bottom + 15;
            ChunkSection* section = chunk.section_for_y(bottom);
            if (section == nullptr) {
                continue;
            }
            const auto [low, high] = std::ranges::minmax(floor);
            if (high <= bottom) {
                section->sky_light() = LightArray{kMaxLightLevel};
            } else if (low > top) {
                section->sky_light() = LightArray{0};
            } else {
                section->sky_light() = LightArray{0};
                for (usize column = 0; column < 256; ++column) {
                    const i32 from = std::max(floor[column], bottom);
                    for (i32 y = from; y <= top; ++y) {
                        section->sky_light().set(
                            section_index(column & 15, static_cast<usize>(y - bottom), column >> 4),
                            kMaxLightLevel);
                    }
                }
            }
        }

        for (usize column = 0; column < 256; ++column) {
            const usize lx = column & 15;
            const usize lz = column >> 4;
            const i32   x  = base_x + static_cast<i32>(lx);
            const i32   z  = base_z + static_cast<i32>(lz);

            // The tail: what filtered light still reaches below the floor.
            u8 level = kMaxLightLevel;
            for (i32 y = floor[column] - 1; y >= shape.min_y && level > 1; --y) {
                level = through(level, kDown, info_of_state(chunk.get_block(lx, y, lz).value()), kSky);
                if (level == 0) {
                    break;
                }
                chunk.section_for_y(y)->sky_light().set(
                    section_index(lx, static_cast<usize>(y & 15), lz), level);
                increase.push_back(Node{x, y, z, level});
            }

            // Cells at 15 beside a column whose 15 stops higher up: the only
            // ones that give anything sideways. A cell at 15 among cells at 15
            // gives nothing, and seeding all of them was the old engine's cost.
            i32 shadow = floor[column];
            for (const auto& [dx, dz] :
                 std::array<std::pair<i32, i32>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                const i32 nx = static_cast<i32>(lx) + dx;
                const i32 nz = static_cast<i32>(lz) + dz;
                if (nx < 0 || nx > 15 || nz < 0 || nz > 15) {
                    continue;  // across the border: `stitch` decides
                }
                shadow = std::max(shadow, floor[static_cast<usize>(nz * 16 + nx)]);
            }
            for (i32 y = floor[column]; y < shadow && y <= shape.max_y(); ++y) {
                increase.push_back(Node{x, y, z, kMaxLightLevel});
            }
        }
    }

    void light_chunk(Chunk& chunk, bool keep_sky) {
        SingleChunk alone{chunk};
        LightStats  ignored;
        begin(alone);
        increase.clear();
        seed_block_light(chunk);
        run_increase(kBlock, ignored);
        if (rules.has_sky && !keep_sky) {
            increase.clear();
            seed_sky_light(chunk);
            run_increase(kSky, ignored);
        }
        const WorldShape shape = chunk.shape();
        for (usize i = 0; i < shape.section_count(); ++i) {
            if (ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16)) {
                touched.push_back(section);
            }
        }
        finish();
    }

    LightStats stitch(LightChunkSource& chunks, ChunkPos pos) {
        LightStats stats;
        begin(chunks);
        Chunk* centre = chunk_at(pos.x, pos.z);
        if (centre == nullptr) {
            finish();
            return stats;
        }
        const WorldShape shape = centre->shape();
        for (const u8 kind : {kSky, kBlock}) {
            if (kind == kSky && !rules.has_sky) {
                continue;
            }
            increase.clear();
            for (u8 dir = kWest; dir <= kSouth; ++dir) {
                Chunk* other = chunk_at(pos.x + kOffsets[dir][0], pos.z + kOffsets[dir][2]);
                if (other == nullptr) {
                    continue;
                }
                for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                    ChunkSection* mine   = centre->section_for_y(y);
                    ChunkSection* theirs = other->section_for_y(y);
                    if (mine == nullptr || theirs == nullptr) {
                        continue;
                    }
                    const LightArray& a = array_of(*mine, kind);
                    const LightArray& b = array_of(*theirs, kind);
                    if (a.is_uniform() && b.is_uniform() &&
                        (a.uniform_value() > b.uniform_value() ? a.uniform_value() - b.uniform_value()
                                                               : b.uniform_value() - a.uniform_value()) <= 1) {
                        y |= 15;  // the whole section pair: nothing crosses
                        continue;
                    }
                    const usize ly = static_cast<usize>(y & 15);
                    for (usize i = 0; i < 16; ++i) {
                        usize ax = 0, az = 0, bx = 0, bz = 0;
                        switch (dir) {
                            case kWest: ax = 0, az = i, bx = 15, bz = i; break;
                            case kEast: ax = 15, az = i, bx = 0, bz = i; break;
                            case kNorth: ax = i, az = 0, bx = i, bz = 15; break;
                            default: ax = i, az = 15, bx = i, bz = 0; break;
                        }
                        const u8 la = a.get(section_index(ax, ly, az));
                        const u8 lb = b.get(section_index(bx, ly, bz));
                        if (la > lb + 1) {
                            increase.push_back(Node{pos.min_block_x() + static_cast<i32>(ax), y,
                                                    pos.min_block_z() + static_cast<i32>(az), 0});
                        } else if (lb > la + 1) {
                            const ChunkPos o = other->position();
                            increase.push_back(Node{o.min_block_x() + static_cast<i32>(bx), y,
                                                    o.min_block_z() + static_cast<i32>(bz), 0});
                        }
                    }
                }
            }
            run_increase(kind, stats);
        }
        finish();
        return stats;
    }
};

LightEngine::LightEngine(const registry::BlockRegistry& blocks, LightRules rules)
    : impl_{std::make_unique<Impl>()} {
    impl_->blocks = &blocks;
    impl_->rules  = rules;
    impl_->table.resize(blocks.state_count());
    for (usize state = 0; state < impl_->table.size(); ++state) {
        const registry::BlockStateId id{static_cast<u16>(state)};
        u8 info = static_cast<u8>(blocks.light_emission(id) & kEmissionMask);
        switch (blocks.light_opacity(blocks.block_of(id))) {
            case registry::BlockRegistry::LightOpacity::Opaque: info |= kOpaqueBit; break;
            case registry::BlockRegistry::LightOpacity::Attenuating: info |= kFilterBit; break;
            case registry::BlockRegistry::LightOpacity::Transparent: break;
        }
        impl_->table[state] = info;
    }
}

LightEngine::~LightEngine()                                 = default;
LightEngine::LightEngine(LightEngine&&) noexcept            = default;
LightEngine& LightEngine::operator=(LightEngine&&) noexcept = default;

const LightRules& LightEngine::rules() const noexcept { return impl_->rules; }

void LightEngine::block_changed(BlockPos pos) { impl_->pending.push_back(pos); }

usize LightEngine::pending() const noexcept { return impl_->pending.size(); }

LightStats LightEngine::propagate(LightChunkSource& chunks) { return impl_->propagate(chunks); }

void LightEngine::light_chunk(Chunk& chunk, bool keep_sky) { impl_->light_chunk(chunk, keep_sky); }

LightStats LightEngine::stitch(LightChunkSource& chunks, ChunkPos pos) {
    return impl_->stitch(chunks, pos);
}

void LightEngine::light_region(LightChunkSource& chunks, std::span<const ChunkPos> positions) {
    for (const ChunkPos pos : positions) {
        if (Chunk* chunk = chunks.light_chunk(pos.x, pos.z)) {
            impl_->light_chunk(*chunk, false);
        }
    }
    for (const ChunkPos pos : positions) {
        (void)impl_->stitch(chunks, pos);
    }
}

u8 LightEngine::emission(registry::BlockStateId state) const noexcept {
    return static_cast<u8>(impl_->info_of_state(state.value()) & kEmissionMask);
}

bool LightEngine::stops_light(registry::BlockStateId state) const noexcept {
    return (impl_->info_of_state(state.value()) & kOpaqueBit) != 0;
}

void LightEngine::testing_skip_removal(bool skip) noexcept { impl_->skip_removal = skip; }

}  // namespace ov::world
