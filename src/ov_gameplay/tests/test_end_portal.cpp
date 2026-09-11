// End portals: the ring of twelve frames, the eyes, the portal, the platform.
//
// The rules are the Minecraft Wiki's (End portal, Java 1.20); the arrival and
// the platform are measured against the real server by
// scripts/measure_end_portal.py, and the figures are in docs/provenance/end.md.
#include "ov/gameplay/end_portal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <string_view>
#include <tuple>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] const registry::BlockRegistry& blocks() {
    static const auto pack = registry::BlockRegistry::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    REQUIRE(pack.has_value());
    return *pack;
}

class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }
    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
        ++writes_;
    }
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
    [[nodiscard]] usize writes() const noexcept { return writes_; }

private:
    using Key = std::tuple<i32, i32, i32>;
    [[nodiscard]] static Key key(BlockPos pos) { return {pos.x, pos.y, pos.z}; }
    std::map<Key, registry::BlockStateId> cells_;
    usize                                 writes_{0};
};

struct Slot {
    BlockPos         pos;
    std::string_view facing;
};

/// The twelve frames round `centre`, each facing the inside.
[[nodiscard]] std::array<Slot, 12> ring(BlockPos centre) {
    std::array<Slot, 12> slots{};
    usize                n = 0;
    for (i32 k = -1; k <= 1; ++k) {
        slots[n++] = {{centre.x + k, centre.y, centre.z - 2}, "south"};
        slots[n++] = {{centre.x + k, centre.y, centre.z + 2}, "north"};
        slots[n++] = {{centre.x - 2, centre.y, centre.z + k}, "east"};
        slots[n++] = {{centre.x + 2, centre.y, centre.z + k}, "west"};
    }
    return slots;
}

}  // namespace

TEST_CASE("twelve eyes open the portal, eleven do not", "[gameplay][end]") {
    const EndPortalRules rules{blocks()};
    REQUIRE(rules.valid());
    MapLevel       level;
    const BlockPos centre{10, 40, -7};
    const auto     slots = ring(centre);
    for (const Slot& slot : slots) {
        level.set_block(slot.pos, rules.frame_state(slot.facing, false));
    }

    for (usize i = 0; i + 1 < slots.size(); ++i) {
        const auto outcome = rules.use_eye(level, slots[i].pos);
        CHECK(outcome.result == EyeUse::Inserted);
        CHECK(rules.has_eye(level.block_at(slots[i].pos)));
    }
    CHECK(level.name_at(centre) == "minecraft:air");
    // A second eye into a full frame is not used.
    CHECK(rules.use_eye(level, slots[0].pos).result == EyeUse::Pass);
    // Nor is an eye on something that is not a frame.
    CHECK(rules.use_eye(level, centre).result == EyeUse::Pass);

    const auto last = rules.use_eye(level, slots[11].pos);
    CHECK(last.result == EyeUse::Activated);
    CHECK(last.portal_centre == centre);
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            CHECK(level.name_at({centre.x + dx, centre.y, centre.z + dz}) == "minecraft:end_portal");
        }
    }
    // Nothing above or below it, and the corners are untouched.
    CHECK(level.name_at({centre.x, centre.y + 1, centre.z}) == "minecraft:air");
    CHECK(level.name_at({centre.x - 2, centre.y, centre.z - 2}) == "minecraft:air");
}

TEST_CASE("a ring whose frames face outwards is no portal", "[gameplay][end]") {
    const EndPortalRules rules{blocks()};
    MapLevel             level;
    const BlockPos       centre{0, 64, 0};
    for (const Slot& slot : ring(centre)) {
        // The opposite of the inside.
        const std::string_view out = slot.facing == "south"   ? "north"
                                     : slot.facing == "north" ? "south"
                                     : slot.facing == "east"  ? "west"
                                                              : "east";
        level.set_block(slot.pos, rules.frame_state(out, true));
    }
    for (const Slot& slot : ring(centre)) {
        CHECK_FALSE(rules.complete_ring(level, slot.pos).has_value());
    }
    CHECK(level.name_at(centre) == "minecraft:air");
}

TEST_CASE("the ring can be completed from any of its twelve frames", "[gameplay][end]") {
    const EndPortalRules rules{blocks()};
    const BlockPos       centre{-3, 12, 5};
    for (usize last = 0; last < 12; ++last) {
        MapLevel   level;
        const auto slots = ring(centre);
        for (usize i = 0; i < slots.size(); ++i) {
            level.set_block(slots[i].pos, rules.frame_state(slots[i].facing, i != last));
        }
        const auto outcome = rules.use_eye(level, slots[last].pos);
        CHECK(outcome.result == EyeUse::Activated);
        CHECK(outcome.portal_centre == centre);
    }
}

TEST_CASE("the portal takes a box that meets its slab", "[gameplay][end]") {
    const EndPortalRules rules{blocks()};
    MapLevel             level;
    level.set_block({0, 60, 0}, rules.portal_state());
    const auto box_at = [](f64 y) {
        return AABB{Vec3d{0.2, y, 0.2}, Vec3d{0.8, y + 1.8, 0.8}};
    };
    CHECK(rules.box_in_portal(level, box_at(60.0)));    // feet in the block
    CHECK(rules.box_in_portal(level, box_at(60.7)));    // feet in the slab
    CHECK_FALSE(rules.box_in_portal(level, box_at(60.75)));  // on the slab's top face
    CHECK_FALSE(rules.box_in_portal(level, box_at(61.0)));   // standing on the block
    CHECK_FALSE(rules.box_in_portal(level, box_at(58.0)));   // head below it: 59.8 < 60.375
}

TEST_CASE("the exit portal: a bedrock bowl, a pillar, four torches", "[gameplay][end]") {
    const EndPortalRules rules{blocks()};
    REQUIRE(rules.valid());
    MapLevel       level;
    const BlockPos origin{0, 62, 0};
    rules.build_exit_portal(level, origin, false);
    // The top ring: bedrock where 2.5 <= distance < 3.5, air inside, the
    // pillar in the middle.
    CHECK(level.name_at({3, 62, 0}) == "minecraft:bedrock");
    CHECK(level.name_at({2, 62, 2}) == "minecraft:bedrock");   // 8 >= 6.25
    CHECK(level.name_at({2, 62, 1}) == "minecraft:air");       // 5 < 6.25: inside
    CHECK(level.name_at({3, 62, 2}) == "minecraft:air");       // 13 >= 12.25: outside, untouched
    CHECK(level.name_at({0, 62, 0}) == "minecraft:bedrock");   // the pillar
    CHECK(level.name_at({0, 65, 0}) == "minecraft:bedrock");
    CHECK(level.name_at({0, 66, 0}) == "minecraft:air");
    // Under the ring: bedrock inside, end stone under the rim.
    CHECK(level.name_at({1, 61, 1}) == "minecraft:bedrock");
    CHECK(level.name_at({2, 61, 1}) == "minecraft:bedrock");    // 4 + 1 + 1 = 6 < 6.25
    CHECK(level.name_at({3, 61, 0}) == "minecraft:end_stone");  // 9 + 1 = 10 < 12.25: the rim
    CHECK(level.name_at({0, 64, -1}) == "minecraft:wall_torch");
    CHECK(level.name_at({1, 64, 0}) == "minecraft:wall_torch");

    rules.build_exit_portal(level, origin, true);
    CHECK(level.name_at({2, 62, 1}) == "minecraft:end_portal");
    CHECK(level.name_at({1, 62, 0}) == "minecraft:end_portal");
    CHECK(level.name_at({0, 62, 0}) == "minecraft:bedrock");
}

TEST_CASE("the twenty gateways are on a circle of radius 96", "[gameplay][end]") {
    const auto gateways = end_gateway_order(1234567890);
    std::set<std::pair<i32, i32>> distinct;
    for (const BlockPos& g : gateways) {
        CHECK(g.y == 75);
        const f64 r = std::sqrt(static_cast<f64>(g.x) * g.x + static_cast<f64>(g.z) * g.z);
        CHECK(r > 94.0);
        CHECK(r < 97.5);
        distinct.insert({g.x, g.z});
    }
    CHECK(distinct.size() == 20);
    CHECK(end_gateway_order(1234567890)[0] == gateways[0]);
}

TEST_CASE("the arrival platform is five by five, obsidian under three of air", "[gameplay][end]") {
    const EndPortalRules rules{blocks()};
    MapLevel             level;
    const auto           stone = blocks().default_state(*blocks().find_block("minecraft:end_stone"));
    level.set_block({100, 51, 0}, stone);
    level.set_block({103, 49, 0}, stone);
    rules.build_platform(level);
    CHECK(level.writes() == 2 + 5 * 5 * 4);
    for (i32 dx = -2; dx <= 2; ++dx) {
        for (i32 dz = -2; dz <= 2; ++dz) {
            CHECK(level.name_at({100 + dx, 49, dz}) == "minecraft:obsidian");
            for (i32 y = 50; y <= 52; ++y) {
                CHECK(level.name_at({100 + dx, y, dz}) == "minecraft:air");
            }
        }
    }
    CHECK(level.name_at({103, 49, 0}) == "minecraft:end_stone");  // outside it
    CHECK(level.name_at({100, 53, 0}) == "minecraft:air");
}
