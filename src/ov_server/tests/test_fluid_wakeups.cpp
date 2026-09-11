// ── worldgen-3 ── A waterfall the generator made flows.
//
// The aquifer marks the fluids it wants woken — the game's `PostProcessing`
// marks (docs/provenance/aquiferes.md § 10.4) — and the chunk carries them
// from the pipeline to the tick thread, which wakes each one as a neighbour's
// change would. Here the path is taken end to end on generated terrain: a
// marked fluid with open air beside or under it is found, left alone for sixty
// ticks (it does not move — the control), then woken (it flows).

#include "../src/generated_world.hpp"
#include "../src/world_ticks.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] std::filesystem::path data_dir() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data";
}
[[nodiscard]] std::filesystem::path pack() {
    return data_dir() / "vanilla" / "1.20.1" / "registry.ovpack";
}

/// One generated chunk as a level: everything outside it is unloaded.
struct OneChunk {
    ChunkPos     pos;
    world::Chunk chunk;
    u64          flowed_into_air{0};

    [[nodiscard]] bool inside(BlockPos p) const {
        return (p.x >> 4) == pos.x && (p.z >> 4) == pos.z && p.y >= chunk.shape().min_y &&
               p.y <= chunk.shape().max_y();
    }
    [[nodiscard]] registry::BlockStateId at(BlockPos p) const {
        if (!inside(p)) {
            return registry::kAirState;
        }
        return chunk.get_block(static_cast<usize>(p.x & 15), p.y, static_cast<usize>(p.z & 15));
    }
};

[[nodiscard]] LevelHooks hooks_for(OneChunk& one, const registry::BlockRegistry& blocks) {
    LevelHooks hooks;
    hooks.block_at  = [&one](BlockPos p) { return one.at(p); };
    hooks.is_loaded = [&one](BlockPos p) { return one.inside(p); };
    hooks.set_block = [&one, &blocks](BlockPos p, registry::BlockStateId s) {
        if (!one.inside(p)) {
            return;
        }
        const auto was = one.at(p);
        if ((was == registry::kAirState || blocks.is_air(blocks.block_of(was))) &&
            blocks.holds_fluid(s)) {
            ++one.flowed_into_air;
        }
        one.chunk.set_block(static_cast<usize>(p.x & 15), p.y, static_cast<usize>(p.z & 15), s);
    };
    hooks.container_signal = [](BlockPos) { return -1; };
    return hooks;
}

}  // namespace

TEST_CASE("a generated waterfall flows once its chunk is woken", "[worldgen3][fluid][server]") {
    if (!std::filesystem::is_regular_file(pack()) ||
        !std::filesystem::is_directory(data_dir() / "vanilla" / "1.20.1" / "generated")) {
        SKIP("no registry pack or generated data");
    }
    auto blocks     = registry::BlockRegistry::load(pack());
    auto registries = registry::Registries::load(pack());
    REQUIRE(blocks);
    REQUIRE(registries);
    std::vector<std::string_view> names;
    for (u32 index = 0; index < blocks->biome_count(); ++index) {
        names.push_back(blocks->biome_name(index));
    }
    auto world = GeneratedWorld::load(data_dir(), *blocks, *registries, names, 1234567890, 1);
    REQUIRE(world);

    // A marked fluid with open air under it or beside it, inside the chunk.
    const auto open_beside = [&](const OneChunk& one, BlockPos p) -> std::optional<BlockPos> {
        constexpr std::array<std::array<i32, 3>, 5> kSides{
            {{0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
        for (const auto& side : kSides) {
            const BlockPos q{p.x + side[0], p.y + side[1], p.z + side[2]};
            const auto     s = one.at(q);
            if (one.inside(q) && (s == registry::kAirState || blocks->is_air(blocks->block_of(s)))) {
                return q;
            }
        }
        return std::nullopt;
    };

    bool found = false;
    for (i32 cx = 0; cx < 6 && !found; ++cx) {
        std::vector<std::pair<ChunkPos, world::Chunk>> out;
        std::vector<BlockPos>                          wakeups;
        world->generate_square(0, cx, 0, 1, out, &wakeups);
        REQUIRE(out.size() == 1);
        OneChunk one{out.front().first, std::move(out.front().second)};
        for (const BlockPos p : wakeups) {
            CHECK(one.inside(p));
        }

        std::optional<BlockPos> fall;
        std::optional<BlockPos> into;
        for (const BlockPos p : wakeups) {
            if (!blocks->holds_fluid(one.at(p))) {
                continue;  // a later stage replaced it; the wake is then a no-op
            }
            if (auto open = open_beside(one, p)) {
                fall = p;
                into = open;
                break;
            }
        }
        if (!fall) {
            continue;
        }
        found = true;

        ServerLevel level{*blocks, hooks_for(one, *blocks)};
        WorldTicks  ticks{*blocks, *registries};

        // The control: nothing wakes it, and nothing moves.
        for (i64 now = 1; now <= 60; ++now) {
            level.set_game_time(now);
            (void)ticks.run(level, now);
        }
        CHECK(one.flowed_into_air == 0);
        const bool still_open = one.at(*into) == registry::kAirState ||
                                blocks->is_air(blocks->block_of(one.at(*into)));
        CHECK(still_open);

        // Woken, as the server wakes a freshly published chunk.
        for (const BlockPos p : wakeups) {
            ticks.fluid().on_neighbour_changed(level, p);
        }
        for (i64 now = 61; now <= 120; ++now) {
            level.set_game_time(now);
            (void)ticks.run(level, now);
        }
        INFO("chunk " << cx << ",0: " << wakeups.size() << " marks, fall at " << fall->x << ' '
                      << fall->y << ' ' << fall->z);
        CHECK(one.flowed_into_air > 0);
        CHECK(blocks->holds_fluid(one.at(*into)));
    }
    CHECK(found);
}
