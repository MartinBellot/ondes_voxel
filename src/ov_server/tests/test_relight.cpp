// The light engine after its rewrite for speed, against the engine it
// replaced, cell for cell.
//
// The rewrite changed *which* cells seed the sky flood fill (only lit cells
// beside an unlit column) and *which* sections are scanned for block light
// (only those whose palette names an emitting state). Both are claims that the
// result cannot change, and the only honest check of such a claim is to run
// the old code beside the new on the same terrain and compare every nibble.
// The old code lives below, as it stood in server.cpp before the move.
#include "../src/relight.hpp"

#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_section.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/world/heightmap.hpp"
#include "ov/world/light_engine.hpp"

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
using namespace ov::server;

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

// ── The previous engine, verbatim but for the registry parameter ───────────

namespace reference {

i32 sky_floor(const registry::BlockRegistry* blocks, const world::Chunk& chunk, usize x, usize z,
              i32 top) {
    for (i32 y = top; y >= chunk.shape().min_y; --y) {
        if (stops_sky_light(blocks, chunk.get_block(x, y, z))) {
            return y + 1;
        }
    }
    return chunk.shape().min_y;
}

void relight_blocks(world::Chunk& chunk, const registry::BlockRegistry& blocks) {
    const auto shape = chunk.shape();
    struct Cell {
        u8  x;
        i32 y;
        u8  z;
    };
    const auto light_at = [&](usize x, i32 y, usize z) -> u8 {
        const world::ChunkSection* section = chunk.section_for_y(y);
        return section == nullptr ? 0
                                  : section->block_light().get(
                                        world::section_index(x, static_cast<usize>(y & 15), z));
    };
    const auto set_light = [&](usize x, i32 y, usize z, u8 value) {
        world::ChunkSection* section = chunk.section_for_y(y);
        if (section != nullptr) {
            section->block_light().set(world::section_index(x, static_cast<usize>(y & 15), z),
                                       value);
        }
    };
    std::vector<Cell> frontier;
    for (usize i = 0; i < shape.section_count(); ++i) {
        world::ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16);
        if (section == nullptr) {
            continue;
        }
        for (usize index = 0;
             index < world::kSectionSize * world::kSectionSize * world::kSectionSize; ++index) {
            section->block_light().set(index, 0);
        }
    }
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                const u8 emission = blocks.light_emission(chunk.get_block(x, y, z));
                if (emission > 0) {
                    set_light(x, y, z, emission);
                    frontier.push_back(Cell{static_cast<u8>(x), y, static_cast<u8>(z)});
                }
            }
        }
    }
    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8                  spread = static_cast<u8>(current - 1);
        const std::array<Cell, 6> neighbours{{
            {static_cast<u8>(cell.x - 1), cell.y, cell.z},
            {static_cast<u8>(cell.x + 1), cell.y, cell.z},
            {cell.x, cell.y, static_cast<u8>(cell.z - 1)},
            {cell.x, cell.y, static_cast<u8>(cell.z + 1)},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};
        for (const Cell& next : neighbours) {
            if (next.x >= 16 || next.z >= 16 || next.y < shape.min_y || next.y > shape.max_y()) {
                continue;
            }
            if (stops_sky_light(&blocks, chunk.get_block(next.x, next.y, next.z))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }
    for (usize i = 0; i < shape.section_count(); ++i) {
        world::ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16);
        if (section != nullptr) {
            section->block_light().compact();
        }
    }
}

void relight_neighbourhood(const ChunkLookup& lookup, i32 centre_x, i32 centre_z,
                           const registry::BlockRegistry* blocks) {
    const auto shape = world::WorldShape::overworld();
    struct Loaded {
        world::Chunk* chunk;
        i32           origin_x;
        i32           origin_z;
    };
    std::vector<Loaded> loaded;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (world::Chunk* chunk = lookup(centre_x + dx, centre_z + dz)) {
                loaded.push_back(Loaded{chunk, (centre_x + dx) * 16, (centre_z + dz) * 16});
            }
        }
    }
    if (loaded.empty()) {
        return;
    }
    const auto chunk_for = [&](i32 x, i32 z) -> world::Chunk* {
        for (const Loaded& entry : loaded) {
            if (x >= entry.origin_x && x < entry.origin_x + 16 && z >= entry.origin_z &&
                z < entry.origin_z + 16) {
                return entry.chunk;
            }
        }
        return nullptr;
    };
    i32 top = shape.min_y;
    for (const Loaded& entry : loaded) {
        const auto& surface = entry.chunk->heightmap(world::HeightmapType::WorldSurface);
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                top = std::max(top, surface.first_free(x, z));
            }
        }
    }
    top = std::min(top + 1, shape.max_y());
    const auto light_at = [&](i32 x, i32 y, i32 z) -> u8 {
        world::Chunk* chunk = chunk_for(x, z);
        if (chunk == nullptr) {
            return 0;
        }
        const world::ChunkSection* section = chunk->section_for_y(y);
        return section == nullptr ? 0
                                  : section->sky_light().get(world::section_index(
                                        static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                        static_cast<usize>(z & 15)));
    };
    const auto set_light = [&](i32 x, i32 y, i32 z, u8 value) {
        world::Chunk* chunk = chunk_for(x, z);
        if (chunk == nullptr) {
            return;
        }
        world::ChunkSection* section = chunk->section_for_y(y);
        if (section != nullptr) {
            section->sky_light().set(
                world::section_index(static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                     static_cast<usize>(z & 15)),
                value);
        }
    };
    struct Cell {
        i32 x;
        i32 y;
        i32 z;
    };
    std::vector<Cell> frontier;
    for (const Loaded& entry : loaded) {
        for (usize lz = 0; lz < 16; ++lz) {
            for (usize lx = 0; lx < 16; ++lx) {
                const i32 first_free = sky_floor(blocks, *entry.chunk, lx, lz, top);
                const i32 x          = entry.origin_x + static_cast<i32>(lx);
                const i32 z          = entry.origin_z + static_cast<i32>(lz);
                for (i32 y = shape.min_y; y <= top; ++y) {
                    const bool lit = y >= first_free;
                    set_light(x, y, z, lit ? world::kMaxLightLevel : 0);
                    if (lit) {
                        frontier.push_back(Cell{x, y, z});
                    }
                }
            }
        }
    }
    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8                  spread = static_cast<u8>(current - 1);
        const std::array<Cell, 6> neighbours{{
            {cell.x - 1, cell.y, cell.z},
            {cell.x + 1, cell.y, cell.z},
            {cell.x, cell.y, cell.z - 1},
            {cell.x, cell.y, cell.z + 1},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};
        for (const Cell& next : neighbours) {
            if (next.y < shape.min_y || next.y > top) {
                continue;
            }
            world::Chunk* chunk = chunk_for(next.x, next.z);
            if (chunk == nullptr) {
                continue;
            }
            if (stops_sky_light(blocks,
                                chunk->get_block(static_cast<usize>(next.x & 15), next.y,
                                                 static_cast<usize>(next.z & 15)))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }
    for (const Loaded& entry : loaded) {
        for (usize i = 0; i < shape.section_count(); ++i) {
            world::ChunkSection* section =
                entry.chunk->section_for_y(shape.min_y + static_cast<i32>(i) * 16);
            if (section != nullptr) {
                section->sky_light().compact();
            }
        }
    }
}

}  // namespace reference

// ── Terrain ──────────────────────────────────────────────────────────────────

/// A 3x3 of chunks around (0, 0) with everything light has to get right:
/// columns of different heights, roofs and overhangs, glass, leaves, caves
/// with glowstone and lava in them, torches on the ground. Deterministic, so
/// two calls with one seed build the same world twice.
using World = std::map<std::pair<i32, i32>, world::Chunk>;

World build(const registry::BlockRegistry& blocks, u32 seed, bool with_hole) {
    const auto state = [&](std::string_view name) {
        const auto block = blocks.find_block(name);
        REQUIRE(block.has_value());
        return blocks.default_state(*block);
    };
    const auto stone     = state("minecraft:stone");
    const auto glass     = state("minecraft:glass");
    const auto leaves    = state("minecraft:oak_leaves");
    const auto glowstone = state("minecraft:glowstone");
    const auto torch     = state("minecraft:torch");
    const auto lava      = state("minecraft:lava");
    const auto air       = world::AirStates::from(blocks);

    std::mt19937 random{seed};
    World        out;
    for (i32 cz = -1; cz <= 1; ++cz) {
        for (i32 cx = -1; cx <= 1; ++cx) {
            if (with_hole && cx == 1 && cz == 0) {
                continue;  // a neighbour that is not loaded
            }
            world::Chunk chunk{ChunkPos{cx, cz}, world::WorldShape::overworld(), air, &blocks};
            for (usize z = 0; z < 16; ++z) {
                for (usize x = 0; x < 16; ++x) {
                    const i32 height = -20 + static_cast<i32>(random() % 40);
                    for (i32 y = -64; y <= height; ++y) {
                        chunk.set_block(x, y, z, stone);
                    }
                    const u32 roll = random() % 100;
                    if (roll < 12) {  // a roof, with a gap under it
                        chunk.set_block(x, height + 4, z, stone);
                        chunk.set_block(x, height + 5, z, stone);
                    } else if (roll < 16) {
                        chunk.set_block(x, height + 3, z, glass);
                    } else if (roll < 22) {
                        chunk.set_block(x, height + 2, z, leaves);
                    } else if (roll < 26) {
                        chunk.set_block(x, height + 1, z, torch);
                    }
                    if (random() % 9 == 0) {  // a cave pocket, sometimes lit
                        const i32 floor = -50 + static_cast<i32>(random() % 20);
                        for (i32 y = floor; y < floor + 4; ++y) {
                            chunk.set_block(x, y, z, air.cave_air);
                        }
                        const u32 light = random() % 6;
                        if (light == 0) {
                            chunk.set_block(x, floor, z, glowstone);
                        } else if (light == 1) {
                            chunk.set_block(x, floor, z, lava);
                        }
                    }
                }
            }
            relight_chunk(chunk, &blocks);
            out.emplace(std::pair{cx, cz}, std::move(chunk));
        }
    }
    return out;
}

// Parameters are never called `world`: that would hide the namespace, and
// `world::Chunk` in their scope would stop naming a type.
ChunkLookup lookup_in(World& loaded) {
    return [&loaded](i32 cx, i32 cz) -> world::Chunk* {
        const auto it = loaded.find({cx, cz});
        return it == loaded.end() ? nullptr : &it->second;
    };
}

void relight_with_reference(World& loaded, const registry::BlockRegistry& blocks) {
    const ChunkLookup lookup = lookup_in(loaded);
    reference::relight_neighbourhood(lookup, 0, 0, &blocks);
    for (auto& [pos, chunk] : loaded) {
        reference::relight_blocks(chunk, blocks);
    }
}

/// Every nibble of both arrays of every section, and whether each array ended
/// up materialised: a representation that differs is a different packet.
usize differences(const World& a, const World& b) {
    usize count = 0;
    for (const auto& [pos, chunk] : a) {
        const world::Chunk& other = b.at(pos);
        const auto          ours  = chunk.sections();
        const auto          theirs = other.sections();
        REQUIRE(ours.size() == theirs.size());
        for (usize s = 0; s < ours.size(); ++s) {
            count += static_cast<usize>(ours[s].sky_light().is_uniform() !=
                                        theirs[s].sky_light().is_uniform());
            count += static_cast<usize>(ours[s].block_light().is_uniform() !=
                                        theirs[s].block_light().is_uniform());
            for (usize i = 0; i < world::kLightCellCount; ++i) {
                count += static_cast<usize>(ours[s].sky_light().get(i) !=
                                            theirs[s].sky_light().get(i));
                count += static_cast<usize>(ours[s].block_light().get(i) !=
                                            theirs[s].block_light().get(i));
            }
        }
    }
    return count;
}

}  // namespace

TEST_CASE("the rewritten relight matches the previous engine nibble for nibble", "[relight]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }

    for (const u32 seed : {1U, 2U, 3U}) {
        for (const bool hole : {false, true}) {
            CAPTURE(seed, hole);
            World before = build(*blocks, seed, hole);
            World after  = build(*blocks, seed, hole);
            REQUIRE(differences(before, after) == 0);

            relight_with_reference(before, *blocks);
            relight_after_edit(lookup_in(after), 0, 0, blocks);
            CHECK(differences(before, after) == 0);

            // And after an edit: a shaft dug through the middle, a roof put
            // over its mouth, and a torch at the bottom — the three things a
            // player does that move light the most.
            const auto stone = blocks->default_state(*blocks->find_block("minecraft:stone"));
            const auto torch = blocks->default_state(*blocks->find_block("minecraft:torch"));
            const auto air   = world::AirStates::from(*blocks).air;
            for (World* copy : {&before, &after}) {
                world::Chunk& centre = copy->at({0, 0});
                for (i32 y = 30; y >= -40; --y) {
                    centre.set_block(7, y, 7, air);
                }
                centre.set_block(7, -40, 7, torch);
                centre.set_block(7, 30, 7, stone);
                centre.set_block(0, 25, 7, stone);  // against the west border
            }
            relight_with_reference(before, *blocks);
            relight_after_edit(lookup_in(after), 0, 0, blocks);
            CHECK(differences(before, after) == 0);
        }
    }
}

TEST_CASE("an edit with nothing loaded around it relights nothing and does not crash",
          "[relight]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    const ChunkLookup nothing = [](i32, i32) -> world::Chunk* { return nullptr; };
    relight_after_edit(nothing, 5, -5, blocks);
    SUCCEED();
}

// Timing, not a test: run with `test_ov_server "[.relight-bench]"` in a
// release build to see what one edit's relight costs, old and new.
TEST_CASE("relight cost per edit, previous engine and rewrite", "[.relight-bench]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    constexpr int kRounds = 20;
    World         terrain = build(*blocks, 7, false);
    const auto    measure = [&](auto&& pass) {
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < kRounds; ++i) {
            pass();
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                         started)
                   .count() /
               kRounds;
    };
    const double old_us = measure([&] { relight_with_reference(terrain, *blocks); });
    const double new_us = measure([&] { relight_after_edit(lookup_in(terrain), 0, 0, blocks); });
    std::printf("relight per edit: previous %.0f us, rewrite %.0f us (x%.1f)\n", old_us, new_us,
                old_us / new_us);

    // The two halves apart: which one is left to win on.
    const ChunkLookup lookup  = lookup_in(terrain);
    const double      old_sky = measure([&] { reference::relight_neighbourhood(lookup, 0, 0, blocks); });
    const double      new_sky = measure([&] { relight_neighbourhood(lookup, 0, 0, blocks); });
    const double      old_blk = measure([&] {
        for (auto& [pos, chunk] : terrain) {
            reference::relight_blocks(chunk, *blocks);
        }
    });
    const double new_blk = measure([&] {
        for (auto& [pos, chunk] : terrain) {
            relight_blocks(chunk, *blocks);
        }
    });
    std::printf("  sky over the 3x3: previous %.0f us, rewrite %.0f us\n", old_sky, new_sky);
    std::printf("  block light, 9 chunks: previous %.0f us, rewrite %.0f us\n", old_blk, new_blk);
    SUCCEED();
}

// ── light ── The 3x3 recompute against the incremental engine, per edit, on
// real terrain. Timing, not a test: `test_ov_server "[.relight-bench-incremental]"`.
namespace {

struct RealChunks final : world::LightChunkSource {
    std::map<std::pair<i32, i32>, std::unique_ptr<world::Chunk>> chunks;

    world::Chunk* light_chunk(i32 x, i32 z) override {
        const auto found = chunks.find({x, z});
        return found == chunks.end() ? nullptr : found->second.get();
    }
};

std::unique_ptr<RealChunks> load_real(const registry::BlockRegistry& blocks,
                                      const std::filesystem::path& dir, i32 first, i32 side) {
    std::vector<std::string_view> biome_names(blocks.biome_count());
    for (u32 index = 0; index < blocks.biome_count(); ++index) {
        biome_names[index] = blocks.biome_name(index);
    }
    world::ChunkCodecContext context;
    context.blocks      = &blocks;
    context.biome_names = biome_names;
    context.air         = world::AirStates::from(blocks);
    auto out            = std::make_unique<RealChunks>();
    for (i32 cz = first; cz < first + side; ++cz) {
        for (i32 cx = first; cx < first + side; ++cx) {
            const ChunkPos pos{cx, cz};
            const auto     region = nbt::RegionFile::open(
                dir / "region" /
                ("r." + std::to_string(pos.region_x()) + "." + std::to_string(pos.region_z()) +
                 ".mca"));
            if (!region || !region->has_chunk(pos)) {
                continue;
            }
            const auto document = region->read_chunk(pos);
            if (!document) {
                continue;
            }
            if (auto chunk = world::from_nbt(*document, context)) {
                out->chunks.emplace(std::pair{cx, cz},
                                    std::make_unique<world::Chunk>(std::move(*chunk)));
            }
        }
    }
    return out->chunks.size() < 9 ? nullptr : std::move(out);
}

void print_percentiles(const char* what, std::vector<double>& micros) {
    std::ranges::sort(micros);
    const auto at = [&](double q) {
        return micros[std::min(micros.size() - 1, static_cast<usize>(q * micros.size()))];
    };
    std::printf("  %-26s p50 %9.1f us  p99 %9.1f us  max %9.1f us  (%zu edits)\n", what, at(0.5),
                at(0.99), micros.back(), micros.size());
}

}  // namespace

TEST_CASE("relight cost per edit: 3x3 recompute against incremental, on real worlds",
          "[.relight-bench-incremental]") {
    const registry::BlockRegistry* blocks = registry_or_null();
    if (blocks == nullptr) {
        SKIP("registry.ovpack is not built");
    }
    const std::filesystem::path run = std::filesystem::path{OV_SOURCE_DIR} / "run";
    const std::array<std::pair<const char*, std::filesystem::path>, 2> worlds{{
        {"ov_lab bench world", run / "lab"},
        {"real 1.20.1 world", run / "saves" / "New World"},
    }};
    const std::array<registry::BlockStateId, 5> palette{
        world::AirStates::from(*blocks).air, world::AirStates::from(*blocks).air,
        blocks->default_state(*blocks->find_block("minecraft:stone")),
        blocks->default_state(*blocks->find_block("minecraft:torch")),
        blocks->default_state(*blocks->find_block("minecraft:glowstone"))};
    constexpr i32   kFirst = -2;
    constexpr i32   kSide  = 5;
    constexpr usize kEdits = 300;

    for (const auto& [name, dir] : worlds) {
        auto before = load_real(*blocks, dir, kFirst, kSide);
        auto after  = load_real(*blocks, dir, kFirst, kSide);
        if (!before || !after) {
            std::printf("%s: not under run/\n", name);
            continue;
        }
        world::LightEngine    engine{*blocks, kOverworldLight};
        std::vector<ChunkPos> positions;
        for (const auto& [pos, chunk] : after->chunks) {
            positions.emplace_back(pos.first, pos.second);
        }
        engine.light_region(*after, positions);
        const ChunkLookup lookup = [&](i32 x, i32 z) { return before->light_chunk(x, z); };

        std::mt19937        random{23};
        std::vector<double> old_us;
        std::vector<double> new_us;
        for (usize edit = 0; edit < kEdits; ++edit) {
            // Inside the middle 3x3, so that the old pass has its whole
            // neighbourhood — the case it was built for.
            const i32 cx = kFirst + 1 + static_cast<i32>(random() % 3);
            const i32 cz = kFirst + 1 + static_cast<i32>(random() % 3);
            world::Chunk* old_chunk = before->light_chunk(cx, cz);
            world::Chunk* new_chunk = after->light_chunk(cx, cz);
            if (old_chunk == nullptr || new_chunk == nullptr) {
                continue;
            }
            const usize lx      = random() % 16;
            const usize lz      = random() % 16;
            const i32   surface = new_chunk->heightmap(world::HeightmapType::WorldSurface)
                                    .first_free(lx, lz);
            const i32   y       = std::clamp(surface - 3 + static_cast<i32>(random() % 4),
                                             new_chunk->shape().min_y, new_chunk->shape().max_y());
            const auto  state   = palette[random() % palette.size()];
            old_chunk->set_block(lx, y, lz, state);
            new_chunk->set_block(lx, y, lz, state);

            auto started = std::chrono::steady_clock::now();
            relight_after_edit(lookup, cx, cz, blocks);
            old_us.push_back(std::chrono::duration<double, std::micro>(
                                 std::chrono::steady_clock::now() - started)
                                 .count());

            started = std::chrono::steady_clock::now();
            engine.block_changed(BlockPos{cx * 16 + static_cast<i32>(lx), y,
                                          cz * 16 + static_cast<i32>(lz)});
            (void)engine.propagate(*after);
            new_us.push_back(std::chrono::duration<double, std::micro>(
                                 std::chrono::steady_clock::now() - started)
                                 .count());
        }
        std::printf("%s, %zu chunks:\n", name, after->chunks.size());
        print_percentiles("3x3 recompute (before)", old_us);
        print_percentiles("incremental (after)", new_us);
    }
    SUCCEED();
}
