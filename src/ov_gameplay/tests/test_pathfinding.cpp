// The A*, and the maze a real 1.20.1 zombie already walked.
//
// The parity test at the bottom is the one that matters. Everything above it
// checks a piece; that one checks the whole thing against the game, on the same
// geometry, and it is the only test here that could fail because the *rule* is
// wrong rather than because the code is.
//
// The oracle is scripts/measure_mobs.py, campaign `maze`. Its trace and the
// maze it was walked in are in data/vanilla/1.20.1/normalized/mob_spawning.json;
// the maze is restated here as constants so the test does not need the file,
// and docs/provenance/mobs.md records both.
#include "ov/gameplay/pathfinding.hpp"

#include "ov/base/alloc_scope.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <unordered_map>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

/// A world made of whatever the test puts in it, over an infinite stone floor.
///
/// Deliberately not a Chunk: the rules under test take a `LevelView&` precisely
/// so that they can be exercised against something this small, and a test that
/// needed a chunk pipeline to path two blocks would not be run.
class TestLevel final : public world::LevelView {
public:
    explicit TestLevel(const registry::BlockRegistry& registry, i32 floor_y = -1)
        : registry_{&registry}, floor_y_{floor_y} {
        air_   = registry.default_state(registry.find_block("minecraft:air").value());
        stone_ = registry.default_state(registry.find_block("minecraft:stone").value());
    }

    void set(BlockPos pos, registry::BlockStateId state) { overrides_[key(pos)] = state; }

    void set_named(BlockPos pos, std::string_view name) {
        set(pos, registry_->default_state(registry_->find_block(name).value()));
    }

    void fill(BlockPos from, BlockPos to, std::string_view name) {
        const registry::BlockStateId state =
            registry_->default_state(registry_->find_block(name).value());
        for (i32 x = from.x; x <= to.x; ++x) {
            for (i32 y = from.y; y <= to.y; ++y) {
                for (i32 z = from.z; z <= to.z; ++z) {
                    set({x, y, z}, state);
                }
            }
        }
    }

    /// Limit the floor to a box. Outside it there is nothing to stand on, which
    /// is the only way to make "there is no way round" true in a test: with an
    /// infinite floor a mob simply walks past the end of any wall, and four of
    /// the tests below were measuring that rather than what they claimed to.
    void set_floor_bounds(i32 min_x, i32 max_x, i32 min_z, i32 max_z) {
        bounded_ = true;
        min_x_ = min_x;
        max_x_ = max_x;
        min_z_ = min_z;
        max_z_ = max_z;
    }

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = overrides_.find(key(pos));
        if (found != overrides_.end()) {
            return found->second;
        }
        if (bounded_ &&
            (pos.x < min_x_ || pos.x > max_x_ || pos.z < min_z_ || pos.z > max_z_)) {
            return air_;
        }
        return pos.y <= floor_y_ ? stone_ : air_;
    }

    [[nodiscard]] bool             is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    [[nodiscard]] static u64 key(BlockPos pos) noexcept {
        return (static_cast<u64>(static_cast<u32>(pos.x)) << 40) ^
               (static_cast<u64>(static_cast<u32>(pos.z)) << 12) ^
               static_cast<u64>(static_cast<u32>(pos.y) & 0xFFFU);
    }

    const registry::BlockRegistry*             registry_;
    i32                                        floor_y_;
    registry::BlockStateId                     air_{};
    registry::BlockStateId                     stone_{};
    bool                                       bounded_{false};
    i32                                        min_x_{0};
    i32                                        max_x_{0};
    i32                                        min_z_{0};
    i32                                        max_z_{0};
    std::unordered_map<u64, registry::BlockStateId> overrides_;
};

/// A zombie: one column wide, two tall, steps up one, falls three.
[[nodiscard]] MobSize zombie_size() noexcept { return MobSize::from_box(0.6F, 1.95F); }

[[nodiscard]] PathAbilities walker() noexcept {
    PathAbilities abilities;
    abilities.enters_water = true;
    return abilities;
}

// ── The oracle's maze ───────────────────────────────────────────────────────
//
// Three transverse walls with one gap each, alternating sides, inside a walled
// square. Identical to scripts/measure_mobs.py: MAZE_HALF, MAZE_WALL_Z and the
// gap rule are the same numbers, and changing one without the other makes the
// comparison meaningless.
constexpr i32 kMazeHalf     = 8;
constexpr i32 kMazeWallZ[3] = {-4, 0, 4};

/// The gap in wall `index`: the west side for even walls, the east for odd.
[[nodiscard]] constexpr i32 maze_gap(i32 index) noexcept {
    return index % 2 == 0 ? -kMazeHalf + 1 : kMazeHalf - 1;
}

void build_maze(TestLevel& level, i32 floor_y) {
    const i32 y = floor_y + 1;
    // The perimeter, standing exactly on the ends of the transverse walls. The
    // oracle's first arena was two blocks wider and the zombie solved the maze
    // by walking round the end of every wall.
    level.fill({-kMazeHalf, y, -kMazeHalf}, {-kMazeHalf, y + 3, kMazeHalf}, "minecraft:stone");
    level.fill({kMazeHalf, y, -kMazeHalf}, {kMazeHalf, y + 3, kMazeHalf}, "minecraft:stone");
    level.fill({-kMazeHalf, y, -kMazeHalf}, {kMazeHalf, y + 3, -kMazeHalf}, "minecraft:stone");
    level.fill({-kMazeHalf, y, kMazeHalf}, {kMazeHalf, y + 3, kMazeHalf}, "minecraft:stone");

    for (i32 index = 0; index < 3; ++index) {
        const i32 wall_z = kMazeWallZ[index];
        const i32 gap    = maze_gap(index);
        for (i32 x = -kMazeHalf; x <= kMazeHalf; ++x) {
            if (x == gap) {
                continue;
            }
            level.fill({x, y, wall_z}, {x, y + 2, wall_z}, "minecraft:stone");
        }
    }
}

/// Which wall the route crosses, and at which x — the same reduction the oracle
/// applies to the zombie's trace.
///
/// The comparison is made on this and not on the raw position sequence, because
/// the two are not comparable at that resolution: vanilla's mob wobbles within
/// its block and re-paths several times a second, while an A* emits block
/// centres. What does not wobble is which opening it went through, and a path
/// finder that picks a different gap has genuinely found a different way.
[[nodiscard]] std::vector<std::pair<i32, i32>> crossings(const Path& path) {
    std::vector<std::pair<i32, i32>> out;
    for (usize i = 1; i < path.steps.size(); ++i) {
        const i32 before = path.steps[i - 1].pos.z;
        const i32 after  = path.steps[i].pos.z;
        for (const i32 wall_z : kMazeWallZ) {
            if ((before < wall_z && after >= wall_z) || (before > wall_z && after <= wall_z)) {
                out.emplace_back(wall_z, path.steps[i].pos.x);
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("a walker crosses flat ground in a straight line", "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    TestLevel  level{*blocks()};
    PathFinder finder;
    Path       path;
    REQUIRE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {6, 0, 0}, zombie_size(), walker(),
                        64.0F, path));
    REQUIRE(path.steps.front().pos == BlockPos{0, 0, 0});
    REQUIRE(path.steps.back().pos == BlockPos{6, 0, 0});
    // Seven nodes, not eight: the start counts.
    CHECK(path.steps.size() == 7);
}

TEST_CASE("a wall is walked around, not through", "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    TestLevel level{*blocks()};
    level.set_floor_bounds(-1, 8, -4, 4);
    // A wall across the direct line, with one gap.
    level.fill({3, 0, -4}, {3, 2, 4}, "minecraft:stone");
    level.fill({3, 0, 2}, {3, 2, 2}, "minecraft:air");

    PathFinder finder;
    Path       path;
    REQUIRE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {6, 0, 0}, zombie_size(), walker(),
                        64.0F, path));
    bool through_gap = false;
    for (const PathStep& step : path.steps) {
        CHECK_FALSE((step.pos.x == 3 && step.pos.z != 2 && step.pos.z >= -4 && step.pos.z <= 4));
        if (step.pos.x == 3 && step.pos.z == 2) {
            through_gap = true;
        }
    }
    CHECK(through_gap);
}

TEST_CASE("a one-block step is climbed and a two-block wall is not", "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    SECTION("one block up") {
        TestLevel level{*blocks()};
        level.fill({2, 0, 0}, {8, 0, 0}, "minecraft:stone");
        PathFinder finder;
        Path       path;
        REQUIRE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {8, 1, 0}, zombie_size(),
                            walker(), 64.0F, path));
        CHECK(path.steps.back().pos == BlockPos{8, 1, 0});
    }
    SECTION("two blocks up, with no way round") {
        TestLevel level{*blocks()};
        level.set_floor_bounds(-1, 10, -3, 3);
        // A full-height barrier from wall to wall: nothing can get past it.
        level.fill({2, 0, -3}, {2, 3, 3}, "minecraft:stone");
        PathFinder finder;
        Path       path;
        CHECK_FALSE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {8, 2, 0}, zombie_size(),
                                walker(), 24.0F, path));
        // Still a route: the best it could do. A partial path is what makes a
        // mob walk *at* an unreachable target rather than stand still.
        CHECK_FALSE(path.steps.empty());
        CHECK(path.steps.front().pos == BlockPos{0, 0, 0});
    }
}

TEST_CASE("a mob two blocks wide does not fit through a one-block gap",
          "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    TestLevel level{*blocks()};
    level.set_floor_bounds(-1, 8, -6, 6);
    level.fill({3, 0, -6}, {3, 2, 6}, "minecraft:stone");
    level.fill({3, 0, 0}, {3, 2, 0}, "minecraft:air");

    PathFinder finder;
    Path       path;
    // A spider is 1.4 wide, which rounds up to two columns.
    const MobSize spider = MobSize::from_box(1.4F, 0.9F);
    CHECK(spider.width == 2);
    CHECK_FALSE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {6, 0, 0}, spider, walker(),
                            32.0F, path));
    // The same gap, widened by one, and it fits.
    level.fill({3, 0, 1}, {3, 2, 1}, "minecraft:air");
    CHECK(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {6, 0, 0}, spider, walker(), 32.0F,
                      path));
}

TEST_CASE("lava is never crossed and water is crossed reluctantly",
          "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    SECTION("lava") {
        TestLevel level{*blocks()};
        level.set_floor_bounds(-1, 8, -8, 8);
        level.fill({3, 0, -8}, {3, 0, 8}, "minecraft:lava");
        PathFinder finder;
        Path       path;
        CHECK_FALSE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {6, 0, 0}, zombie_size(),
                                walker(), 24.0F, path));
    }
    SECTION("water, with a dry way round that is short") {
        TestLevel level{*blocks()};
        level.set_floor_bounds(-2, 11, -4, 4);
        // Four blocks of water on the direct line; a dry detour two blocks
        // longer. The malus is eight a block, so the detour wins by a mile.
        level.fill({3, 0, 0}, {6, 0, 0}, "minecraft:water");
        PathFinder finder;
        Path       path;
        REQUIRE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {9, 0, 0}, zombie_size(),
                            walker(), 32.0F, path));
        for (const PathStep& step : path.steps) {
            CHECK(step.type != PathNodeType::Water);
        }
    }
    SECTION("water, walled in, so there is no way round") {
        TestLevel level{*blocks()};
        level.set_floor_bounds(-1, 10, -1, 1);
        level.fill({-1, 0, -1}, {10, 3, -1}, "minecraft:stone");
        level.fill({-1, 0, 1}, {10, 3, 1}, "minecraft:stone");
        level.fill({3, 0, 0}, {6, 0, 0}, "minecraft:water");
        PathFinder finder;
        Path       path;
        REQUIRE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {9, 0, 0}, zombie_size(),
                            walker(), 32.0F, path));
        bool wet = false;
        for (const PathStep& step : path.steps) {
            wet = wet || step.type == PathNodeType::Water;
        }
        CHECK(wet);
    }
}

TEST_CASE("a fence is not jumped", "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    TestLevel level{*blocks()};
    level.set_floor_bounds(-1, 8, -6, 6);
    // Recognised as a fence by its collision box leaving the cube, not by name.
    level.fill({3, 0, -6}, {3, 0, 6}, "minecraft:oak_fence");
    const WalkNodeEvaluator walk;
    CHECK(walk.type_at(level, {3, 0, 0}, zombie_size(), walker()) == PathNodeType::Fence);

    PathFinder finder;
    Path       path;
    CHECK_FALSE(finder.find(WalkNodeEvaluator{}, level, {0, 0, 0}, {6, 0, 0}, zombie_size(),
                            walker(), 20.0F, path));
}

TEST_CASE("a search allocates nothing after the first one", "[gameplay][path]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    TestLevel  level{*blocks()};
    build_maze(level, -1);
    PathFinder finder;
    Path       path;
    path.steps.reserve(512);
    // Warm every buffer: the node pool, the heap and the visited table are all
    // sized at construction, but `Path::steps` grows on the first search and
    // `PathFollower` copies into its own vector.
    (void)finder.find(WalkNodeEvaluator{}, level, {0, 0, -7}, {0, 0, 7}, zombie_size(), walker(),
                      64.0F, path);
    {
        const NoAllocScope guard{"path search"};
        (void)finder.find(WalkNodeEvaluator{}, level, {0, 0, -7}, {0, 0, 7}, zombie_size(),
                          walker(), 64.0F, path);
    }
    CHECK(path.visited > 0);
}

// ── The parity test ─────────────────────────────────────────────────────────

TEST_CASE("our route through the oracle's maze is the route the game took",
          "[gameplay][path][parity]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    TestLevel level{*blocks()};
    build_maze(level, -1);

    // The oracle's start and finish, in the same coordinates.
    const BlockPos start{0, 0, -(kMazeHalf - 1)};
    const BlockPos finish{0, 0, kMazeHalf - 1};

    PathFinder finder;
    Path       path;
    REQUIRE(finder.find(WalkNodeEvaluator{}, level, start, finish, zombie_size(), walker(), 128.0F,
                        path));
    REQUIRE(path.reached);

    // Measured on a real 1.20.1 server: the zombie crossed wall z = -4 at
    // x = -6.42, wall z = 0 at x = +7.42 and wall z = +4 at x = -6.42, in that
    // order and once each, and reached the villager. The gaps are at x = -7,
    // +7 and -7; a mob standing in the gap column reports a position half a
    // block off centre, which is where the .42 comes from.
    const std::vector<std::pair<i32, i32>> ours = crossings(path);
    REQUIRE(ours.size() == 3);
    CHECK(ours[0] == std::pair<i32, i32>{-4, maze_gap(0)});
    CHECK(ours[1] == std::pair<i32, i32>{0, maze_gap(1)});
    CHECK(ours[2] == std::pair<i32, i32>{4, maze_gap(2)});

    // And the length. The vanilla trace is a walk, not a node list, so the
    // comparable quantity is the route's own length in blocks — which for a
    // serpentine with one corridor is determined by the geometry and is
    // therefore something both sides must agree on exactly.
    //
    // Bounded rather than asserted to the block: our path may cut a corner
    // diagonally where a vanilla mob rounds it, and that is a difference of a
    // handful of tenths, not of route.
    CHECK(path.steps.size() >= 40);
    CHECK(path.steps.size() <= 60);
}
