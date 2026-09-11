// The incremental light engine against a full recompute, nibble for nibble.
//
// The claim of an incremental engine is that it reaches the same fixed point a
// recompute from nothing reaches, after any sequence of edits. The only honest
// check is to make the edits — thousands of them, on real terrain — and after
// them recompute everything with code that shares nothing with the engine.
//
// The reference below is deliberately naive: dense arrays over the whole
// region, every source seeded, and a fill that processes levels from 15 down
// (a bucket queue), so that a cell is final the first time it is popped at its
// level. It is written from the documented rules only — one level lost per
// block, nothing enters a block that stops light, sky light at 15 falls without
// loss unless a filtering block is below — and knows nothing of columns,
// removal waves or borders.
//
// A control proves the comparison has teeth: the same edits with the removal
// pass switched off must disagree with the reference.
#include "ov/world/light_engine.hpp"

#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_section.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/world/heightmap.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace ov;

namespace {

const registry::BlockRegistry* registry_or_null() {
    static const std::optional<registry::BlockRegistry> blocks = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        auto loaded = registry::BlockRegistry::load(path);
        return loaded ? std::optional<registry::BlockRegistry>{std::move(*loaded)} : std::nullopt;
    }();
    return blocks ? &*blocks : nullptr;
}

registry::BlockStateId state_of(const registry::BlockRegistry& blocks, std::string_view name) {
    const auto block = blocks.find_block(name);
    REQUIRE(block.has_value());
    return blocks.default_state(*block);
}

// ── A set of loaded chunks ──────────────────────────────────────────────────

struct Loaded final : world::LightChunkSource {
    std::map<std::pair<i32, i32>, std::unique_ptr<world::Chunk>> chunks;

    world::Chunk* light_chunk(i32 x, i32 z) override {
        const auto found = chunks.find({x, z});
        return found == chunks.end() ? nullptr : found->second.get();
    }

    [[nodiscard]] std::vector<ChunkPos> positions() const {
        std::vector<ChunkPos> out;
        for (const auto& [pos, chunk] : chunks) {
            out.emplace_back(pos.first, pos.second);
        }
        return out;
    }

    void set(const registry::BlockRegistry& blocks, world::LightEngine* engine, BlockPos pos,
             registry::BlockStateId state) {
        world::Chunk* chunk = light_chunk(pos.x >> 4, pos.z >> 4);
        if (chunk == nullptr || !chunk->shape().contains_y(pos.y)) {
            return;
        }
        (void)blocks;
        chunk->set_block(static_cast<usize>(pos.x & 15), pos.y, static_cast<usize>(pos.z & 15),
                         state);
        if (engine != nullptr) {
            engine->block_changed(pos);
        }
    }
};

// ── The reference ───────────────────────────────────────────────────────────

struct Reference {
    i32             min_cx{0}, min_cz{0}, span_x{0}, span_z{0};
    i32             min_y{0}, height{0};
    std::vector<u8> sky;
    std::vector<u8> block;

    [[nodiscard]] usize index(i32 x, i32 y, i32 z) const {
        return (static_cast<usize>(y - min_y) * static_cast<usize>(span_z * 16) +
                static_cast<usize>(z - min_cz * 16)) *
                   static_cast<usize>(span_x * 16) +
               static_cast<usize>(x - min_cx * 16);
    }
};

Reference recompute(Loaded& loaded, const registry::BlockRegistry& blocks, bool filtering) {
    Reference ref;
    i32       max_cx = 0;
    i32       max_cz = 0;
    bool      first  = true;
    for (const auto& [pos, chunk] : loaded.chunks) {
        if (first) {
            ref.min_cx = max_cx = pos.first;
            ref.min_cz = max_cz = pos.second;
            ref.min_y           = chunk->shape().min_y;
            ref.height          = static_cast<i32>(chunk->shape().height);
            first               = false;
        }
        ref.min_cx = std::min(ref.min_cx, pos.first);
        ref.min_cz = std::min(ref.min_cz, pos.second);
        max_cx     = std::max(max_cx, pos.first);
        max_cz     = std::max(max_cz, pos.second);
    }
    ref.span_x       = max_cx - ref.min_cx + 1;
    ref.span_z       = max_cz - ref.min_cz + 1;
    const i32 size_x = ref.span_x * 16;
    const i32 size_z = ref.span_z * 16;
    const usize cells = static_cast<usize>(size_x) * static_cast<usize>(size_z) *
                        static_cast<usize>(ref.height);

    // Per cell: 0 absent (no chunk), else 1 + flags.
    constexpr u8     kPresent = 1, kOpaque = 2, kFilter = 4;
    std::vector<u8>  kind(cells, 0);
    std::vector<u8>  emission(cells, 0);
    for (const auto& [pos, chunk] : loaded.chunks) {
        for (i32 y = ref.min_y; y < ref.min_y + ref.height; ++y) {
            for (i32 lz = 0; lz < 16; ++lz) {
                for (i32 lx = 0; lx < 16; ++lx) {
                    const auto state = chunk->get_block(static_cast<usize>(lx), y,
                                                        static_cast<usize>(lz));
                    const auto opacity = blocks.light_opacity(blocks.block_of(state));
                    u8 flags = kPresent;
                    if (opacity == registry::BlockRegistry::LightOpacity::Opaque) {
                        flags |= kOpaque;
                    } else if (opacity == registry::BlockRegistry::LightOpacity::Attenuating) {
                        flags |= kFilter;
                    }
                    const usize at = ref.index(pos.first * 16 + lx, y, pos.second * 16 + lz);
                    kind[at]       = flags;
                    emission[at]   = blocks.light_emission(state);
                }
            }
        }
    }

    // The documented rule, and nothing else.
    const auto through = [&](u8 level, bool down, u8 flags, bool is_sky) -> u8 {
        if ((flags & kOpaque) != 0) {
            return 0;
        }
        if (is_sky && down && level == 15 && !(filtering && (flags & kFilter) != 0)) {
            return 15;
        }
        return level > 0 ? static_cast<u8>(level - 1) : u8{0};
    };

    const std::array<std::array<i32, 3>, 6> offsets{
        {{-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}}};

    for (const bool is_sky : {true, false}) {
        std::vector<u8>&                  light = is_sky ? ref.sky : ref.block;
        std::array<std::vector<usize>, 16> buckets;
        light.assign(cells, 0);
        for (i32 y = ref.min_y; y < ref.min_y + ref.height; ++y) {
            for (i32 z = 0; z < size_z; ++z) {
                for (i32 x = 0; x < size_x; ++x) {
                    const usize at = ref.index(ref.min_cx * 16 + x, y, ref.min_cz * 16 + z);
                    if (kind[at] == 0) {
                        continue;
                    }
                    u8 own = 0;
                    if (is_sky) {
                        if (y == ref.min_y + ref.height - 1) {
                            own = through(15, true, kind[at], true);
                        }
                    } else {
                        own = emission[at];
                    }
                    if (own > 0) {
                        light[at] = own;
                        buckets[own].push_back(at);
                    }
                }
            }
        }
        for (i32 level = 15; level >= 2; --level) {
            auto& bucket = buckets[static_cast<usize>(level)];
            for (usize head = 0; head < bucket.size(); ++head) {
                const usize at = bucket[head];
                if (light[at] != level) {
                    continue;  // raised since, and processed at its own level
                }
                const i32 x = static_cast<i32>(at % static_cast<usize>(size_x));
                const i32 z = static_cast<i32>((at / static_cast<usize>(size_x)) %
                                               static_cast<usize>(size_z));
                const i32 y = static_cast<i32>(at / (static_cast<usize>(size_x) *
                                                     static_cast<usize>(size_z)));
                for (usize d = 0; d < 6; ++d) {
                    const i32 nx = x + offsets[d][0];
                    const i32 ny = y + offsets[d][1];
                    const i32 nz = z + offsets[d][2];
                    if (nx < 0 || nx >= size_x || nz < 0 || nz >= size_z || ny < 0 ||
                        ny >= ref.height) {
                        continue;
                    }
                    const usize next = ref.index(ref.min_cx * 16 + nx, ref.min_y + ny,
                                                 ref.min_cz * 16 + nz);
                    if (kind[next] == 0) {
                        continue;
                    }
                    const u8 given = through(static_cast<u8>(level), d == 4, kind[next], is_sky);
                    if (given > light[next]) {
                        light[next] = given;
                        buckets[given].push_back(next);
                    }
                }
            }
        }
    }
    return ref;
}

/// Nibbles that differ between the engine's chunks and the reference, plus
/// arrays left materialised although uniform (a different packet).
usize differences(Loaded& loaded, const Reference& ref, bool has_sky = true) {
    usize count = 0;
    for (const auto& [pos, chunk] : loaded.chunks) {
        const auto shape = chunk->shape();
        for (usize s = 0; s < shape.section_count(); ++s) {
            const i32                  bottom  = shape.min_y + static_cast<i32>(s) * 16;
            const world::ChunkSection* section = chunk->section_for_y(bottom);
            for (const bool is_sky : {true, false}) {
                if (is_sky && !has_sky) {
                    continue;
                }
                const world::LightArray& array = is_sky ? section->sky_light() : section->block_light();
                world::LightArray copy = array;
                copy.compact();
                count += static_cast<usize>(copy.is_uniform() != array.is_uniform());
                for (usize i = 0; i < world::kLightCellCount; ++i) {
                    const i32   x  = pos.first * 16 + static_cast<i32>(i & 15);
                    const i32   z  = pos.second * 16 + static_cast<i32>((i >> 4) & 15);
                    const i32   y  = bottom + static_cast<i32>(i >> 8);
                    const usize at = ref.index(x, y, z);
                    count += static_cast<usize>(array.get(i) != (is_sky ? ref.sky : ref.block)[at]);
                }
            }
        }
    }
    return count;
}

// ── Terrain ─────────────────────────────────────────────────────────────────

/// A flat world of `side` x `side` chunks from (0, 0): stone up to y 0.
std::unique_ptr<Loaded> flat(const registry::BlockRegistry& blocks, i32 side) {
    auto       holder = std::make_unique<Loaded>();
    Loaded&    out    = *holder;
    const auto stone = state_of(blocks, "minecraft:stone");
    const auto air   = world::AirStates::from(blocks);
    for (i32 cz = 0; cz < side; ++cz) {
        for (i32 cx = 0; cx < side; ++cx) {
            auto chunk = std::make_unique<world::Chunk>(ChunkPos{cx, cz},
                                                        world::WorldShape::overworld(), air, &blocks);
            for (i32 y = -64; y <= 0; ++y) {
                for (usize z = 0; z < 16; ++z) {
                    for (usize x = 0; x < 16; ++x) {
                        chunk->set_block(x, y, z, stone);
                    }
                }
            }
            out.chunks.emplace(std::pair{cx, cz}, std::move(chunk));
        }
    }
    return holder;
}

/// Chunks read from a real world, read-only: `side` x `side` from
/// (`first_x`, `first_z`). Missing chunks are left out, and are a hole the
/// engine has to respect like any unloaded neighbour.
std::unique_ptr<Loaded> real(const registry::BlockRegistry& blocks, const std::filesystem::path& dir,
                           i32 first_x, i32 first_z, i32 side) {
    std::vector<std::string_view> biome_names(blocks.biome_count());
    for (u32 index = 0; index < blocks.biome_count(); ++index) {
        biome_names[index] = blocks.biome_name(index);
    }
    world::ChunkCodecContext context;
    context.blocks      = &blocks;
    context.biome_names = biome_names;
    context.air         = world::AirStates::from(blocks);

    auto                                                 holder = std::make_unique<Loaded>();
    Loaded&                                              out    = *holder;
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions;
    for (i32 cz = first_z; cz < first_z + side; ++cz) {
        for (i32 cx = first_x; cx < first_x + side; ++cx) {
            const ChunkPos pos{cx, cz};
            const auto     key = std::pair{pos.region_x(), pos.region_z()};
            if (!regions.contains(key)) {
                const auto path = dir / "region" /
                                  ("r." + std::to_string(key.first) + "." +
                                   std::to_string(key.second) + ".mca");
                auto opened = nbt::RegionFile::open(path);
                regions[key] = opened ? std::optional<nbt::RegionFile>{std::move(*opened)}
                                      : std::nullopt;
            }
            const auto& region = regions[key];
            if (!region || !region->has_chunk(pos)) {
                continue;
            }
            const auto document = region->read_chunk(pos);
            if (!document) {
                continue;
            }
            auto chunk = world::from_nbt(*document, context);
            if (!chunk || chunk->non_air_count() == 0) {
                continue;
            }
            out.chunks.emplace(std::pair{cx, cz}, std::make_unique<world::Chunk>(std::move(*chunk)));
        }
    }
    if (out.chunks.size() < 4) {
        return nullptr;
    }
    return holder;
}

// ── Random edits ────────────────────────────────────────────────────────────

struct Palette {
    std::vector<registry::BlockStateId> states;

    explicit Palette(const registry::BlockRegistry& blocks) {
        for (const char* name :
             {"minecraft:air", "minecraft:air", "minecraft:air", "minecraft:stone",
              "minecraft:stone", "minecraft:glass", "minecraft:oak_leaves", "minecraft:water",
              "minecraft:ice", "minecraft:torch", "minecraft:glowstone", "minecraft:sea_lantern",
              "minecraft:lava", "minecraft:redstone_torch", "minecraft:magma_block",
              "minecraft:oak_fence"}) {
            states.push_back(state_of(blocks, name));
        }
    }
};

/// One batch of edits: usually one block near the surface, sometimes a shaft
/// dug or capped (the sky openings), sometimes a scatter of several.
void random_batch(Loaded& loaded, const registry::BlockRegistry& blocks, world::LightEngine& engine,
                  const Palette& palette, std::mt19937& random) {
    const auto positions = loaded.positions();
    const auto pick_column = [&](i32& x, i32& z, world::Chunk*& chunk) {
        const ChunkPos at = positions[random() % positions.size()];
        x                 = at.min_block_x() + static_cast<i32>(random() % 16);
        z                 = at.min_block_z() + static_cast<i32>(random() % 16);
        chunk             = loaded.light_chunk(at.x, at.z);
    };
    const auto near_surface = [&](world::Chunk& chunk, i32 x, i32 z) {
        const i32 surface = chunk.heightmap(world::HeightmapType::WorldSurface)
                                .first_free(static_cast<usize>(x & 15), static_cast<usize>(z & 15));
        if (random() % 5 == 0) {
            return chunk.shape().min_y + static_cast<i32>(random() % chunk.shape().height);
        }
        return std::clamp(surface - 8 + static_cast<i32>(random() % 12), chunk.shape().min_y,
                          chunk.shape().max_y());
    };

    const u32 roll = random() % 100;
    i32 x = 0, z = 0;
    world::Chunk* chunk = nullptr;
    pick_column(x, z, chunk);
    const auto air = world::AirStates::from(blocks).air;
    if (roll < 8) {
        // A shaft dug from the surface down, or capped: opens or closes the sky.
        const i32  top   = near_surface(*chunk, x, z) + 4;
        const i32  depth = 3 + static_cast<i32>(random() % 20);
        const bool dig   = random() % 2 == 0;
        for (i32 y = top; y > top - depth; --y) {
            loaded.set(blocks, &engine, BlockPos{x, y, z},
                       dig ? air : palette.states[3 + random() % 3]);
        }
    } else if (roll < 16) {
        // A scatter: several edits in one batch, as a tick's drain makes.
        const usize count = 2 + random() % 12;
        for (usize i = 0; i < count; ++i) {
            const i32 dx = static_cast<i32>(random() % 9) - 4;
            const i32 dz = static_cast<i32>(random() % 9) - 4;
            world::Chunk* there = loaded.light_chunk((x + dx) >> 4, (z + dz) >> 4);
            if (there == nullptr) {
                continue;
            }
            loaded.set(blocks, &engine, BlockPos{x + dx, near_surface(*there, x + dx, z + dz), z + dz},
                       palette.states[random() % palette.states.size()]);
        }
    } else {
        loaded.set(blocks, &engine, BlockPos{x, near_surface(*chunk, x, z), z},
                   palette.states[random() % palette.states.size()]);
    }
}

struct Timing {
    std::vector<double> micros;

    void print(const char* what) {
        if (micros.empty()) {
            return;
        }
        std::ranges::sort(micros);
        const auto at = [&](double q) {
            return micros[std::min(micros.size() - 1, static_cast<usize>(q * micros.size()))];
        };
        std::printf("%s: %zu batches, propagate p50 %.1f us, p99 %.1f us, max %.1f us\n", what,
                    micros.size(), at(0.5), at(0.99), micros.back());
    }
};

/// Light `loaded` from nothing, check it, then make `batches` random batches
/// of edits, checking against the reference every `every` batches. Returns
/// the differences found in total.
usize run_edits(Loaded& loaded, const registry::BlockRegistry& blocks, world::LightRules rules,
                u32 seed, usize batches, usize every, bool skip_removal, Timing* timing) {
    world::LightEngine engine{blocks, rules};
    const auto         positions = loaded.positions();
    engine.light_region(loaded, positions);
    usize found = differences(loaded, recompute(loaded, blocks, rules.filtering_dims_sky),
                              rules.has_sky);
    INFO("the region lit from nothing");
    CHECK(found == 0);

    engine.testing_skip_removal(skip_removal);
    const Palette palette{blocks};
    std::mt19937  random{seed};
    for (usize batch = 1; batch <= batches; ++batch) {
        random_batch(loaded, blocks, engine, palette, random);
        const auto started = std::chrono::steady_clock::now();
        (void)engine.propagate(loaded);
        if (timing != nullptr) {
            timing->micros.push_back(std::chrono::duration<double, std::micro>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
        }
        if (batch % every == 0 || batch == batches) {
            found += differences(loaded, recompute(loaded, blocks, rules.filtering_dims_sky),
                                 rules.has_sky);
            if (found != 0 && !skip_removal) {
                UNSCOPED_INFO("first difference after batch " << batch);
                break;
            }
        }
    }
    return found;
}

}  // namespace

// ── Small cases, each one thing ─────────────────────────────────────────────

TEST_CASE("a torch lights its surroundings across a chunk border and goes out when broken",
          "[light]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    auto               holder = flat(*blocks, 2);
    Loaded&            loaded = *holder;
    world::LightEngine engine{*blocks};
    engine.light_region(loaded, loaded.positions());

    const auto torch = state_of(*blocks, "minecraft:torch");
    const auto air   = world::AirStates::from(*blocks).air;
    const auto block_light = [&](i32 x, i32 y, i32 z) {
        return loaded.light_chunk(x >> 4, z >> 4)
            ->section_for_y(y)
            ->block_light()
            .get(world::section_index(static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                      static_cast<usize>(z & 15)));
    };

    loaded.set(*blocks, &engine, BlockPos{15, 1, 4}, torch);  // against the east border
    (void)engine.propagate(loaded);
    CHECK(block_light(15, 1, 4) == 14);
    CHECK(block_light(16, 1, 4) == 13);  // in the next chunk
    CHECK(block_light(20, 1, 4) == 9);
    CHECK(differences(loaded, recompute(loaded, *blocks, false)) == 0);

    loaded.set(*blocks, &engine, BlockPos{15, 1, 4}, air);
    const auto stats = engine.propagate(loaded);
    CHECK(stats.removed > 0);
    CHECK(block_light(16, 1, 4) == 0);
    CHECK(differences(loaded, recompute(loaded, *blocks, false)) == 0);
}

TEST_CASE("a shaft opened to the sky fills with 15 and a roof puts it back in shade", "[light]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    for (const bool filtering : {false, true}) {
        CAPTURE(filtering);
        auto               holder = flat(*blocks, 3);
        Loaded&            loaded = *holder;
        world::LightEngine engine{*blocks, world::LightRules{true, filtering}};
        engine.light_region(loaded, loaded.positions());
        const auto air   = world::AirStates::from(*blocks).air;
        const auto stone = state_of(*blocks, "minecraft:stone");
        const auto water = state_of(*blocks, "minecraft:water");

        for (i32 y = 0; y >= -30; --y) {
            loaded.set(*blocks, &engine, BlockPos{24, y, 24}, air);
        }
        (void)engine.propagate(loaded);
        CHECK(differences(loaded, recompute(loaded, *blocks, filtering)) == 0);

        loaded.set(*blocks, &engine, BlockPos{24, 0, 24}, water);  // a filtering cap
        (void)engine.propagate(loaded);
        CHECK(differences(loaded, recompute(loaded, *blocks, filtering)) == 0);

        loaded.set(*blocks, &engine, BlockPos{24, 0, 24}, stone);
        (void)engine.propagate(loaded);
        CHECK(differences(loaded, recompute(loaded, *blocks, filtering)) == 0);

        // The top of the world itself.
        loaded.set(*blocks, &engine, BlockPos{3, 319, 3}, stone);
        loaded.set(*blocks, &engine, BlockPos{40, 319, 7}, water);
        (void)engine.propagate(loaded);
        CHECK(differences(loaded, recompute(loaded, *blocks, filtering)) == 0);
        loaded.set(*blocks, &engine, BlockPos{3, 319, 3}, air);
        (void)engine.propagate(loaded);
        CHECK(differences(loaded, recompute(loaded, *blocks, filtering)) == 0);
    }
}

TEST_CASE("chunks lit alone and stitched as they arrive reach the region's fixed point",
          "[light]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    auto         holder    = flat(*blocks, 3);
    Loaded&      full      = *holder;
    const auto   glowstone = state_of(*blocks, "minecraft:glowstone");
    const auto   stone     = state_of(*blocks, "minecraft:stone");
    std::mt19937 random{11};
    for (int i = 0; i < 200; ++i) {
        const i32 x = static_cast<i32>(random() % 48);
        const i32 z = static_cast<i32>(random() % 48);
        const i32 y = 1 + static_cast<i32>(random() % 8);
        full.set(*blocks, nullptr, BlockPos{x, y, z}, random() % 4 == 0 ? glowstone : stone);
    }
    Loaded arriving;
    world::LightEngine engine{*blocks};
    // In an order that leaves holes until the end.
    for (const auto& [cx, cz] : std::array<std::pair<i32, i32>, 9>{
             {{0, 0}, {2, 2}, {2, 0}, {0, 2}, {1, 1}, {1, 0}, {0, 1}, {2, 1}, {1, 2}}}) {
        world::Chunk& chunk = *full.chunks.at({cx, cz});
        auto          moved = std::make_unique<world::Chunk>(std::move(chunk));
        engine.light_chunk(*moved);
        arriving.chunks.emplace(std::pair{cx, cz}, std::move(moved));
        (void)engine.stitch(arriving, ChunkPos{cx, cz});
        CHECK(differences(arriving, recompute(arriving, *blocks, false)) == 0);
    }
}

TEST_CASE("a dimension without sky keeps block light only", "[light]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    auto               holder = flat(*blocks, 2);
    Loaded&            loaded = *holder;
    world::LightEngine engine{*blocks, world::LightRules{false, false}};
    engine.light_region(loaded, loaded.positions());
    for (const auto& [pos, chunk] : loaded.chunks) {
        for (const auto& section : chunk->sections()) {
            CHECK(section.sky_light().is_absent());
        }
    }
    loaded.set(*blocks, &engine, BlockPos{8, 1, 8}, state_of(*blocks, "minecraft:glowstone"));
    (void)engine.propagate(loaded);
    CHECK(differences(loaded, recompute(loaded, *blocks, false), false) == 0);
}

// ── Thousands of edits on generated-looking terrain and real worlds ──────────

TEST_CASE("random edits on flat terrain stay at the fixed point, and the control does not",
          "[light]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    for (const bool filtering : {false, true}) {
        CAPTURE(filtering);
        auto honest = flat(*blocks, 3);
        CHECK(run_edits(*honest, *blocks, world::LightRules{true, filtering}, 5, 300, 1, false,
                        nullptr) == 0);
    }
    // The control: without the removal pass, a torch broken keeps shining.
    auto control = flat(*blocks, 3);
    CHECK(run_edits(*control, *blocks, world::LightRules{}, 5, 600, 100, true, nullptr) > 0);
}

namespace {

/// Random edits on the two real worlds under both rules, and the control.
void real_world_edits(const registry::BlockRegistry* blocks, usize batches, usize every) {
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    struct World {
        const char* name;
        std::filesystem::path dir;
        i32 first_x;
        i32 first_z;
    };
    const std::filesystem::path run = std::filesystem::path{OV_SOURCE_DIR} / "run";
    const std::array<World, 2>  worlds{{
        {"ov_lab bench world", run / "lab", 0, 0},
        {"a real 1.20.1 world", run / "saves" / "New World", -2, -2},
    }};
    usize tried = 0;
    for (const World& source : worlds) {
        auto loaded = real(*blocks, source.dir, source.first_x, source.first_z, 4);
        if (!loaded) {
            WARN("no chunks under " << source.dir.string());
            continue;
        }
        ++tried;
        for (const bool filtering : {false, true}) {
            CAPTURE(source.name, filtering, loaded->chunks.size());
            Timing timing;
            CHECK(run_edits(*loaded, *blocks, world::LightRules{true, filtering}, 17, batches,
                            every, false, &timing) == 0);
            timing.print(source.name);
        }
        // The control on the same terrain.
        auto control = real(*blocks, source.dir, source.first_x, source.first_z, 4);
        const usize control_batches = std::min<usize>(batches, 400);
        CHECK(run_edits(*control, *blocks, world::LightRules{}, 17, control_batches,
                        control_batches, true, nullptr) > 0);
    }
    if (tried == 0) {
        SKIP("no real world under run/");
    }
}

}  // namespace

// Sized for the unit suite every agent and CI runs: a few minutes in Debug.
TEST_CASE("random edits on real worlds match a full recompute", "[light][real]") {
    real_world_edits(registry_or_null(), 500, 100);
}

// The documented measurement (docs/provenance/incremental-light.md § 6.1):
// 2000 batches per world and rule. Over twenty minutes in Debug, so hidden:
// `test_ov_world "[.light-real-thousands]"`.
TEST_CASE("thousands of random edits on real worlds match a full recompute",
          "[.light-real-thousands]") {
    real_world_edits(registry_or_null(), 2000, 200);
}

// ── After an edit, against the real server ─────────────────────────────────
//
// A measurement: `test_ov_world "[.light-vanilla-edits]"`, after
// scripts/measure_light_edits.py has written `.scratch/light-lab/{before,after}`.
// Our light over `before`, the edits found by comparing the blocks of the two
// saves and applied incrementally, then every cell whose light vanilla changed
// between the two saves is compared with ours. The control is our light before
// the edits: on those same cells it must be wrong.
TEST_CASE("the light after an edit against what the real server wrote", "[.light-vanilla-edits]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    const auto lab = std::filesystem::path{OV_SOURCE_DIR} / ".scratch" / "light-lab";
    constexpr i32 kFirst = -2;
    constexpr i32 kSide  = 8;
    for (const bool filtering : {false, true}) {
        auto before = real(*blocks, lab / "before", kFirst, kFirst, kSide);
        auto after  = real(*blocks, lab / "after", kFirst, kFirst, kSide);
        if (!before || !after) {
            SKIP("run scripts/measure_light_edits.py first");
        }
        auto ours = real(*blocks, lab / "before", kFirst, kFirst, kSide);
        world::LightEngine engine{*blocks, world::LightRules{true, filtering}};
        engine.light_region(*ours, ours->positions());
        // Our light before the edit, kept for the control.
        auto control = real(*blocks, lab / "before", kFirst, kFirst, kSide);
        engine.light_region(*control, control->positions());

        usize edits = 0;
        for (const auto& [pos, chunk] : after->chunks) {
            world::Chunk* mine = ours->light_chunk(pos.first, pos.second);
            if (mine == nullptr) {
                continue;
            }
            const auto shape = chunk->shape();
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                for (usize z = 0; z < 16; ++z) {
                    for (usize x = 0; x < 16; ++x) {
                        const auto state = chunk->get_block(x, y, z);
                        if (mine->get_block(x, y, z) != state) {
                            ours->set(*blocks, &engine,
                                      BlockPos{pos.first * 16 + static_cast<i32>(x), y,
                                               pos.second * 16 + static_cast<i32>(z)},
                                      state);
                            ++edits;
                        }
                    }
                }
            }
        }
        const auto stats = engine.propagate(*ours);

        std::array<usize, 2> changed{}, same{}, control_same{};
        for (const auto& [pos, chunk] : after->chunks) {
            bool inner = true;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    inner = inner && after->chunks.contains({pos.first + dx, pos.second + dz}) &&
                            ours->chunks.contains({pos.first + dx, pos.second + dz});
                }
            }
            if (!inner) {
                continue;
            }
            const world::Chunk& was = *before->chunks.at(pos);
            const world::Chunk& now = *chunk;
            const world::Chunk& us  = *ours->chunks.at(pos);
            const world::Chunk& ctl = *control->chunks.at(pos);
            const auto          shape = now.shape();
            for (usize s = 0; s < shape.section_count(); ++s) {
                const i32 bottom = shape.min_y + static_cast<i32>(s) * 16;
                for (usize kind = 0; kind < 2; ++kind) {
                    const auto array = [&](const world::Chunk& c) -> const world::LightArray& {
                        const world::ChunkSection& section = *c.section_for_y(bottom);
                        return kind == 0 ? section.sky_light() : section.block_light();
                    };
                    // A section vanilla did not store sky light for is not
                    // compared: absent there means "not written", not dark.
                    if (kind == 0 && (array(was).is_absent() || array(now).is_absent())) {
                        continue;
                    }
                    for (usize i = 0; i < world::kLightCellCount; ++i) {
                        if (array(was).get(i) == array(now).get(i)) {
                            continue;
                        }
                        ++changed[kind];
                        same[kind] += static_cast<usize>(array(us).get(i) == array(now).get(i));
                        control_same[kind] +=
                            static_cast<usize>(array(ctl).get(i) == array(now).get(i));
                    }
                }
            }
        }
        std::printf("filtering_dims_sky=%d: %zu edits found, %zu cells darkened, %zu raised\n",
                    filtering ? 1 : 0, edits, stats.removed, stats.raised);
        std::printf("  sky light:   %zu/%zu cells vanilla changed are identical (control %zu/%zu)\n",
                    same[0], changed[0], control_same[0], changed[0]);
        std::printf("  block light: %zu/%zu cells vanilla changed are identical (control %zu/%zu)\n",
                    same[1], changed[1], control_same[1], changed[1]);
    }
    SUCCEED();
}

// ── Against the light vanilla itself stored ────────────────────────────────
//
// A measurement, not a test: run with `test_ov_world "[.light-vanilla]"`. A
// world written by the real 1.20.1 server carries the light the game computed.
// Our engine relights a copy from the blocks alone, and every cell of every
// chunk whose eight neighbours are loaded (so that nothing from outside the
// patch could have reached it) is compared with what vanilla wrote.
//
// Not `run/saves/New World` itself: every chunk of it is stored with
// `isLightOn` unset — the game never finished lighting it and relights it on
// load, so its light is no oracle. The world vanilla itself saved after
// loading and relighting it is: `.scratch/light-lab/before`, written by
// scripts/measure_light_edits.py.
TEST_CASE("our light over a real 1.20.1 world against the light vanilla stored",
          "[.light-vanilla]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    const auto dir = std::filesystem::path{OV_SOURCE_DIR} / ".scratch" / "light-lab" / "before";
    constexpr i32 kFirst = -4;
    constexpr i32 kSide  = 8;
    auto stored = real(*blocks, dir, kFirst, kFirst, kSide);
    if (!stored) {
        SKIP("no real world under run/saves");
    }
    for (const bool filtering : {false, true}) {
        auto ours = real(*blocks, dir, kFirst, kFirst, kSide);
        world::LightEngine engine{*blocks, world::LightRules{true, filtering}};
        engine.light_region(*ours, ours->positions());

        usize sky_same = 0, sky_total = 0, block_same = 0, block_total = 0;
        usize absent_sections = 0, absent_but_lit = 0, chunks_compared = 0;
        std::map<std::string, usize> sky_by_block;
        std::map<std::string, usize> block_by_block;
        std::map<int, usize>         sky_delta;
        for (const auto& [pos, chunk] : ours->chunks) {
            bool inner = true;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    inner = inner && ours->chunks.contains({pos.first + dx, pos.second + dz});
                }
            }
            if (!inner) {
                continue;
            }
            ++chunks_compared;
            const world::Chunk& theirs = *stored->chunks.at(pos);
            const auto          shape  = chunk->shape();
            for (usize s = 0; s < shape.section_count(); ++s) {
                const i32 bottom = shape.min_y + static_cast<i32>(s) * 16;
                const world::ChunkSection& mine  = *chunk->section_for_y(bottom);
                const world::ChunkSection& other = *theirs.section_for_y(bottom);
                const bool sky_stored = !other.sky_light().is_absent();
                if (!sky_stored) {
                    ++absent_sections;
                }
                for (usize i = 0; i < world::kLightCellCount; ++i) {
                    const auto name = [&] {
                        const auto state = mine.blocks().get(i);
                        return std::string{blocks->block_name(
                            blocks->block_of(registry::BlockStateId{state}))};
                    };
                    if (sky_stored) {
                        ++sky_total;
                        const int a = mine.sky_light().get(i);
                        const int b = other.sky_light().get(i);
                        if (a == b) {
                            ++sky_same;
                        } else {
                            ++sky_by_block[name()];
                            ++sky_delta[a - b];
                        }
                    } else if (mine.sky_light().get(i) != 0) {
                        ++absent_but_lit;
                    }
                    ++block_total;
                    if (mine.block_light().get(i) == other.block_light().get(i)) {
                        ++block_same;
                    } else {
                        ++block_by_block[name()];
                    }
                }
            }
        }
        std::printf("filtering_dims_sky=%d over %zu inner chunks:\n", filtering ? 1 : 0,
                    chunks_compared);
        std::printf("  sky light   %zu/%zu identical (%zu differ) in sections vanilla stored\n",
                    sky_same, sky_total, sky_total - sky_same);
        std::printf("  block light %zu/%zu identical (%zu differ)\n", block_same, block_total,
                    block_total - block_same);
        std::printf("  %zu sections with no stored sky light; %zu of their cells are lit by us\n",
                    absent_sections, absent_but_lit);
        const auto top = [](const std::map<std::string, usize>& counts, const char* what) {
            std::vector<std::pair<usize, std::string>> sorted;
            for (const auto& [name, count] : counts) {
                sorted.emplace_back(count, name);
            }
            std::ranges::sort(sorted, std::greater<>{});
            std::printf("  %s differences by block at the cell:", what);
            for (usize i = 0; i < std::min<usize>(sorted.size(), 10); ++i) {
                std::printf(" %s %zu,", sorted[i].second.c_str(), sorted[i].first);
            }
            std::printf("\n");
        };
        top(sky_by_block, "sky");
        top(block_by_block, "block");
        std::printf("  sky (ours - vanilla):");
        for (const auto& [delta, count] : sky_delta) {
            std::printf(" %+d:%zu", delta, count);
        }
        std::printf("\n");
    }
    SUCCEED();
}
