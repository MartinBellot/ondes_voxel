// The Great Pyramid — an original Ondes VOXEL structure (great_pyramid.hpp).
//
// Everything here runs on a synthetic world: a sampler whose terrain and biome
// are formulas, and a level that is a dense block array. No generator, no jar,
// only the block registry — so the rules are checked alone: the gates, the
// labyrinth, the foundation, and that a chunk's part does not depend on the
// order chunks are placed in.
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/great_pyramid.hpp"
#include "ov/worldgen/structure_nbt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::worldgen;

namespace {

constexpr i64 kSeed = 20260911;

[[nodiscard]] std::filesystem::path registry_pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const std::optional<registry::BlockRegistry> loaded = [] {
        std::optional<registry::BlockRegistry> out;
        if (auto b = registry::BlockRegistry::load(registry_pack())) {
            out.emplace(std::move(*b));
        }
        return out;
    }();
    return loaded ? &*loaded : nullptr;
}

#define REQUIRE_BLOCKS()                                 \
    if (blocks() == nullptr) {                           \
        SKIP("registry.ovpack not built");               \
    }                                                    \
    const registry::BlockRegistry& reg = *blocks()

/// Terrain and biome as formulas.
class FakeSampler final : public StructureWorldSampler {
public:
    std::function<i32(i32, i32)>              ground = [](i32, i32) { return 70; };
    std::function<std::string_view(i32, i32)> biome  = [](i32, i32) {
        return std::string_view{"minecraft:desert"};
    };

    [[nodiscard]] std::string_view biome_at(i32 x, i32 /*y*/, i32 z) const override {
        return biome(x, z);
    }
    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override { return ground(x, z); }
    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override { return ground(x, z); }
    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return y < ground(x, z);
    }
};

/// The candidate chunk of grid cell (0, 0) for the test seed.
[[nodiscard]] ChunkPos candidate() { return great_pyramid_placement().candidate(kSeed, 0, 0); }

/// A dense block array around one pyramid, writable only inside `clip` —
/// which is how a chunk's structure step sees the world.
class MapLevel final : public StructureLevel {
public:
    MapLevel(const registry::BlockRegistry& b, i32 cx, i32 cz, i32 half, i32 y_lo, i32 y_hi)
        : blocks_(&b), x0_(cx - half), z0_(cz - half), side_(2 * half + 1), y_lo_(y_lo),
          y_hi_(y_hi) {
        cells_.assign(static_cast<usize>(side_) * static_cast<usize>(side_) *
                          static_cast<usize>(y_hi_ - y_lo_ + 1),
                      registry::kAirState);
    }

    void terrain(const std::function<registry::BlockStateId(i32, i32, i32)>& at) {
        for (i32 z = z0_; z < z0_ + side_; ++z) {
            for (i32 x = x0_; x < x0_ + side_; ++x) {
                for (i32 y = y_lo_; y <= y_hi_; ++y) {
                    cells_[index(x, y, z)] = at(x, y, z);
                }
            }
        }
    }

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        return inside(x, y, z) ? cells_[index(x, y, z)] : registry::kAirState;
    }
    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        if (!inside(x, y, z) || !clip.contains(x, y, z)) {
            ++refused;
            return false;
        }
        cells_[index(x, y, z)] = state;
        return true;
    }
    [[nodiscard]] i32 height(world::HeightmapType, i32 x, i32 z) const override {
        for (i32 y = y_hi_; y >= y_lo_; --y) {
            if (block_at(x, y, z) != registry::kAirState) {
                return y + 1;
            }
        }
        return y_lo_;
    }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:desert";
    }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }
    void set_block_entity(i32 x, i32 y, i32 z, nbt::Tag data) override {
        if (clip.contains(x, y, z)) {
            entities[{x, y, z}] = std::move(data);
        }
    }
    [[nodiscard]] const nbt::Tag* block_entity(i32 x, i32 y, i32 z) const override {
        const auto it = entities.find({x, y, z});
        return it == entities.end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool same_as(const MapLevel& other) const {
        return cells_ == other.cells_ && entities == other.entities;
    }
    [[nodiscard]] std::string_view name_at(i32 x, i32 y, i32 z) const {
        return blocks_->block_name(blocks_->block_of(block_at(x, y, z)));
    }
    [[nodiscard]] std::string_view value_at(i32 x, i32 y, i32 z, std::string_view property) const {
        const registry::BlockStateId state = block_at(x, y, z);
        const auto prop = blocks_->find_property(blocks_->block_of(state), property);
        return prop ? blocks_->property_value(state, *prop) : std::string_view{};
    }

    BoundingBox                                  clip;
    u64                                          refused{0};
    std::map<std::array<i32, 3>, nbt::Tag>       entities;

private:
    [[nodiscard]] bool inside(i32 x, i32 y, i32 z) const noexcept {
        return x >= x0_ && x < x0_ + side_ && z >= z0_ && z < z0_ + side_ && y >= y_lo_ &&
               y <= y_hi_;
    }
    [[nodiscard]] usize index(i32 x, i32 y, i32 z) const noexcept {
        return (static_cast<usize>(y - y_lo_) * static_cast<usize>(side_) +
                static_cast<usize>(z - z0_)) *
                   static_cast<usize>(side_) +
               static_cast<usize>(x - x0_);
    }

    const registry::BlockRegistry*      blocks_;
    i32                                 x0_, z0_, side_, y_lo_, y_hi_;
    std::vector<registry::BlockStateId> cells_;
};

[[nodiscard]] registry::BlockStateId state_of(const registry::BlockRegistry& b,
                                              std::string_view name) {
    return b.default_state(*b.find_block(name));
}

/// Bumpy desert: sand over stone at 70 +- 3, a dug-out pit and a cave under
/// the plinth, and water in the pit — the three things the fill must handle.
struct Site {
    FakeSampler        sampler;
    GreatPyramidLayout layout;
    PyramidDecision    decision{PyramidDecision::NotCandidate};
};

[[nodiscard]] i32 bumpy(i32 x, i32 z) { return 70 + ((x * 7 + z * 13) >> 3) % 4 - ((x + z) & 1); }

void make_site(const GreatPyramid& pyramid, Site& site) {
    const auto c  = candidate();
    const i32  cx = c.x * 16 + 8;
    const i32  cz = c.z * 16 + 8;
    site.sampler.ground = [cx, cz](i32 x, i32 z) {
        const i32 dx = x - cx;
        const i32 dz = z - cz;
        if (dx >= 30 && dx <= 40 && dz >= -40 && dz <= -30) {
            return 58;  // a pit under the plinth, away from every sample point
        }
        return bumpy(x, z);
    };
    site.decision = pyramid.decide(kSeed, c.x, c.z, &site.sampler, nullptr, &site.layout);
}

void paint_terrain(MapLevel& level, const Site& site, const registry::BlockRegistry& b) {
    const auto stone = state_of(b, "minecraft:stone");
    const auto sand  = state_of(b, "minecraft:sand");
    const auto water = state_of(b, "minecraft:water");
    const auto bush  = state_of(b, "minecraft:dead_bush");
    const i32  cx    = site.layout.centre_x;
    const i32  cz    = site.layout.centre_z;
    level.terrain([&](i32 x, i32 y, i32 z) {
        const i32 g = site.sampler.ground(x, z);
        // A cave under the plinth's south-west quarter.
        if (x - cx <= -20 && x - cx >= -30 && z - cz >= 10 && z - cz <= 20 && y >= g - 9 &&
            y <= g - 4) {
            return registry::kAirState;
        }
        if (y < g - 3) {
            return stone;
        }
        if (y < g) {
            return sand;
        }
        if (g == 58 && y < 61) {
            return water;
        }
        if (y == g && ((x ^ z) & 15) == 0) {
            return bush;
        }
        return registry::kAirState;
    });
}

/// Place every chunk the pyramid crosses, in `order`.
void place_all(const GreatPyramid& pyramid, const GreatPyramidLayout& layout, MapLevel& level,
               std::vector<std::array<i32, 2>> order) {
    for (const auto& [chunk_x, chunk_z] : order) {
        level.clip = BoundingBox::chunk_column(chunk_x, chunk_z, -2048, 2047);
        (void)pyramid.place(level, layout, level.clip);
    }
}

[[nodiscard]] std::vector<std::array<i32, 2>> chunks_of(const GreatPyramidLayout& layout) {
    std::vector<std::array<i32, 2>> out;
    for (i32 z = layout.box.min_z >> 4; z <= layout.box.max_z >> 4; ++z) {
        for (i32 x = layout.box.min_x >> 4; x <= layout.box.max_x >> 4; ++x) {
            out.push_back({x, z});
        }
    }
    return out;
}

}  // namespace

TEST_CASE("the set is its own: grid, salt and the JSON agree", "[great_pyramid]") {
    const RandomSpreadPlacement p = great_pyramid_placement();
    REQUIRE(p.spacing == 24);
    REQUIRE(p.separation == 12);
    REQUIRE(p.salt == 20260911);
    std::ifstream in{std::filesystem::path{OV_SOURCE_DIR} / "data" / "ondes_voxel" /
                     "ov_structures" / "great_pyramid.json"};
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    const std::string json = text.str();
    REQUIRE(json.find("\"spacing\": 24") != std::string::npos);
    REQUIRE(json.find("\"separation\": 12") != std::string::npos);
    REQUIRE(json.find("\"salt\": 20260911") != std::string::npos);
    REQUIRE(json.find("\"ondes_voxel:great_pyramid\"") != std::string::npos);
}

TEST_CASE("the footprint gates refuse by name", "[great_pyramid]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    const auto c = candidate();

    GreatPyramidLayout layout;
    FakeSampler        flat;
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &flat, nullptr, &layout) == PyramidDecision::Placed);
    REQUIRE(layout.base_y == 70);
    REQUIRE(pyramid->decide(kSeed, c.x + 1, c.z, &flat, nullptr, nullptr) ==
            PyramidDecision::NotCandidate);
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, nullptr, nullptr, nullptr) ==
            PyramidDecision::NoSampler);

    FakeSampler badlands;
    badlands.biome = [](i32, i32) { return std::string_view{"minecraft:badlands"}; };
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &badlands, nullptr, nullptr) ==
            PyramidDecision::NotDesert);

    // Desert in the middle, but a third of the footprint is savanna.
    FakeSampler edge;
    const i32   x0 = c.x * 16 + 8;
    edge.biome     = [x0](i32 x, i32) {
        return x - x0 > 20 ? std::string_view{"minecraft:savanna"} : std::string_view{"minecraft:desert"};
    };
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &edge, nullptr, nullptr) == PyramidDecision::NotDesert);

    FakeSampler slope;
    slope.ground = [x0](i32 x, i32) { return 80 + (x - x0) / 4; };  // 101 blocks: 24 up
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &slope, nullptr, nullptr) == PyramidDecision::TooSteep);

    FakeSampler gentle;
    gentle.ground = [x0](i32 x, i32) { return 80 + (x - x0) / 5; };  // 20 up: allowed
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &gentle, nullptr, &layout) == PyramidDecision::Placed);
    REQUIRE(layout.base_y == 80);  // the median of a symmetric slope is its middle

    FakeSampler shore;
    shore.ground = [](i32, i32) { return 64; };
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &shore, nullptr, nullptr) == PyramidDecision::TooLow);
}

TEST_CASE("the labyrinth is a perfect maze over the ring", "[great_pyramid]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    FakeSampler flat;
    // Several starts: different seeds, different mazes, same properties.
    for (i64 seed : {kSeed, i64{1}, i64{-7}, i64{1234567890}}) {
        const auto         c = great_pyramid_placement().candidate(seed, 3, -2);
        GreatPyramidLayout layout;
        REQUIRE(pyramid->decide(seed, c.x, c.z, &flat, nullptr, &layout) == PyramidDecision::Placed);

        constexpr i32 kN    = GreatPyramidLayout::kMazeCells;
        i32           cells = 0;
        i32           edges = 0;
        for (i32 j = 0; j < kN; ++j) {
            for (i32 i = 0; i < kN; ++i) {
                if (!layout.in_maze(i, j)) {
                    // No passage leads out of the ring into the core.
                    for (u8 dir = 0; dir < 4; ++dir) {
                        REQUIRE_FALSE(layout.open(i, j, dir));
                    }
                    continue;
                }
                ++cells;
                edges += layout.open(i, j, 1) ? 1 : 0;
                edges += layout.open(i, j, 2) ? 1 : 0;
            }
        }
        REQUIRE(cells == 900 - 18 * 18);  // the ring: 576 cells
        // A spanning tree: every cell reachable, and exactly cells - 1 edges.
        REQUIRE(edges == cells - 1);
        std::vector<bool>                     seen(900, false);
        std::queue<std::array<i32, 2>>        todo;
        constexpr std::array<std::array<i32, 2>, 4> kStep{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
        todo.push(kMazeEntry);
        seen[GreatPyramidLayout::cell(kMazeEntry[0], kMazeEntry[1])] = true;
        i32 reached = 0;
        while (!todo.empty()) {
            const auto [i, j] = todo.front();
            todo.pop();
            ++reached;
            for (u8 dir = 0; dir < 4; ++dir) {
                if (!layout.open(i, j, dir)) {
                    continue;
                }
                const i32 ni = i + kStep[dir][0];
                const i32 nj = j + kStep[dir][1];
                if (!seen[GreatPyramidLayout::cell(ni, nj)]) {
                    seen[GreatPyramidLayout::cell(ni, nj)] = true;
                    todo.push({ni, nj});
                }
            }
        }
        REQUIRE(reached == cells);
        // Dead ends are furnished, some with traps and some with chests.
        const auto count = [&](DeadEnd kind) {
            return std::count(layout.dead_end_kind.begin(), layout.dead_end_kind.end(), kind);
        };
        REQUIRE(count(DeadEnd::TntTrap) + count(DeadEnd::ArrowTrap) > 0);
        REQUIRE(count(DeadEnd::Chest) > 0);
    }
}

TEST_CASE("the same start gives the same pyramid, and another seed another one",
          "[great_pyramid]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    FakeSampler        flat;
    const auto         c = candidate();
    GreatPyramidLayout a;
    GreatPyramidLayout b;
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &flat, nullptr, &a) == PyramidDecision::Placed);
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &flat, nullptr, &b) == PyramidDecision::Placed);
    REQUIRE(a.maze == b.maze);
    REQUIRE(a.treasure_seeds == b.treasure_seeds);
    REQUIRE(a.facing == b.facing);
    const auto         d = great_pyramid_placement().candidate(kSeed + 1, 0, 0);
    GreatPyramidLayout other;
    REQUIRE(pyramid->decide(kSeed + 1, d.x, d.z, &flat, nullptr, &other) == PyramidDecision::Placed);
    REQUIRE(other.maze != a.maze);
}

TEST_CASE("a chunk's part does not depend on the order chunks are placed in",
          "[great_pyramid][determinism]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    Site site;
    make_site(*pyramid, site);
    REQUIRE(site.decision == PyramidDecision::Placed);
    const GreatPyramidLayout& layout = site.layout;

    const auto make = [&] {
        auto level = std::make_unique<MapLevel>(reg, layout.centre_x, layout.centre_z, 88,
                                                layout.base_y - 64, layout.base_y + 56);
        paint_terrain(*level, site, reg);
        return level;
    };
    auto forward  = make();
    auto backward = make();
    auto shuffled = make();

    auto order = chunks_of(layout);
    REQUIRE(order.size() >= 64);
    place_all(*pyramid, layout, *forward, order);
    std::reverse(order.begin(), order.end());
    place_all(*pyramid, layout, *backward, order);
    std::mt19937 twister{12345};  // test-only shuffle; the generator never sees it
    std::shuffle(order.begin(), order.end(), twister);
    place_all(*pyramid, layout, *shuffled, order);

    REQUIRE(forward->same_as(*backward));
    REQUIRE(forward->same_as(*shuffled));
    // Nothing was ever written outside the chunk being placed.
    REQUIRE(forward->refused == 0);
}

TEST_CASE("nothing floats: every column under the plinth is filled to the ground",
          "[great_pyramid]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    Site site;
    make_site(*pyramid, site);
    REQUIRE(site.decision == PyramidDecision::Placed);
    const GreatPyramidLayout& layout = site.layout;
    MapLevel level{reg, layout.centre_x, layout.centre_z, 88, layout.base_y - 64,
                   layout.base_y + 56};
    paint_terrain(level, site, reg);
    place_all(*pyramid, layout, level, chunks_of(layout));

    // What may lie between the plinth's top and the natural ground: our fill
    // and our floors. The ground itself is sand or stone; a cave *under* it is
    // the world's, and not a gap under the plinth.
    const std::array<std::string_view, 5> fill_blocks{
        "minecraft:sandstone", "minecraft:cut_sandstone", "minecraft:smooth_sandstone",
        "minecraft:orange_terracotta", "minecraft:blue_terracotta"};
    i64 columns = 0;
    for (i32 v = -52; v <= 52; ++v) {
        for (i32 u = -52; u <= 52; ++u) {
            const BlockPos top = layout.world(u, -1, v);
            // Under the crypt and its stair the column is the crypt's; the
            // rule is about the plinth's own columns.
            if (std::abs(u) <= 15 && std::abs(v) <= 12) {
                continue;
            }
            if (std::abs(u) <= 1 && v >= -27 && v <= -25) {
                continue;  // the TNT under the hall's pit
            }
            ++columns;
            // From the plinth down to the first stone, no gap of any kind.
            for (i32 y = top.y; y >= layout.base_y - 60; --y) {
                const auto name = level.name_at(top.x, y, top.z);
                if (name == "minecraft:stone" || name == "minecraft:sand") {
                    break;
                }
                INFO("column (" << u << ", " << v << ") y " << y << " is " << name);
                REQUIRE(std::find(fill_blocks.begin(), fill_blocks.end(), name) !=
                        fill_blocks.end());
            }
        }
    }
    REQUIRE(columns > 10000);
    // The pit was deep and flooded: the fill reached its sand floor at 57.
    const BlockPos pit{layout.centre_x + 35, 0, layout.centre_z - 35};
    REQUIRE(level.name_at(pit.x, 60, pit.z) == "minecraft:sandstone");
    REQUIRE(level.name_at(pit.x, 58, pit.z) == "minecraft:sandstone");
    REQUIRE(level.name_at(pit.x, 57, pit.z) == "minecraft:sand");
}

TEST_CASE("the rooms are where the plan puts them", "[great_pyramid]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    Site site;
    make_site(*pyramid, site);
    REQUIRE(site.decision == PyramidDecision::Placed);
    const GreatPyramidLayout& L = site.layout;
    MapLevel level{reg, L.centre_x, L.centre_z, 88, L.base_y - 64, L.base_y + 56};
    paint_terrain(level, site, reg);
    place_all(*pyramid, L, level, chunks_of(L));

    const auto name = [&](i32 u, i32 y, i32 v) {
        const BlockPos p = L.world(u, y, v);
        return level.name_at(p.x, p.y, p.z);
    };
    const auto entity = [&](i32 u, i32 y, i32 v) {
        const BlockPos p = L.world(u, y, v);
        return level.block_entity(p.x, p.y, p.z);
    };
    // Shell, corners, bands, capstone.
    REQUIRE(name(0, 50, 0) == "minecraft:gold_block");
    REQUIRE(name(50, 0, 50) == "minecraft:chiseled_sandstone");
    REQUIRE(name(0, 16, 34) == "minecraft:cut_sandstone");
    REQUIRE(name(0, 28, 22) == "minecraft:blue_terracotta");
    REQUIRE(name(0, 5, 20) == "minecraft:sandstone");  // the solid mass behind the hall
    // The grand entrance and its pillars.
    REQUIRE(name(0, 4, -50) == "minecraft:air");
    REQUIRE(name(0, 4, -40) == "minecraft:air");
    REQUIRE(name(-5, 5, -51) == "minecraft:chiseled_sandstone");
    REQUIRE(name(5, 11, -51) == "minecraft:lantern");
    // The hall: air, pillars, lanterns.
    REQUIRE(name(-9, 5, -15) == "minecraft:air");
    REQUIRE(name(6, 5, -21) == "minecraft:cut_sandstone");
    REQUIRE(name(9, 11, -24) == "minecraft:lantern");
    // The gallery climbs to the door.
    REQUIRE(name(0, 0, -22) == "minecraft:sandstone_stairs");
    REQUIRE(name(0, 24, 2) == "minecraft:sandstone_stairs");
    REQUIRE(name(0, 23, -3) == "minecraft:air");  // the corbelled vault
    REQUIRE(name(-2, 23, -3) == "minecraft:smooth_sandstone");  // ... and its corbel
    // The Pharaoh's chamber: four chests with our loot table.
    for (const auto& [u, v] : std::array<std::array<i32, 2>, 4>{{{-6, 6}, {-6, 10}, {6, 6}, {6, 10}}}) {
        REQUIRE(name(u, 26, v) == "minecraft:chest");
        const nbt::Tag* chest = entity(u, 26, v);
        REQUIRE(chest != nullptr);
        REQUIRE(chest->find("LootTable")->as_string() == "ondes_voxel:chests/great_pyramid/treasure");
    }
    REQUIRE(name(0, 26, 8) == "minecraft:polished_blackstone_bricks");
    REQUIRE(name(0, 30, 3) == "minecraft:blue_terracotta");
    REQUIRE(name(12, 34, 8) == "minecraft:air");  // an air shaft
    // The Queen's chamber, the crypt and its spawner.
    REQUIRE(name(16, 13, -12) == "minecraft:chest");
    REQUIRE(name(0, -12, 0) == "minecraft:spawner");
    REQUIRE(entity(0, -12, 0)->find("SpawnData")->find("entity")->find("id")->as_string() ==
            "minecraft:husk");
    i32 suspicious = 0;
    for (i32 v = 6; v <= 9; ++v) {
        for (i32 u = -3; u <= 3; ++u) {
            if (name(u, -13, v) == "minecraft:suspicious_sand") {
                ++suspicious;
                REQUIRE(entity(u, -13, v)->find("LootTable")->as_string() ==
                        "ondes_voxel:archaeology/great_pyramid");
            }
        }
    }
    REQUIRE(suspicious == 6);
    REQUIRE(name(14, -1, -3) == "minecraft:birch_trapdoor");
    REQUIRE(name(14, -3, -3) == "minecraft:air");
    // The labyrinth: the stair arrives in its entry cell.
    REQUIRE(name(-17, 13, -15) == "minecraft:sandstone_stairs");
    REQUIRE(name(-18, 15, -13) == "minecraft:air");
    REQUIRE(name(-19, 15, -13) == "minecraft:air");
}

TEST_CASE("every trap is wired in a consistent, unfired state", "[great_pyramid][redstone]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    Site site;
    make_site(*pyramid, site);
    REQUIRE(site.decision == PyramidDecision::Placed);
    const GreatPyramidLayout& L = site.layout;
    MapLevel level{reg, L.centre_x, L.centre_z, 88, L.base_y - 64, L.base_y + 56};
    paint_terrain(level, site, reg);
    place_all(*pyramid, L, level, chunks_of(L));
    const auto name = [&](i32 u, i32 y, i32 v) {
        const BlockPos p = L.world(u, y, v);
        return level.name_at(p.x, p.y, p.z);
    };
    const auto value = [&](i32 u, i32 y, i32 v, std::string_view property) {
        const BlockPos p = L.world(u, y, v);
        return level.value_at(p.x, p.y, p.z, property);
    };

    // The hall's pit: a plate up, nine TNT under the floor.
    REQUIRE(name(0, 0, -26) == "minecraft:stone_pressure_plate");
    REQUIRE(value(0, 0, -26, "powered") == "false");
    for (i32 v = -27; v <= -25; ++v) {
        for (i32 u = -1; u <= 1; ++u) {
            REQUIRE(name(u, -2, v) == "minecraft:tnt");
        }
    }
    // The Queen's passage: two hooks, attached and unpowered; the string;
    // two untriggered dispensers holding arrows.
    for (const i32 v : {-11, -9}) {
        REQUIRE(name(5, 13, v) == "minecraft:tripwire_hook");
        REQUIRE(value(5, 13, v, "attached") == "true");
        REQUIRE(value(5, 13, v, "powered") == "false");
    }
    REQUIRE(name(5, 13, -10) == "minecraft:tripwire");
    REQUIRE(value(5, 13, -10, "attached") == "true");
    REQUIRE(value(5, 13, -10, "powered") == "false");
    for (const i32 v : {-12, -8}) {
        REQUIRE(name(5, 14, v) == "minecraft:dispenser");
        REQUIRE(value(5, 14, v, "triggered") == "false");
        const BlockPos p = L.world(5, 14, v);
        REQUIRE(level.block_entity(p.x, p.y, p.z)->find("Items")->list()->size() == 1);
    }
    // The secret door: both pistons out, both heads sticky, the door shut, the
    // lever on.
    for (const i32 y : {26, 27}) {
        REQUIRE(name(-2, y, 3) == "minecraft:sticky_piston");
        REQUIRE(value(-2, y, 3, "extended") == "true");
        REQUIRE(name(-1, y, 3) == "minecraft:piston_head");
        REQUIRE(value(-1, y, 3, "type") == "sticky");
        REQUIRE(name(0, y, 3) == "minecraft:cut_sandstone");
    }
    REQUIRE(name(-2, 27, 1) == "minecraft:lever");
    REQUIRE(value(-2, 27, 1, "powered") == "true");
    // The labyrinth's dead ends: every plate up, every TNT trap on TNT.
    constexpr i32 kN = GreatPyramidLayout::kMazeCells;
    i32           traps = 0;
    for (i32 j = 0; j < kN; ++j) {
        for (i32 i = 0; i < kN; ++i) {
            const DeadEnd kind = L.dead_end_kind[GreatPyramidLayout::cell(i, j)];
            const i32     u    = -29 + 2 * i;
            const i32     v    = -29 + 2 * j;
            if (kind == DeadEnd::TntTrap || kind == DeadEnd::ArrowTrap) {
                ++traps;
                REQUIRE(name(u, 14, v) == "minecraft:stone_pressure_plate");
                REQUIRE(value(u, 14, v, "powered") == "false");
            }
            if (kind == DeadEnd::TntTrap) {
                REQUIRE(name(u, 12, v) == "minecraft:tnt");
            }
            if (kind == DeadEnd::Chest) {
                REQUIRE(name(u, 14, v) == "minecraft:chest");
            }
        }
    }
    REQUIRE(traps > 0);
}

TEST_CASE("the start is stored in the chunk format, under our namespace", "[great_pyramid]") {
    REQUIRE_BLOCKS();
    const auto pyramid = GreatPyramid::create(reg);
    REQUIRE(pyramid);
    FakeSampler        flat;
    const auto         c = candidate();
    GreatPyramidLayout layout;
    REQUIRE(pyramid->decide(kSeed, c.x, c.z, &flat, nullptr, &layout) == PyramidDecision::Placed);
    const nbt::Tag start = GreatPyramid::start_to_nbt(layout);
    REQUIRE(start.find("id")->as_string() == "ondes_voxel:great_pyramid");
    REQUIRE(start.find("ChunkX")->as_i64() == c.x);
    REQUIRE(start.find("ChunkZ")->as_i64() == c.z);
    REQUIRE(start.find("Children")->list()->size() == 7);
    for (const nbt::Tag& child : *start.find("Children")->list()) {
        REQUIRE(child.find("id")->as_string().starts_with("ondes_voxel:great_pyramid/"));
        REQUIRE(child.find("BB")->get_if<nbt::Tag::IntArray>()->size() == 6);
    }

    // The stage writes the start into its chunk and a reference into every
    // chunk its box crosses.
    GreatPyramidStage stage{reg, nullptr, &flat, kSeed};
    REQUIRE(stage.ready());
    nbt::Tag here = chunk_structures_to_nbt({}, {});
    REQUIRE(stage.record(c.x, c.z, here));
    REQUIRE(here.find("starts")->find("ondes_voxel:great_pyramid") != nullptr);
    nbt::Tag edge = chunk_structures_to_nbt({}, {});
    REQUIRE(stage.record(c.x + 3, c.z - 3, edge));
    REQUIRE(edge.find("starts")->find("ondes_voxel:great_pyramid") == nullptr);
    const auto* refs =
        edge.find("References")->find("ondes_voxel:great_pyramid")->get_if<nbt::Tag::LongArray>();
    REQUIRE(refs->size() == 1);
    REQUIRE(refs->front() == packed_chunk_pos(c.x, c.z));
    nbt::Tag far = chunk_structures_to_nbt({}, {});
    REQUIRE_FALSE(stage.record(c.x + 9, c.z, far));
}
