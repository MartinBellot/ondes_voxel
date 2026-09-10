// Nether portals: the frame rules, the arithmetic of arriving, the new portal.
//
// The limits and the rules here are the Minecraft Wiki's (Nether portal, Java
// 1.20); where a portal is *placed* is measured against the real server by
// scripts/measure_nether_portal.py, and the figures are in
// docs/provenance/nether.md.
#include "ov/gameplay/nether_portal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <tuple>

using namespace ov;
using namespace ov::gameplay;
using Catch::Approx;

namespace {

[[nodiscard]] const registry::BlockRegistry& blocks() {
    static const auto pack = registry::BlockRegistry::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    REQUIRE(pack.has_value());
    return *pack;
}

[[nodiscard]] registry::BlockStateId block(std::string_view name) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    return blocks().default_state(*id);
}

class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return world::WorldShape::nether(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }
    void set_block(BlockPos pos, registry::BlockStateId state) override { cells_[key(pos)] = state; }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

    [[nodiscard]] std::string_view name_at(BlockPos pos) const {
        const auto state = block_at(pos);
        return state == registry::kAirState ? "minecraft:air"
                                            : ::blocks().block_name(::blocks().block_of(state));
    }

private:
    using Key = std::tuple<i32, i32, i32>;
    [[nodiscard]] static Key key(BlockPos pos) { return {pos.x, pos.y, pos.z}; }
    std::map<Key, registry::BlockStateId> cells_;
};

/// An obsidian frame around an inside of `width` x `height` on the x axis,
/// with or without its corners.
void build_frame(MapLevel& level, BlockPos inside, i32 width, i32 height, bool corners) {
    const auto obsidian = block("minecraft:obsidian");
    for (i32 i = -1; i <= width; ++i) {
        for (i32 j = -1; j <= height; ++j) {
            const bool edge   = i == -1 || i == width || j == -1 || j == height;
            const bool corner = (i == -1 || i == width) && (j == -1 || j == height);
            if (edge && (corners || !corner)) {
                level.set_block({inside.x + i, inside.y + j, inside.z}, obsidian);
            }
        }
    }
}

}  // namespace

TEST_CASE("a frame of every legal size lights, corners or not", "[nether][portal]") {
    const PortalRules rules{blocks()};
    for (const auto [width, height] : {std::pair{2, 3}, std::pair{21, 21}, std::pair{5, 3}}) {
        for (const bool corners : {true, false}) {
            MapLevel level;
            build_frame(level, {10, 40, 7}, width, height, corners);
            level.set_block({11, 41, 7}, block("minecraft:fire"));
            REQUIRE(rules.on_block_changed(level, {11, 41, 7}) ==
                    static_cast<usize>(width * height));
            CHECK(level.name_at({10, 40, 7}) == "minecraft:nether_portal");
            CHECK(level.name_at({10 + width - 1, 40 + height - 1, 7}) == "minecraft:nether_portal");
            CHECK(rules.axis_of(level.block_at({10, 40, 7})) == PortalAxis::X);
        }
    }
}

TEST_CASE("a frame out of range, or unfinished, does not light", "[nether][portal]") {
    const PortalRules rules{blocks()};
    SECTION("too narrow") {
        MapLevel level;
        build_frame(level, {0, 40, 0}, 1, 3, true);
        level.set_block({0, 40, 0}, block("minecraft:fire"));
        CHECK(rules.on_block_changed(level, {0, 40, 0}) == 0);
    }
    SECTION("too tall") {
        MapLevel level;
        build_frame(level, {0, 40, 0}, 2, 22, true);
        level.set_block({0, 40, 0}, block("minecraft:fire"));
        CHECK(rules.on_block_changed(level, {0, 40, 0}) == 0);
    }
    SECTION("a missing side block") {
        MapLevel level;
        build_frame(level, {0, 40, 0}, 2, 3, true);
        level.set_block({-1, 41, 0}, registry::kAirState);
        level.set_block({0, 40, 0}, block("minecraft:fire"));
        CHECK(rules.on_block_changed(level, {0, 40, 0}) == 0);
    }
    SECTION("something inside") {
        MapLevel level;
        build_frame(level, {0, 40, 0}, 2, 3, true);
        level.set_block({1, 42, 0}, block("minecraft:netherrack"));
        level.set_block({0, 40, 0}, block("minecraft:fire"));
        CHECK(rules.on_block_changed(level, {0, 40, 0}) == 0);
    }
}

TEST_CASE("a frame in the z plane lights on the z axis", "[nether][portal]") {
    const PortalRules rules{blocks()};
    MapLevel          level;
    const auto        obsidian = block("minecraft:obsidian");
    for (i32 i = -1; i <= 2; ++i) {
        for (i32 j = -1; j <= 3; ++j) {
            if (i == -1 || i == 2 || j == -1 || j == 3) {
                level.set_block({5, 60 + j, 3 + i}, obsidian);
            }
        }
    }
    level.set_block({5, 60, 4}, block("minecraft:fire"));
    REQUIRE(rules.on_block_changed(level, {5, 60, 4}) == 6);
    CHECK(rules.axis_of(level.block_at({5, 62, 3})) == PortalAxis::Z);
}

TEST_CASE("breaking the frame empties the whole portal", "[nether][portal]") {
    const PortalRules rules{blocks()};
    MapLevel          level;
    build_frame(level, {0, 40, 0}, 3, 4, true);
    level.set_block({0, 40, 0}, block("minecraft:fire"));
    REQUIRE(rules.on_block_changed(level, {0, 40, 0}) == 12);

    // A block against the portal's face is not in its plane: nothing happens.
    level.set_block({1, 41, 1}, block("minecraft:stone"));
    CHECK(rules.on_block_changed(level, {1, 41, 1}) == 0);

    // A corner is not part of the frame either.
    level.set_block({-1, 39, 0}, registry::kAirState);
    CHECK(rules.on_block_changed(level, {-1, 39, 0}) == 0);

    // A side block is.
    level.set_block({3, 42, 0}, registry::kAirState);
    CHECK(rules.on_block_changed(level, {3, 42, 0}) == 12);
    CHECK(level.name_at({0, 40, 0}) == "minecraft:air");
    CHECK(level.name_at({2, 43, 0}) == "minecraft:air");
}

TEST_CASE("the scale is 1:8 and floors", "[nether][portal]") {
    CHECK(scaled_target({800.5, 70.0, -801.2}, 1.0 / 8.0) == BlockPos{100, 70, -101});
    CHECK(scaled_target({-1.0, 64.9, 1.0}, 1.0 / 8.0) == BlockPos{-1, 64, 0});
    CHECK(scaled_target({12.5, 64.0, -3.5}, 8.0) == BlockPos{100, 64, -28});
    // The border clamp.
    CHECK(scaled_target({4'000'000.0, 64.0, 0.0}, 8.0).x == 29'999'983);
}

TEST_CASE("the closest portal block wins, and the lower one on a tie", "[nether][portal]") {
    const std::array<BlockPos, 4> known{BlockPos{10, 70, 0}, BlockPos{0, 90, 0},
                                        BlockPos{0, 50, 0}, BlockPos{200, 70, 0}};
    CHECK(closest_portal(known, {0, 70, 0}, 128) == BlockPos{10, 70, 0});
    CHECK(closest_portal(known, {0, 70, 0}, 5) == BlockPos{0, 50, 0});
    CHECK_FALSE(closest_portal(known, {1000, 70, 0}, 128).has_value());
}

TEST_CASE("a relative position survives a crossing", "[nether][portal]") {
    const PortalRect from{{0, 64, 0}, PortalAxis::X, 4, 5};
    const PortalRect to{{100, 30, 50}, PortalAxis::X, 2, 3};
    // At the middle of the source, on its floor.
    const Vec3d rel = relative_position(from, {2.0, 64.0, 0.5}, 0.6, 1.8);
    CHECK(rel.x == Approx(0.5));
    CHECK(rel.y == Approx(0.0));
    CHECK(rel.z == Approx(0.0));
    f32         yaw = 10.0F;
    const Vec3d out = exit_position(to, PortalAxis::X, rel, 0.6, 1.8, yaw);
    CHECK(out.x == Approx(101.0));
    CHECK(out.y == Approx(30.0));
    CHECK(out.z == Approx(50.5));
    CHECK(yaw == 10.0F);

    const PortalRect turned{{100, 30, 50}, PortalAxis::Z, 2, 3};
    const Vec3d      other = exit_position(turned, PortalAxis::X, rel, 0.6, 1.8, yaw);
    CHECK(other.x == Approx(100.5));
    CHECK(other.z == Approx(51.0));
    CHECK(yaw == 100.0F);
}

TEST_CASE("a new portal stands on solid ground with room above", "[nether][portal]") {
    const PortalRules rules{blocks()};
    MapLevel          level;
    const auto        netherrack = block("minecraft:netherrack");
    // A floor at y = 40 around (0, 0), and nothing else.
    for (i32 x = -20; x <= 20; ++x) {
        for (i32 z = -20; z <= 20; ++z) {
            level.set_block({x, 40, z}, netherrack);
        }
    }
    const PortalRect made = rules.create(level, {3, 60, -2}, PortalAxis::X, 127);
    CHECK(made.width == 2);
    CHECK(made.height == 3);
    CHECK(made.min_corner.y == 41);
    CHECK(level.name_at(made.min_corner) == "minecraft:nether_portal");
    CHECK(level.name_at({made.min_corner.x - 1, 41, made.min_corner.z}) == "minecraft:obsidian");
    CHECK(level.name_at({made.min_corner.x + 2, 44, made.min_corner.z}) == "minecraft:obsidian");

    SECTION("with no floor anywhere, it is forced, on a platform") {
        MapLevel         empty;
        const PortalRect forced = rules.create(empty, {5, 20, 5}, PortalAxis::Z, 127);
        CHECK(forced.min_corner == BlockPos{5, 70, 5});
        CHECK(empty.name_at({4, 69, 5}) == "minecraft:obsidian");
        CHECK(empty.name_at({6, 69, 6}) == "minecraft:obsidian");
    }
}
