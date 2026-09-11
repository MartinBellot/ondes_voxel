// Rails against the real game.
//
// The placement table comes from scripts/measure_rails.py `shapes`: 1536 rails
// set among every arrangement of four neighbours on a real 1.20.1 server, read
// back off the save. When that file is present (it is gitignored, regenerable)
// every cell is replayed here through `Rails::on_placed`; the frozen cases
// below are a hand-copied subset that runs without it.
#include "ov/gameplay/rails.hpp"

#include "tnt_gravity_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <simdjson.h>

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr i32 kY = -50;

struct World {
    Signals signals;
    Rails   rails;
    World() : signals{*test::pack_blocks(), *test::pack_registries()}, rails{*test::pack_blocks(), signals} {}
};

[[nodiscard]] const World& rail_rules() {
    static const World instance;
    return instance;
}

/// "minecraft:rail[shape=north_south,waterlogged=false]" to a state.
[[nodiscard]] registry::BlockStateId parse(std::string_view text) {
    const registry::BlockRegistry& blocks = *test::pack_blocks();
    const auto                     open   = text.find('[');
    const auto block = blocks.find_block(text.substr(0, open));
    REQUIRE(block);
    registry::BlockStateId state = blocks.default_state(*block);
    if (open == std::string_view::npos) {
        return state;
    }
    std::string_view rest = text.substr(open + 1, text.size() - open - 2);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto pair  = rest.substr(0, comma);
        const auto eq    = pair.find('=');
        const auto property = blocks.find_property(*block, pair.substr(0, eq));
        REQUIRE(property);
        const auto value = pair.substr(eq + 1);
        for (u16 i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks.with_property(state, *property, i);
            }
        }
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return state;
}

[[nodiscard]] std::string_view shape_name(registry::BlockStateId state) {
    const auto shape = rail_rules().rails.shape_of(state);
    return shape ? kRailShapeNames[static_cast<usize>(*shape)] : std::string_view{"-"};
}

enum class Kind { None, Flat, Up, Down };

/// One cell of the bench: neighbours first, each alone, then the centre.
struct Cell {
    test::TestLevel level{*test::pack_blocks(), kY - 1};
    BlockPos        centre{0, kY, 0};

    Cell(std::string_view centre_block, std::string_view initial, std::string_view neighbour,
         const std::array<Kind, 4>& kinds) {
        constexpr std::array<std::pair<i32, i32>, 4> kSteps{{{0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
        const auto rail = parse(std::string{"minecraft:"} + std::string{neighbour});
        for (usize i = 0; i < 4; ++i) {
            const auto [dx, dz] = kSteps[i];
            switch (kinds[i]) {
                case Kind::None: break;
                case Kind::Flat: level.place(centre.offset(dx, 0, dz), rail); break;
                case Kind::Up:
                    level.place(centre.offset(dx, 0, dz), test::state_of("minecraft:stone"));
                    level.place(centre.offset(dx, 1, dz), rail);
                    break;
                case Kind::Down: level.place(centre.offset(dx, -1, dz), rail); break;
            }
        }
        level.place(centre, parse(std::string{"minecraft:"} + std::string{centre_block} +
                                  "[shape=" + std::string{initial} + "]"));
        rail_rules().rails.on_placed(level, centre);
    }

    [[nodiscard]] BlockPos neighbour(usize i, Kind kind) const {
        constexpr std::array<std::pair<i32, i32>, 4> kSteps{{{0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
        const auto [dx, dz] = kSteps[i];
        const i32 dy        = kind == Kind::Up ? 1 : (kind == Kind::Down ? -1 : 0);
        return centre.offset(dx, dy, dz);
    }
};

}  // namespace

TEST_CASE("a lone rail keeps the shape it was set with", "[rails]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const Cell ns{"rail", "north_south", "rail", {Kind::None, Kind::None, Kind::None, Kind::None}};
    CHECK(shape_name(ns.level.block_at(ns.centre)) == "north_south");
    const Cell ew{"rail", "east_west", "rail", {Kind::None, Kind::None, Kind::None, Kind::None}};
    CHECK(shape_name(ew.level.block_at(ew.centre)) == "east_west");
}

TEST_CASE("junctions take the south-east curve", "[rails]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    // N, S, W, E. Measured: every one of these, on a real server.
    const Cell nse{"rail", "north_south", "rail", {Kind::Flat, Kind::Flat, Kind::None, Kind::Flat}};
    CHECK(shape_name(nse.level.block_at(nse.centre)) == "south_east");
    const Cell nwe{"rail", "north_south", "rail", {Kind::Flat, Kind::None, Kind::Flat, Kind::Flat}};
    CHECK(shape_name(nwe.level.block_at(nwe.centre)) == "north_east");
    const Cell nsw{"rail", "north_south", "rail", {Kind::Flat, Kind::Flat, Kind::Flat, Kind::None}};
    CHECK(shape_name(nsw.level.block_at(nsw.centre)) == "south_west");
    const Cell all{"rail", "north_south", "rail", {Kind::Flat, Kind::Flat, Kind::Flat, Kind::Flat}};
    CHECK(shape_name(all.level.block_at(all.centre)) == "south_east");
    // The two it joins turn to face it; the other two are left alone.
    CHECK(shape_name(all.level.block_at(all.neighbour(3, Kind::Flat))) == "east_west");
    CHECK(shape_name(all.level.block_at(all.neighbour(1, Kind::Flat))) == "north_south");
    CHECK(shape_name(all.level.block_at(all.neighbour(2, Kind::Flat))) == "north_south");
}

TEST_CASE("slopes rise towards a rail one block up", "[rails]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const Cell up_n{"rail", "north_south", "rail", {Kind::Up, Kind::None, Kind::None, Kind::None}};
    CHECK(shape_name(up_n.level.block_at(up_n.centre)) == "ascending_north");
    // Both ends raised: the later check wins.
    const Cell both_ns{"rail", "north_south", "rail", {Kind::Up, Kind::Up, Kind::None, Kind::None}};
    CHECK(shape_name(both_ns.level.block_at(both_ns.centre)) == "ascending_south");
    const Cell both_ew{"rail", "north_south", "rail", {Kind::None, Kind::None, Kind::Up, Kind::Up}};
    CHECK(shape_name(both_ew.level.block_at(both_ew.centre)) == "ascending_west");
    // A rail one block down stays flat itself and makes the lower one rise.
    const Cell down{"rail", "north_south", "rail", {Kind::Down, Kind::None, Kind::None, Kind::None}};
    CHECK(shape_name(down.level.block_at(down.centre)) == "north_south");
    CHECK(shape_name(down.level.block_at(down.neighbour(0, Kind::Down))) == "ascending_south");
}

TEST_CASE("a straight-only rail offered both axes keeps its own", "[rails]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const Cell ns{"powered_rail", "north_south", "rail",
                  {Kind::Flat, Kind::None, Kind::Flat, Kind::None}};
    CHECK(shape_name(ns.level.block_at(ns.centre)) == "north_south");
    const Cell ew{"powered_rail", "east_west", "rail",
                  {Kind::Flat, Kind::None, Kind::Flat, Kind::None}};
    CHECK(shape_name(ew.level.block_at(ew.centre)) == "east_west");
}

TEST_CASE("the measured placement table, every cell", "[rails][oracle]") {
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "normalized" / "rails_shapes.json";
    if (test::pack_blocks() == nullptr || !std::filesystem::exists(path)) {
        SUCCEED("no measured table: run scripts/measure_rails.py shapes");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element doc = parser.load(path.string());
    usize                  cells = 0;
    usize                  centres_agree = 0;
    usize                  neighbours_seen = 0;
    usize                  neighbours_agree = 0;
    for (simdjson::dom::element cell : doc["cells"]) {
        const std::string_view centre    = cell["centre"].get_string().value();
        const std::string_view initial   = cell["initial"].get_string().value();
        const std::string_view neighbour = cell["neighbour"].get_string().value();
        std::array<Kind, 4>    kinds{};
        constexpr std::array<std::string_view, 4> kDirs{"north", "south", "west", "east"};
        for (usize i = 0; i < 4; ++i) {
            const std::string_view kind = cell["kinds"][kDirs[i]].get_string().value();
            kinds[i] = kind == "flat" ? Kind::Flat
                       : kind == "up" ? Kind::Up
                       : kind == "down" ? Kind::Down
                                        : Kind::None;
        }
        const Cell ours{centre, initial, neighbour, kinds};
        ++cells;
        const auto want_centre = parse(cell["centre_after"].get_string().value());
        if (ours.level.block_at(ours.centre) == want_centre) {
            ++centres_agree;
        } else {
            UNSCOPED_INFO("centre " << centre << " " << initial << " next to " << neighbour
                                    << ": want " << shape_name(want_centre) << ", got "
                                    << shape_name(ours.level.block_at(ours.centre)));
        }
        for (usize i = 0; i < 4; ++i) {
            if (kinds[i] == Kind::None) {
                continue;
            }
            ++neighbours_seen;
            const auto want = parse(cell["neighbours_after"][kDirs[i]].get_string().value());
            if (ours.level.block_at(ours.neighbour(i, kinds[i])) == want) {
                ++neighbours_agree;
            }
        }
    }
    INFO("centres " << centres_agree << "/" << cells << ", neighbours " << neighbours_agree
                    << "/" << neighbours_seen);
    CHECK(cells == 1536);
    CHECK(centres_agree == cells);
    CHECK(neighbours_agree == neighbours_seen);
}

TEST_CASE("neighbours already connected, as measured", "[rails][oracle]") {
    // scripts/measure_rails.py `busy`: the east neighbour is a full line of
    // three, half a line (one rail north or south of it), or alone; a west and
    // a north neighbour come and go. The rails were set one after another, so
    // they are replayed in the same order, each through its own placement.
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "normalized" / "rails_busy.json";
    if (test::pack_blocks() == nullptr || !std::filesystem::exists(path)) {
        SUCCEED("no measured table: run scripts/measure_rails.py busy");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element doc = parser.load(path.string());
    usize                  cells = 0;
    usize                  agree = 0;
    for (simdjson::dom::element c : doc["cases"]) {
        const std::string_view east   = c["east"].get_string().value();
        const std::string_view west   = c["west"].get_string().value();
        const std::string_view north  = c["north"].get_string().value();
        const std::string_view centre = c["centre"].get_string().value();
        std::vector<BlockPos>  placed;
        if (east == "full") {
            placed = {{1, kY, -1}, {1, kY, 0}, {1, kY, 1}};
        } else if (east == "half") {
            placed = {{1, kY, -1}, {1, kY, 0}};
        } else if (east == "half_south") {
            placed = {{1, kY, 1}, {1, kY, 0}};
        } else {
            placed = {{1, kY, 0}};
        }
        if (west == "free") {
            placed.emplace_back(-1, kY, 0);
        }
        if (north == "free") {
            placed.emplace_back(0, kY, -1);
        }
        test::TestLevel level{*test::pack_blocks(), kY - 1};
        for (const BlockPos pos : placed) {
            level.place(pos, parse("minecraft:rail"));
            rail_rules().rails.on_placed(level, pos);
        }
        const BlockPos middle{0, kY, 0};
        level.place(middle, parse(std::string{"minecraft:"} + std::string{centre}));
        rail_rules().rails.on_placed(level, middle);
        for (i32 dx = -1; dx <= 1; ++dx) {
            for (i32 dz = -1; dz <= 1; ++dz) {
                const std::string key = std::to_string(dx) + "," + std::to_string(dz);
                const auto want = parse(c["after"][key].get_string().value());
                ++cells;
                if (level.block_at(middle.offset(dx, 0, dz)) == want) {
                    ++agree;
                } else {
                    UNSCOPED_INFO(east << " " << west << " " << north << " " << centre << " at "
                                       << key << ": want " << shape_name(want) << ", got "
                                       << shape_name(level.block_at(middle.offset(dx, 0, dz))));
                }
            }
        }
    }
    INFO("cells " << agree << "/" << cells);
    CHECK(cells == 32 * 9);
    CHECK(agree == cells);
}

namespace {

/// Recompute every rail until nothing changes: the notification waves the
/// server's drain runs, without their order.
void settle(test::TestLevel& level, const std::vector<BlockPos>& cells) {
    for (int wave = 0; wave < 64; ++wave) {
        bool changed = false;
        for (const BlockPos pos : cells) {
            changed = rail_rules().rails.neighbour_changed(level, pos).power_changed || changed;
        }
        if (!changed) {
            return;
        }
    }
    FAIL("power never settled");
}

}  // namespace

TEST_CASE("one source powers seventeen rails", "[rails]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    for (const std::string_view kind : {"powered_rail", "activator_rail"}) {
        test::TestLevel       level{*test::pack_blocks(), kY - 1};
        std::vector<BlockPos> line;
        for (i32 x = 0; x < 24; ++x) {
            line.emplace_back(x, kY, 0);
            level.place(line.back(), parse(std::string{"minecraft:"} + std::string{kind}));
            rail_rules().rails.on_placed(level, line.back());
        }
        level.place(BlockPos{12, kY, -1}, test::state_of("minecraft:redstone_block"));
        settle(level, line);
        std::vector<i32> lit;
        for (const BlockPos pos : line) {
            if (rail_rules().rails.powered(level.block_at(pos))) {
                lit.push_back(pos.x);
            }
        }
        REQUIRE(lit.size() == 17);
        CHECK(lit.front() == 4);
        CHECK(lit.back() == 20);

        level.place(BlockPos{12, kY, -1}, test::state_of("minecraft:stone"));
        settle(level, line);
        for (const BlockPos pos : line) {
            CHECK_FALSE(rail_rules().rails.powered(level.block_at(pos)));
        }
    }
}

TEST_CASE("a rail needs a rim under it", "[rails]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const Rails& rails = rail_rules().rails;
    CHECK(rails.rigid_top(test::state_of("minecraft:stone")));
    CHECK(rails.rigid_top(test::state_of("minecraft:glass")));
    CHECK(rails.rigid_top(test::state_of("minecraft:hopper")));
    CHECK(rails.rigid_top(parse("minecraft:oak_slab[type=top]")));
    CHECK_FALSE(rails.rigid_top(parse("minecraft:oak_slab[type=bottom]")));
    CHECK_FALSE(rails.rigid_top(registry::kAirState));

    test::TestLevel level{*test::pack_blocks(), kY - 1};
    const BlockPos  pos{0, kY + 1, 0};  // standing on air
    level.place(pos, parse("minecraft:rail"));
    const RailUpdate update = rails.neighbour_changed(level, pos);
    CHECK(update.broke);
    CHECK(level.block_at(pos) == registry::kAirState);

    // A slope loses its high side.
    test::TestLevel slope{*test::pack_blocks(), kY - 1};
    slope.place(BlockPos{1, kY, 0}, test::state_of("minecraft:stone"));
    slope.place(BlockPos{0, kY, 0}, parse("minecraft:rail[shape=ascending_east]"));
    CHECK_FALSE(rails.neighbour_changed(slope, BlockPos{0, kY, 0}).broke);
    slope.place(BlockPos{1, kY, 0}, registry::kAirState);
    CHECK(rails.neighbour_changed(slope, BlockPos{0, kY, 0}).broke);
}
