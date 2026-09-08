// Fluids, against what the real 1.20.1 server actually did.
//
// The expected shapes in this file are not invented. Each one is a scenario
// that was played on a real server through its console, left to settle, and
// read back out of the Anvil save with `ov-inspect state`. The scripts are in
// docs/provenance/fluides.md; the maps below are transcriptions of what came
// back, character for character.
#include "ov/gameplay/fluid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<FluidRules>              rules;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        Loaded out;
        auto   blocks = registry::BlockRegistry::load(path);
        if (blocks) {
            out.blocks = std::move(*blocks);
            out.rules.emplace(*out.blocks);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] const registry::BlockRegistry& blocks() {
    REQUIRE(loaded().blocks.has_value());
    return *loaded().blocks;
}

[[nodiscard]] const FluidRules& rules() {
    REQUIRE(loaded().rules.has_value());
    return *loaded().rules;
}

[[nodiscard]] registry::BlockStateId state_of(std::string_view name) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    return blocks().default_state(*id);
}

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view                                               name,
    std::span<const std::pair<std::string_view, std::string_view>> properties) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    const auto state = blocks().state_for(*id, properties);
    REQUIRE(state.has_value());
    return *state;
}

/// A world that is a hash map, and nothing else.
///
/// Enough to run every rule in fluid.cpp, which is the point of the interface:
/// the same code the server will run also runs here, with no server anywhere
/// near it.
class TestLevel : public world::LevelWriter {
public:
    explicit TestLevel(world::DimensionTraits traits = {}) : traits_{traits} {}

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::BlockStateId{0} : found->second;
    }

    [[nodiscard]] bool is_loaded(BlockPos pos) const override {
        return shape_.contains_y(pos.y);
    }

    [[nodiscard]] world::WorldShape shape() const override { return shape_; }

    [[nodiscard]] world::DimensionTraits traits() const override { return traits_; }

    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
    }

    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue queue,
                       world::TickPriority priority) override {
        scheduler(queue).schedule(pos, what, delay, now_, priority);
    }

    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue queue) const override {
        return (queue == world::TickQueue::Fluid ? fluid_ticks_ : block_ticks_)
            .is_scheduled(pos, what);
    }

    [[nodiscard]] i64 game_time() const override { return now_; }

    /// Fill a solid floor and clear the air above it.
    void floor(i32 y, i32 half, registry::BlockStateId state) {
        for (i32 z = -half; z <= half; ++z) {
            for (i32 x = -half; x <= half; ++x) {
                set_block(BlockPos{x, y, z}, state);
            }
        }
    }

    /// Run the world forward until nothing is scheduled, or `limit` ticks pass.
    ///
    /// Returns the number of ticks it took to settle, or -1 if it never did —
    /// a fluid that keeps rescheduling for ever is a bug this has to be able to
    /// report rather than hang on.
    i64 settle(const FluidRules& fluid, i64 limit = 20000) {
        std::vector<world::ScheduledTick> due;
        for (i64 elapsed = 0; elapsed < limit; ++elapsed) {
            ++now_;
            fluid_ticks_.collect_due(now_, due);
            if (due.empty() && fluid_ticks_.pending() == 0) {
                return elapsed;
            }
            for (const world::ScheduledTick& tick : due) {
                fluid.tick(*this, tick.pos);
            }
        }
        return -1;
    }

    [[nodiscard]] usize pending_fluid_ticks() const { return fluid_ticks_.pending(); }

private:
    /// A tuple rather than a packed integer.
    ///
    /// The first version of this XOR-ed three shifted coordinates into one i64
    /// and the fields overlapped, so distant positions collided and the puddle
    /// came out as a sparse skeleton. The bug was in the test world, not in the
    /// rule under test, which is the most expensive kind — it looked exactly
    /// like a broken flow.
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos pos) {
        return {pos.x, pos.y, pos.z};
    }

    world::BlockTickScheduler& scheduler(world::TickQueue queue) {
        return queue == world::TickQueue::Fluid ? fluid_ticks_ : block_ticks_;
    }

    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells_;
    world::BlockTickScheduler             fluid_ticks_;
    world::BlockTickScheduler             block_ticks_;
    world::WorldShape                     shape_{world::WorldShape::overworld()};
    world::DimensionTraits                traits_{};
    i64                                   now_{0};
};

/// Place a block and wake the fluids around it.
///
/// What a real level does for free: setting a block notifies its neighbours.
/// The test world's `set_block` is the raw primitive the rules themselves call,
/// so it cannot notify anything without recursing — the courtesy lives here.
void place(TestLevel& level, BlockPos pos, registry::BlockStateId state);

/// Render one horizontal slice the way the measurement scripts printed it:
/// a hex digit for a water level, an uppercase one for lava, '.' for air.
[[nodiscard]] std::vector<std::string> slice(const TestLevel& level, i32 y, i32 half) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::vector<std::string>          rows;
    for (i32 z = -half; z <= half; ++z) {
        std::string row;
        for (i32 x = -half; x <= half; ++x) {
            const registry::BlockStateId state = level.block_at(BlockPos{x, y, z});
            const FluidState             fluid = rules().fluid_at(state);
            if (fluid.kind == FluidKind::Water) {
                row += kDigits[fluid.falling ? 8 : fluid.amount];
            } else if (fluid.kind == FluidKind::Lava) {
                row += fluid.falling ? '8' : static_cast<char>('A' + fluid.amount);
            } else if (blocks().is_air(blocks().block_of(state))) {
                row += '.';
            } else if (state == state_of("minecraft:obsidian")) {
                row += 'O';
            } else if (state == state_of("minecraft:cobblestone")) {
                row += 'C';
            } else {
                row += '#';
            }
        }
        rows.push_back(row);
    }
    return rows;
}

void place(TestLevel& level, BlockPos pos, registry::BlockStateId state) {
    level.set_block(pos, state);
    rules().on_neighbour_changed(level, pos);
    for (u8 d = 0; d < kDirectionCount; ++d) {
        rules().on_neighbour_changed(level, pos.offset(static_cast<Direction>(d)));
    }
}

/// How many positions of the two maps agree, and how many there are.
struct Agreement {
    usize same{0};
    usize total{0};
};

[[nodiscard]] Agreement compare(const std::vector<std::string>& got,
                                const std::vector<std::string>& want) {
    Agreement out;
    REQUIRE(got.size() == want.size());
    for (usize row = 0; row < got.size(); ++row) {
        REQUIRE(got[row].size() == want[row].size());
        for (usize column = 0; column < got[row].size(); ++column) {
            ++out.total;
            if (got[row][column] == want[row][column]) {
                ++out.same;
            }
        }
    }
    return out;
}

void require_identical(const std::vector<std::string>& got,
                       const std::vector<std::string>& want) {
    const Agreement agreement = compare(got, want);
    if (agreement.same != agreement.total) {
        // Print both so a failure says what changed rather than only that
        // something did.
        for (usize row = 0; row < got.size(); ++row) {
            UNSCOPED_INFO(want[row] << "   want\n" << got[row] << "   got");
        }
    }
    REQUIRE(agreement.same == agreement.total);
}

}  // namespace

// ── The block states ────────────────────────────────────────────────────────

TEST_CASE("the level property maps to amount and the falling flag", "[fluid]") {
    const FluidRules& fluid = rules();

    const FluidState source = fluid.fluid_at(state_of("minecraft:water"));
    REQUIRE(source.kind == FluidKind::Water);
    REQUIRE(source.is_source());
    REQUIRE(source.amount == 0);
    REQUIRE(source.height() == 8);

    for (u8 amount = 1; amount <= 7; ++amount) {
        const std::string                                        text = std::to_string(amount);
        const std::array<std::pair<std::string_view, std::string_view>, 1> pair{
            std::pair<std::string_view, std::string_view>{"level", text}};
        const FluidState flowing = fluid.fluid_at(state_of("minecraft:water", pair));
        REQUIRE(flowing.kind == FluidKind::Water);
        REQUIRE(flowing.amount == amount);
        REQUIRE_FALSE(flowing.falling);
        REQUIRE_FALSE(flowing.is_source());
    }

    // level=8 is what a real server writes for every falling cell of a shaft,
    // whatever the amount would otherwise be.
    const std::array<std::pair<std::string_view, std::string_view>, 1> eight{
        std::pair<std::string_view, std::string_view>{"level", "8"}};
    const FluidState falling = fluid.fluid_at(state_of("minecraft:water", eight));
    REQUIRE(falling.falling);
    REQUIRE(falling.kind == FluidKind::Water);
    REQUIRE_FALSE(falling.is_source());

    // And back again.
    REQUIRE(fluid.state_for(FluidState{FluidKind::Water, 0, false}) == state_of("minecraft:water"));
    REQUIRE(fluid.state_for(FluidState{FluidKind::Water, 3, true}) ==
            state_of("minecraft:water", eight));
}

TEST_CASE("a scheduled fluid tick names the fluid, not the block", "[fluid]") {
    // Measured in a save from a real server: fluid_ticks carry
    // "minecraft:flowing_lava" and "minecraft:lava", never "minecraft:lava"
    // for both. Writing the block name produces a file vanilla loads with
    // every pending flow silently dropped.
    REQUIRE(FluidRules::tick_name(FluidState{FluidKind::Water, 0, false}) == "minecraft:water");
    REQUIRE(FluidRules::tick_name(FluidState{FluidKind::Water, 3, false}) ==
            "minecraft:flowing_water");
    REQUIRE(FluidRules::tick_name(FluidState{FluidKind::Water, 0, true}) ==
            "minecraft:flowing_water");
    REQUIRE(FluidRules::tick_name(FluidState{FluidKind::Lava, 0, false}) == "minecraft:lava");
    REQUIRE(FluidRules::tick_name(FluidState{FluidKind::Lava, 2, false}) ==
            "minecraft:flowing_lava");
}

TEST_CASE("the tick delays are the ones the server wrote down", "[fluid]") {
    // Not timed: read out of the `t` field of a freshly placed source in a real
    // save, so these are exact integers.
    const FluidRules& fluid = rules();
    REQUIRE(fluid.tick_delay(FluidKind::Water, world::DimensionTraits{}) == 5);
    REQUIRE(fluid.tick_delay(FluidKind::Lava, world::DimensionTraits{}) == 30);
    REQUIRE(fluid.tick_delay(FluidKind::Lava, world::DimensionTraits{true, false}) == 10);
    // Water is unaffected by the Nether's heat as far as its clock goes.
    REQUIRE(fluid.tick_delay(FluidKind::Water, world::DimensionTraits{true, false}) == 5);
}

// ── The puddle ──────────────────────────────────────────────────────────────

TEST_CASE("a water source on a flat floor makes the diamond vanilla makes", "[fluid][parity]") {
    // Scenario `flaque_eau_sol_plat`. 19x19 read back from the save; the level
    // at every position is the Manhattan distance from the source, out to 7.
    TestLevel level;
    level.floor(-61, 12, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{
        "...................",  //
        "...................",  //
        ".........7.........",  //
        "........767........",  //
        ".......76567.......",  //
        "......7654567......",  //
        ".....765434567.....",  //
        "....76543234567....",  //
        "...7654321234567...",  //
        "..765432101234567..",  //
        "...7654321234567...",  //
        "....76543234567....",  //
        ".....765434567.....",  //
        "......7654567......",  //
        ".......76567.......",  //
        "........767........",  //
        ".........7.........",  //
        "...................",  //
        "..................."};
    require_identical(slice(level, -60, 9), want);
}

TEST_CASE("lava thins by two in the Overworld and by one in the Nether", "[fluid][parity]") {
    // Scenarios `flaque_lave_sol_plat` and `nether_flaque_lave`. The same
    // source makes a radius-3 pool on stone and a radius-7 one on netherrack —
    // the Nether puddle is indistinguishable from water's.
    SECTION("overworld") {
        TestLevel level;
        level.floor(-61, 10, state_of("minecraft:stone"));
        REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Lava));
        REQUIRE(level.settle(rules()) >= 0);

        const std::vector<std::string> want{
            ".............",  //
            ".............",  //
            ".............",  //
            "......G......",  //
            ".....GEG.....",  //
            "....GECEG....",  //
            "...GECACEG...",  //
            "....GECEG....",  //
            ".....GEG.....",  //
            "......G......",  //
            ".............",  //
            ".............",  //
            "............."};
        require_identical(slice(level, -60, 6), want);
    }

    SECTION("nether") {
        TestLevel level{world::DimensionTraits{true, false}};
        level.floor(-61, 12, state_of("minecraft:netherrack"));
        REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Lava));
        REQUIRE(level.settle(rules()) >= 0);

        const std::vector<std::string> want{
            ".....HGH.....",  //
            "....HGFGH....",  //
            "...HGFEFGH...",  //
            "..HGFEDEFGH..",  //
            ".HGFEDCDEFGH.",  //
            "HGFEDCBCDEFGH",  //
            "GFEDCBABCDEFG",  //
            "HGFEDCBCDEFGH",  //
            ".HGFEDCDEFGH.",  //
            "..HGFEDEFGH..",  //
            "...HGFEFGH...",  //
            "....HGFGH....",  //
            ".....HGH....."};
        require_identical(slice(level, -60, 6), want);
    }
}

// ── The hole search ─────────────────────────────────────────────────────────

TEST_CASE("water goes to a hole instead of spreading", "[fluid][parity]") {
    // Scenario `trou_a_l_est_distance_4`. This is the rule that makes a stream
    // a stream: with a hole four blocks east the source makes a one-block-wide
    // line east and leaves the other three sides completely dry.
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    level.set_block(BlockPos{4, -61, 0}, registry::BlockStateId{0});
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        "........01234....",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        ".................",  //
        "................."};
    require_identical(slice(level, -60, 8), want);
}

TEST_CASE("water steps away from a hole to get round a wall", "[fluid][parity]") {
    // Scenario `trou_derriere_un_mur`, and the sharpest evidence there is that
    // the search is a real breadth-first walk rather than a straight line: the
    // hole is two blocks east, a two-block wall is in the way, and the water's
    // **first step is south** — the opposite of where the hole is.
    TestLevel level;
    level.floor(-61, 8, state_of("minecraft:stone"));
    level.set_block(BlockPos{1, -60, -1}, state_of("minecraft:stone"));
    level.set_block(BlockPos{1, -60, 0}, state_of("minecraft:stone"));
    level.set_block(BlockPos{2, -61, 0}, registry::BlockStateId{0});
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{
        ".............",  //
        ".............",  //
        ".............",  //
        ".............",  //
        ".............",  //
        ".......#.....",  //
        "......0#4....",  //
        "......123....",  //
        ".............",  //
        ".............",  //
        ".............",  //
        ".............",  //
        "............."};
    require_identical(slice(level, -60, 6), want);
}

TEST_CASE("two holes at the same distance both get fed", "[fluid][parity]") {
    // Scenario `deux_trous_equidistants`: every direction tied for the shortest
    // path receives flow, so the water runs both ways and nowhere else.
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    level.set_block(BlockPos{3, -61, 0}, registry::BlockStateId{0});
    level.set_block(BlockPos{-3, -61, 0}, registry::BlockStateId{0});
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{
        ".........",  //
        ".........",  //
        ".........",  //
        ".........",  //
        ".3210123.",  //
        ".........",  //
        ".........",  //
        ".........",  //
        "........."};
    require_identical(slice(level, -60, 4), want);
}

TEST_CASE("a hole placed diagonally fills the rectangle between", "[fluid][parity]") {
    // Scenario `trou_distance_5_diagonale`: the hole is three east and two
    // south, so every cell of the 4x3 box lies on some shortest path and every
    // one of them gets water, at the Manhattan level.
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    level.set_block(BlockPos{3, -61, 2}, registry::BlockStateId{0});
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{
        ".........",  //
        ".........",  //
        ".........",  //
        ".........",  //
        "....0123.",  //
        "....1234.",  //
        "....2345.",  //
        ".........",  //
        "........."};
    require_identical(slice(level, -60, 4), want);
}

TEST_CASE("the search reaches exactly five blocks and no further", "[fluid][parity]") {
    // The pair of scenarios that pins the radius. A hole at five is found and
    // the water beelines; a hole at six is not, and the diamond comes back.
    SECTION("a hole at five is found") {
        TestLevel level;
        level.floor(-61, 10, state_of("minecraft:stone"));
        level.set_block(BlockPos{5, -61, 0}, registry::BlockStateId{0});
        REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
        REQUIRE(level.settle(rules()) >= 0);

        const std::vector<std::string> want{
            ".........",  //
            ".........",  //
            ".........",  //
            ".........",  //
            "....01234",  //
            ".........",  //
            ".........",  //
            ".........",  //
            "........."};
        require_identical(slice(level, -60, 4), want);
    }

    SECTION("a hole at six is not") {
        TestLevel level;
        level.floor(-61, 12, state_of("minecraft:stone"));
        level.set_block(BlockPos{6, -61, 0}, registry::BlockStateId{0});
        REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
        REQUIRE(level.settle(rules()) >= 0);

        // Exactly what the save held: a full diamond, except that the east arm
        // stops at the hole because the water drops through it rather than
        // spreading on.
        const std::vector<std::string> want{
            ".................",  //
            "........7........",  //
            ".......767.......",  //
            "......76567......",  //
            ".....7654567.....",  //
            "....765434567....",  //
            "...76543234567...",  //
            "..7654321234567..",  //
            ".76543210123456..",  //
            "..7654321234567..",  //
            "...76543234567...",  //
            "....765434567....",  //
            ".....7654567.....",  //
            "......76567......",  //
            ".......767.......",  //
            "........7........",  //
            "................."};
        require_identical(slice(level, -60, 8), want);
    }
}

// ── Sources ─────────────────────────────────────────────────────────────────

TEST_CASE("two adjacent water sources make a third", "[fluid][parity]") {
    // Scenario `eau_infinie_deux_sources_alignees`: the gap between two sources
    // one block apart becomes a source itself, which is what makes an infinite
    // water source work.
    TestLevel level;
    level.floor(-61, 6, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{-1, -60, 0}, FluidKind::Water));
    REQUIRE(rules().place_fluid(level, BlockPos{1, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(rules().fluid_at(level.block_at(BlockPos{0, -60, 0})).is_source());

    const std::vector<std::string> want{"21112", "10001", "21112"};
    std::vector<std::string>       got;
    for (i32 z = -1; z <= 1; ++z) {
        std::string row;
        for (i32 x = -2; x <= 2; ++x) {
            row += static_cast<char>(
                '0' + rules().fluid_at(level.block_at(BlockPos{x, -60, z})).amount);
        }
        got.push_back(row);
    }
    require_identical(got, want);
}

TEST_CASE("two diagonal sources do not", "[fluid][parity]") {
    // Scenario `eau_infinie_refusee_diagonale`. Adjacency has to be horizontal
    // and orthogonal; no cell here has two orthogonal source neighbours.
    TestLevel level;
    level.floor(-61, 6, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{-1, -60, -1}, FluidKind::Water));
    REQUIRE(rules().place_fluid(level, BlockPos{1, -60, 1}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{"012", "121", "210"};
    require_identical(slice(level, -60, 1), want);
}

TEST_CASE("lava is never infinite", "[fluid][parity]") {
    // Scenario `lave_pas_infinie`: two lava sources one apart leave the gap
    // flowing at level 2, never a source.
    TestLevel level;
    level.floor(-61, 6, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{-1, -60, 0}, FluidKind::Lava));
    REQUIRE(rules().place_fluid(level, BlockPos{1, -60, 0}, FluidKind::Lava));
    REQUIRE(level.settle(rules()) >= 0);

    const FluidState middle = rules().fluid_at(level.block_at(BlockPos{0, -60, 0}));
    REQUIRE(middle.kind == FluidKind::Lava);
    REQUIRE_FALSE(middle.is_source());
    REQUIRE(middle.amount == 2);
}

// ── Falling ─────────────────────────────────────────────────────────────────

TEST_CASE("water falls down a shaft and spreads at full strength below",
          "[fluid][parity]") {
    // Scenario `chute_puits`. Two things are being checked at once: the shaft
    // holds level 8 the whole way down and wets nothing beside it, and the
    // landing block feeds its neighbours at level 1 — as a source would, not as
    // a level-8 flow would.
    TestLevel level;
    level.floor(-50, 10, state_of("minecraft:stone"));
    level.floor(-56, 10, state_of("minecraft:stone"));
    level.set_block(BlockPos{2, -50, 0}, registry::BlockStateId{0});
    REQUIRE(rules().place_fluid(level, BlockPos{0, -49, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    for (i32 y = -50; y >= -55; --y) {
        const FluidState cell = rules().fluid_at(level.block_at(BlockPos{2, y, 0}));
        REQUIRE(cell.kind == FluidKind::Water);
        REQUIRE(cell.falling);
        // Nothing beside the column — except at the bottom, where the water has
        // landed and is spreading.
        if (y > -55) {
            REQUIRE(rules().fluid_at(level.block_at(BlockPos{2, y, 1})).empty());
            REQUIRE(rules().fluid_at(level.block_at(BlockPos{3, y, 0})).empty());
        }
    }

    // The landing block's neighbours: level 1 in all four directions.
    for (Direction direction : kHorizontal) {
        const FluidState beside =
            rules().fluid_at(level.block_at(BlockPos{2, -55, 0}.offset(direction)));
        REQUIRE(beside.kind == FluidKind::Water);
        REQUIRE(beside.amount == 1);
    }
}

// ── The lava / water table ──────────────────────────────────────────────────

TEST_CASE("water reaching a lava source makes obsidian", "[fluid][parity]") {
    // Scenario `melange_eau_coule_sur_source_de_lave`.
    TestLevel level;
    level.floor(-61, 8, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Lava));
    REQUIRE(rules().place_fluid(level, BlockPos{3, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(level.block_at(BlockPos{0, -60, 0}) == state_of("minecraft:obsidian"));
}

TEST_CASE("water above a lava source makes obsidian and survives", "[fluid][parity]") {
    // Scenario `melange_eau_au_dessus_source_de_lave`: the lava becomes
    // obsidian, the water stays a source where it was.
    TestLevel level;
    level.floor(-61, 6, state_of("minecraft:stone"));
    place(level, BlockPos{0, -60, 0}, state_of("minecraft:lava"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -59, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(level.block_at(BlockPos{0, -60, 0}) == state_of("minecraft:obsidian"));
    REQUIRE(rules().fluid_at(level.block_at(BlockPos{0, -59, 0})).is_source());
}

TEST_CASE("water reaching flowing lava makes cobblestone", "[fluid][parity]") {
    // Scenario `lave_installee_puis_eau_laterale`, which had to be played in
    // two acts: water moves at 5 ticks a block and lava at 30, so on any flat
    // ground the water arrives before the lava has flowed at all and only the
    // **source** ever converts. Letting the lava settle first is the only way
    // to see its flow meet water.
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Lava));
    REQUIRE(level.settle(rules()) >= 0);
    // The lava's own flow, before the water: level 2 one block out.
    REQUIRE(rules().fluid_at(level.block_at(BlockPos{1, -60, 0})).amount == 2);

    REQUIRE(rules().place_fluid(level, BlockPos{4, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    // The flow at the edge hardened into cobblestone; the source is untouched
    // because the water never reached it.
    REQUIRE(level.block_at(BlockPos{3, -60, 0}) == state_of("minecraft:cobblestone"));
    REQUIRE(rules().fluid_at(level.block_at(BlockPos{0, -60, 0})).is_source());
}

TEST_CASE("water poured over a lava pool leaves obsidian inside cobblestone",
          "[fluid][parity]") {
    // Scenario `lave_installee_puis_eau_dessus`, which shows the whole table in
    // one picture: an obsidian centre where the source was, a cobblestone ring
    // where the flow was.
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Lava));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(rules().place_fluid(level, BlockPos{0, -56, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(level.block_at(BlockPos{0, -60, 0}) == state_of("minecraft:obsidian"));
    for (Direction direction : kHorizontal) {
        REQUIRE(level.block_at(BlockPos{0, -60, 0}.offset(direction)) ==
                state_of("minecraft:cobblestone"));
    }
}

TEST_CASE("lava arriving on water makes stone, source or flow", "[fluid][parity]") {
    // Scenario `measure_mix2`: lava dropped onto a water source, a thin flow
    // and a thick flow. All three left stone where the water was, and the lava
    // above it untouched.
    SECTION("onto a source") {
        TestLevel level;
        level.floor(-61, 6, state_of("minecraft:stone"));
        place(level, BlockPos{0, -60, 0}, state_of("minecraft:water"));
        place(level, BlockPos{0, -59, 0}, state_of("minecraft:lava"));
        REQUIRE(level.settle(rules()) >= 0);

        REQUIRE(level.block_at(BlockPos{0, -60, 0}) == state_of("minecraft:stone"));
        REQUIRE(rules().fluid_at(level.block_at(BlockPos{0, -59, 0})).kind == FluidKind::Lava);
    }

    SECTION("onto a flow") {
        TestLevel level;
        level.floor(-61, 10, state_of("minecraft:stone"));
        REQUIRE(rules().place_fluid(level, BlockPos{-4, -60, 0}, FluidKind::Water));
        REQUIRE(level.settle(rules()) >= 0);
        REQUIRE(rules().fluid_at(level.block_at(BlockPos{0, -60, 0})).amount == 4);

        place(level, BlockPos{0, -55, 0}, state_of("minecraft:lava"));
        REQUIRE(level.settle(rules()) >= 0);
        REQUIRE(level.block_at(BlockPos{0, -60, 0}) == state_of("minecraft:stone"));
    }
}

// ── Waterlogging ────────────────────────────────────────────────────────────

TEST_CASE("a waterlogged block feeds its neighbours like a source", "[fluid][parity]") {
    // Scenario `waterlogging_dalle_source`: a waterlogged slab alone on a floor
    // grows the diamond a source grows.
    TestLevel level;
    level.floor(-61, 6, state_of("minecraft:stone"));
    const std::pair<std::string_view, std::string_view> props[]{{"type", "bottom"},
                                                                {"waterlogged", "true"}};
    place(level, BlockPos{0, -60, 0}, state_of("minecraft:oak_slab", props));
    REQUIRE(level.settle(rules()) >= 0);

    const std::vector<std::string> want{"32123", "21s12", "32123"};
    std::vector<std::string>       got;
    for (i32 z = -1; z <= 1; ++z) {
        std::string row;
        for (i32 x = -2; x <= 2; ++x) {
            const registry::BlockStateId state = level.block_at(BlockPos{x, -60, z});
            if (blocks().block_of(state) == *blocks().find_block("minecraft:oak_slab")) {
                row += 's';
            } else {
                row += static_cast<char>('0' + rules().fluid_at(state).amount);
            }
        }
        got.push_back(row);
    }
    require_identical(got, want);
}

TEST_CASE("flowing water does not waterlog what it meets", "[fluid][parity]") {
    // Scenario `waterlogging_eau_coulante_sur_dalle`: the slab in the path
    // stayed `waterlogged=false` and the water went round it. Waterlogging is
    // something a placement does, never a flow.
    TestLevel level;
    level.floor(-61, 8, state_of("minecraft:stone"));
    const std::pair<std::string_view, std::string_view> props[]{{"type", "bottom"},
                                                                {"waterlogged", "false"}};
    const registry::BlockStateId dry = state_of("minecraft:oak_slab", props);
    level.set_block(BlockPos{2, -60, 0}, dry);
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(level.block_at(BlockPos{2, -60, 0}) == dry);
    REQUIRE_FALSE(rules().releases_water(level.block_at(BlockPos{2, -60, 0})));
    // And it went around. Three east of the source the save reads level 5, not
    // the 3 a straight line would have cost: the water goes south, east, east,
    // then north again, and pays for every step.
    REQUIRE(rules().fluid_at(level.block_at(BlockPos{3, -60, 0})).amount == 5);
}

TEST_CASE("a bucket fills and empties a waterloggable block", "[fluid]") {
    TestLevel level;
    level.floor(-61, 4, state_of("minecraft:stone"));
    const std::pair<std::string_view, std::string_view> dry_props[]{{"type", "bottom"},
                                                                    {"waterlogged", "false"}};
    const registry::BlockStateId dry = state_of("minecraft:oak_slab", dry_props);
    level.set_block(BlockPos{0, -60, 0}, dry);

    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(rules().releases_water(level.block_at(BlockPos{0, -60, 0})));
    // Breaking it now gives the water back.
    REQUIRE(rules().state_after_break(level.block_at(BlockPos{0, -60, 0})) ==
            state_of("minecraft:water"));

    // Filling it twice is refused rather than quietly repeated.
    REQUIRE_FALSE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));

    const auto picked = rules().pick_up_fluid(level, BlockPos{0, -60, 0});
    REQUIRE(picked.has_value());
    REQUIRE(*picked == FluidKind::Water);
    REQUIRE(level.block_at(BlockPos{0, -60, 0}) == dry);
    REQUIRE(rules().state_after_break(dry) == registry::BlockStateId{0});
}

TEST_CASE("there is no lavalogging", "[fluid]") {
    TestLevel level;
    level.floor(-61, 4, state_of("minecraft:stone"));
    const std::pair<std::string_view, std::string_view> props[]{{"type", "bottom"},
                                                                {"waterlogged", "false"}};
    level.set_block(BlockPos{0, -60, 0}, state_of("minecraft:oak_slab", props));
    REQUIRE_FALSE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Lava));
}

TEST_CASE("only a source fills a bucket", "[fluid]") {
    TestLevel level;
    level.floor(-61, 8, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(rules().pick_up_fluid(level, BlockPos{0, -60, 0}).has_value());
    REQUIRE_FALSE(rules().pick_up_fluid(level, BlockPos{3, -60, 0}).has_value());
}

// ── Draining ────────────────────────────────────────────────────────────────

TEST_CASE("removing the source drains the whole puddle", "[fluid]") {
    // Breaking a dam. Nothing above is a special case for this: each cell
    // recomputes itself from its neighbours, so the emptiness travels outward
    // the same way the water did.
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);
    REQUIRE(rules().fluid_at(level.block_at(BlockPos{7, -60, 0})).amount == 7);

    REQUIRE(rules().pick_up_fluid(level, BlockPos{0, -60, 0}).has_value());
    for (Direction direction : kHorizontal) {
        rules().on_neighbour_changed(level, BlockPos{0, -60, 0}.offset(direction));
    }
    REQUIRE(level.settle(rules()) >= 0);

    for (i32 z = -8; z <= 8; ++z) {
        for (i32 x = -8; x <= 8; ++x) {
            INFO("at " << x << "," << z);
            REQUIRE(rules().fluid_at(level.block_at(BlockPos{x, -60, z})).empty());
        }
    }
}

// ── The current, and the sponge ─────────────────────────────────────────────

TEST_CASE("still water pushes nothing and a flow pushes downhill", "[fluid]") {
    TestLevel level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    // At the source, the four neighbours are symmetric, so the pushes cancel.
    const Vec3d at_source = rules().flow_vector(level, BlockPos{0, -60, 0});
    REQUIRE(at_source.x == 0.0);
    REQUIRE(at_source.z == 0.0);

    // Three blocks east, the water is thinner to the east than to the west, so
    // the push is eastward and has no z component.
    const Vec3d downhill = rules().flow_vector(level, BlockPos{3, -60, 0});
    REQUIRE(downhill.x > 0.0);
    REQUIRE(downhill.z == 0.0);

    // Off the axis it points away from the source in both.
    const Vec3d diagonal = rules().flow_vector(level, BlockPos{2, -60, 2});
    REQUIRE(diagonal.x > 0.0);
    REQUIRE(diagonal.z > 0.0);
}

TEST_CASE("a sponge dries the puddle it is dropped into", "[fluid][parity]") {
    // Scenario `eponge_dans_une_flaque`: the sponge absorbed the whole
    // 15x15 puddle and became wet.
    TestLevel level;
    level.floor(-61, 12, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    const usize taken = rules().absorb(level, BlockPos{2, -60, 0});
    REQUIRE(taken > 0);
    REQUIRE(taken <= FluidConstants::kSpongeMax);

    // Everything within one block of the sponge is dry, whatever the cap did
    // further out.
    for (Direction direction : kHorizontal) {
        REQUIRE(rules().fluid_at(level.block_at(BlockPos{2, -60, 0}.offset(direction))).empty());
    }
}

TEST_CASE("a sponge takes at most sixty-five blocks", "[fluid]") {
    // A deep pool, so the cap is what stops it rather than the water running
    // out.
    TestLevel level;
    level.floor(-61, 12, state_of("minecraft:stone"));
    for (i32 y = -60; y <= -56; ++y) {
        for (i32 z = -6; z <= 6; ++z) {
            for (i32 x = -6; x <= 6; ++x) {
                level.set_block(BlockPos{x, y, z}, state_of("minecraft:water"));
            }
        }
    }
    REQUIRE(rules().absorb(level, BlockPos{0, -58, 0}) == FluidConstants::kSpongeMax);
}

// ── The abstraction itself ──────────────────────────────────────────────────

TEST_CASE("fluid never flows into a chunk that is not loaded", "[fluid]") {
    // Otherwise the world would depend on where the player happened to walk,
    // which is a whole class of save corruption. `is_loaded` is on the
    // interface exactly so a rule can refuse rather than guess.
    class HalfLoaded final : public TestLevel {
    public:
        [[nodiscard]] bool is_loaded(BlockPos pos) const override {
            return pos.x < 3 && TestLevel::is_loaded(pos);
        }
    };

    HalfLoaded level;
    level.floor(-61, 10, state_of("minecraft:stone"));
    REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
    REQUIRE(level.settle(rules()) >= 0);

    REQUIRE(rules().fluid_at(level.block_at(BlockPos{2, -60, 0})).kind == FluidKind::Water);
    REQUIRE(rules().fluid_at(level.block_at(BlockPos{3, -60, 0})).empty());
}

// ── Validation hors échantillon ─────────────────────────────────────────────
//
// Everything above was measured first and coded second, so replaying it proves
// the transcription and not much more. These four were not: the geometry comes
// from a fixed-seed pseudo-random generator — scattered walls, holes at
// arbitrary distances — and no case was picked for being easy. The terrain is
// the one played on the real server, block for block, and the expected map is
// what its save held.
//
// Replayable with `scripts/measure_fluid_maze.py`.
namespace {

struct Maze {
    std::string_view                     name;
    std::span<const std::pair<i32, i32>> walls;
    std::span<const std::pair<i32, i32>> holes;
    std::span<const std::string_view>    want;
};

constexpr std::array<std::pair<i32, i32>, 40> kMaze1Walls{{
    {-2, 8}, {4, -3}, {2, 8}, {1, 5}, {6, -7}, {8, 9}, {-9, 7}, {5, 1},
    {-5, 7}, {7, -9}, {-4, -1}, {-1, 8}, {8, -7}, {7, 2}, {-5, -8}, {5, 2},
    {-6, 1}, {3, -9}, {5, -6}, {3, 7}, {7, -4}, {-3, -1}, {-1, -7}, {-7, -9},
    {9, -8}, {7, -6}, {9, -5}, {3, -4}, {-8, 3}, {3, -7}, {2, 9}, {4, -7},
    {3, 3}, {0, -5}, {0, 2}, {5, -7}, {6, -4}, {8, 6}, {3, 2}, {-5, 5}
}};
constexpr std::array<std::pair<i32, i32>, 2> kMaze1Holes{{
    {2, 6}, {8, -9}
}};
constexpr std::array<std::string_view, 19> kMaze1Want{{
    "..#.........#...#..",
    "....#.............#",
    "........#...####.#.",
    "........7.7...#.#..",
    ".......76#67......#",
    "......765456#..##..",
    ".....76543456#.....",
    "....76543234567....",
    "...76##321234567...",
    "..765432101234567..",
    "...#6543212345#7...",
    "....76543#34#6#.#..",
    ".#...7654545#7.....",
    "......765656.......",
    "....#..767#7.......",
    "........7........#.",
    "#...#.......#......",
    ".......##..#.......",
    "...........#.....#.",
}};

constexpr std::array<std::pair<i32, i32>, 55> kMaze2Walls{{
    {3, 3}, {-6, 6}, {-5, 6}, {0, -9}, {2, -6}, {-2, 4}, {1, 7}, {-2, 1},
    {-1, -9}, {-4, 8}, {-4, 6}, {8, 1}, {-1, -8}, {9, -7}, {-4, 5}, {3, -5},
    {8, -7}, {-1, 4}, {-5, -9}, {-8, -9}, {5, 0}, {-7, 4}, {8, -2}, {2, -8},
    {2, -4}, {-7, 2}, {-5, 9}, {4, -2}, {9, -3}, {-7, -5}, {-8, 6}, {7, -6},
    {-3, 0}, {3, 5}, {-4, -3}, {4, -3}, {-6, -6}, {1, 4}, {2, 5}, {-8, 3},
    {9, -4}, {6, 3}, {-7, 3}, {-2, 2}, {-4, 1}, {4, -8}, {-4, -5}, {-7, 9},
    {6, 2}, {3, 7}, {9, 4}, {7, 4}, {-2, -2}, {-4, 7}, {-9, 7}
}};
constexpr std::array<std::pair<i32, i32>, 1> kMaze2Holes{{
    {-5, -6}
}};
constexpr std::array<std::string_view, 19> kMaze2Want{{
    ".#..#...##.........",
    "........#..#.#.....",
    ".................##",
    "...#....767#....#..",
    "..#..#.76567#......",
    "......76545#7.....#",
    ".....#6543456#....#",
    "....765#32345#7..#.",
    "....654321234567...",
    "....76#2101234#....",
    ".....#.#21234567.#.",
    "..#...7#3234567#...",
    ".##..7654345#7.#...",
    "..#...7##4#67...#.#",
    ".....#.7656##......",
    ".#.###..767........",
    "#....#...7#.#......",
    ".....#.............",
    "..#.#..............",
}};

constexpr std::array<std::pair<i32, i32>, 25> kMaze3Walls{{
    {-9, 4}, {3, -5}, {-7, -5}, {3, -3}, {-1, -5}, {7, -5}, {-3, 7}, {-6, 4},
    {-1, -2}, {8, -8}, {1, 2}, {-3, 4}, {-9, 0}, {6, -4}, {5, -9}, {-6, -4},
    {-9, 2}, {8, 1}, {6, -9}, {-5, 4}, {-6, -7}, {5, 1}, {5, -3}, {7, 9},
    {6, -7}
}};
constexpr std::array<std::pair<i32, i32>, 4> kMaze3Holes{{
    {5, -2}, {-4, -4}, {-4, -7}, {7, 1}
}};
constexpr std::array<std::string_view, 19> kMaze3Want{{
    "..............##...",
    ".................#.",
    "...#...........#...",
    "...................",
    "..#.....#...#...#..",
    "...#..76.......#...",
    ".....765....#.#....",
    ".....654#234567....",
    ".....5432123456....",
    "#....4321012345....",
    ".....543212345#..#.",
    "#...765432#4567....",
    ".....765434567.....",
    "#..##.#654567......",
    ".......76567.......",
    "........767........",
    "......#..7.........",
    "...................",
    "................#..",
}};

constexpr std::array<std::pair<i32, i32>, 70> kMaze4Walls{{
    {6, 4}, {6, -3}, {3, 3}, {6, 5}, {3, -7}, {5, -5}, {-2, -5}, {6, 7},
    {7, 4}, {-9, 9}, {6, -7}, {-7, 0}, {0, -2}, {1, 1}, {0, -9}, {4, 9},
    {4, 6}, {-5, 2}, {-8, 9}, {1, 5}, {-1, 7}, {2, -4}, {4, 2}, {9, -9},
    {1, -6}, {3, -9}, {1, -2}, {-8, 5}, {5, 8}, {3, -2}, {-4, 7}, {9, 7},
    {8, 5}, {-6, 7}, {1, -9}, {9, -7}, {-5, -4}, {0, -4}, {6, -1}, {4, -2},
    {-9, 7}, {7, 0}, {1, 3}, {4, -1}, {-4, -2}, {-2, -4}, {-2, 9}, {-1, -4},
    {1, -4}, {-8, 7}, {-9, -2}, {7, -1}, {9, -3}, {5, 9}, {-6, -9}, {4, 3},
    {7, -9}, {3, 1}, {-4, -9}, {0, 6}, {-6, 6}, {7, 5}, {8, 9}, {4, -6},
    {-3, 5}, {5, -6}, {2, 9}, {9, 4}, {-9, 3}, {1, -5}
}};
constexpr std::array<std::pair<i32, i32>, 3> kMaze4Holes{{
    {8, -2}, {-1, 2}, {9, 5}
}};
constexpr std::array<std::string_view, 19> kMaze4Want{{
    "...#.#...##.#...#.#",
    "...................",
    "............#..#..#",
    "..........#..##....",
    ".......#..#...#....",
    "....#..#####.......",
    "...............#..#",
    "#....#...##.##.....",
    ".............#.##..",
    "..#.....10......#..",
    "........21#.#......",
    "....#...32...#.....",
    "#.........#.##.....",
    "...............##.#",
    ".#....#...#....###.",
    "...#.....#...#.....",
    "##.#.#..#......#..#",
    "..............#....",
    "##.....#...#.##..#.",
}};

}  // namespace

TEST_CASE("a maze nobody designed comes out the same", "[fluid][parity]") {
    const std::array<Maze, 4> mazes{{
        Maze{"maze_1", kMaze1Walls, kMaze1Holes, kMaze1Want},
        Maze{"maze_2", kMaze2Walls, kMaze2Holes, kMaze2Want},
        Maze{"maze_3", kMaze3Walls, kMaze3Holes, kMaze3Want},
        Maze{"maze_4", kMaze4Walls, kMaze4Holes, kMaze4Want},
    }};

    for (const Maze& maze : mazes) {
        INFO("maze " << maze.name);
        TestLevel level;
        level.floor(-61, 11, state_of("minecraft:stone"));
        for (const auto& [x, z] : maze.walls) {
            level.set_block(BlockPos{x, -60, z}, state_of("minecraft:stone"));
        }
        for (const auto& [x, z] : maze.holes) {
            level.set_block(BlockPos{x, -61, z}, registry::BlockStateId{0});
        }
        REQUIRE(rules().place_fluid(level, BlockPos{0, -60, 0}, FluidKind::Water));
        REQUIRE(level.settle(rules()) >= 0);

        std::vector<std::string> want;
        want.reserve(maze.want.size());
        for (std::string_view row : maze.want) {
            want.emplace_back(row);
        }
        require_identical(slice(level, -60, 9), want);
    }
}
