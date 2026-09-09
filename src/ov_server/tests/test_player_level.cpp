// The writer a player's right-click goes through.
//
// `ItemUse` is measured and tested in ov_gameplay against a real server's Block
// Update packets. What is *not* tested there is the thing that kept a lever
// from moving for two waves: nothing in ov_server handed it a `LevelWriter` at
// all. So what is pinned here is the adapter and the route — a click on a lever
// reaches `set_block`, a click on a button reaches `schedule_tick`, and a door
// moves both of its halves — over a map made of literals rather than chunks.
#include "../src/player_level.hpp"

#include "ov/gameplay/item_use.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <tuple>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Packs out;
        auto  blocks = registry::BlockRegistry::load(path);
        auto  regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
        }
        return out;
    }();
    return state;
}

/// A world of literals, and a log of everything the adapter asked it to do.
///
/// The point of the log is that a rule which "worked" by writing nothing is a
/// rule that did not work: both halves of a door have to appear in it, and a
/// button has to appear in the scheduled ticks as well as in the writes.
struct FakeWorld {
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> blocks;
    std::vector<std::pair<BlockPos, registry::BlockStateId>>    writes;
    std::vector<std::tuple<BlockPos, std::string, i64>>         scheduled;
    i64                                                         now{100};

    [[nodiscard]] PlayerLevelHooks hooks(const registry::BlockRegistry& registry) {
        PlayerLevelHooks out;
        out.blocks   = &registry;
        out.block_at = [this](BlockPos pos) {
            const auto found = blocks.find({pos.x, pos.y, pos.z});
            return found == blocks.end() ? registry::kAirState : found->second;
        };
        out.is_loaded = [](BlockPos) { return true; };
        out.set_block = [this](BlockPos pos, registry::BlockStateId state) {
            blocks[{pos.x, pos.y, pos.z}] = state;
            writes.emplace_back(pos, state);
        };
        out.schedule_tick = [this](BlockPos pos, std::string_view what, i64 delay,
                                   world::TickQueue, world::TickPriority) {
            scheduled.emplace_back(pos, std::string{what}, delay);
        };
        out.has_scheduled_tick = [](BlockPos, std::string_view, world::TickQueue) { return false; };
        out.game_time          = [this] { return now; };
        return out;
    }
};

[[nodiscard]] registry::BlockStateId state_of(std::string_view                    name,
                                              const std::map<std::string, std::string>& properties) {
    const registry::BlockRegistry& registry = *packs().blocks;
    const auto                     block    = registry.find_block(name);
    REQUIRE(block.has_value());
    registry::BlockStateId state = registry.default_state(*block);
    for (const auto& [key, value] : properties) {
        const auto property = registry.find_property(*block, key);
        REQUIRE(property.has_value());
        const auto found = std::ranges::find(property->values, value);
        REQUIRE(found != property->values.end());
        state = registry.with_property(
            state, *property,
            static_cast<u16>(std::distance(property->values.begin(), found)));
    }
    return state;
}

[[nodiscard]] std::string value_of(registry::BlockStateId state, std::string_view property) {
    const registry::BlockRegistry& registry = *packs().blocks;
    const auto found = registry.find_property(registry.block_of(state), property);
    REQUIRE(found.has_value());
    return std::string{registry.property_value(state, *found)};
}

[[nodiscard]] gameplay::UseContext at(i32 x, i32 y, i32 z) {
    gameplay::UseContext context;
    context.position = BlockPos{x, y, z};
    context.face     = 1;
    return context;
}

}  // namespace

TEST_CASE("a lever reaches set_block through the player's writer", "[interaction]") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    FakeWorld       world;
    const BlockPos  lever{1, -59, 12};
    world.blocks[{lever.x, lever.y, lever.z}] =
        state_of("minecraft:lever", {{"face", "floor"}, {"facing", "north"}, {"powered", "false"}});

    PlayerLevel            level{world.hooks(*packs().blocks)};
    const gameplay::ItemUse rules{*packs().blocks, *packs().registries};

    const gameplay::UseOutcome out = rules.use_on(level, at(lever.x, lever.y, lever.z));
    REQUIRE(out.result == gameplay::UseResult::Success);
    REQUIRE(world.writes.size() == 1);
    REQUIRE(level.writes() == 1);
    CHECK(value_of(world.writes[0].second, "powered") == "true");

    // And back. A lever that only ever switches on is a lever nobody can turn
    // off, and the measurement asks for both halves.
    world.writes.clear();
    level.clear_writes();
    REQUIRE(rules.use_on(level, at(lever.x, lever.y, lever.z)).result ==
            gameplay::UseResult::Success);
    CHECK(value_of(world.writes[0].second, "powered") == "false");
}

TEST_CASE("a button schedules its own release", "[interaction]") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    FakeWorld      world;
    const BlockPos button{4, -59, 4};
    world.blocks[{button.x, button.y, button.z}] = state_of(
        "minecraft:oak_button", {{"face", "floor"}, {"facing", "north"}, {"powered", "false"}});

    PlayerLevel             level{world.hooks(*packs().blocks)};
    const gameplay::ItemUse rules{*packs().blocks, *packs().registries};

    REQUIRE(rules.use_on(level, at(button.x, button.y, button.z)).result ==
            gameplay::UseResult::Success);
    REQUIRE(world.writes.size() == 1);
    CHECK(value_of(world.writes[0].second, "powered") == "true");

    // The half that only the adapter can carry: without a `schedule_tick` that
    // reaches the tick thread's real queue, the button stays down for ever.
    REQUIRE(world.scheduled.size() == 1);
    CHECK(std::get<1>(world.scheduled[0]) == "minecraft:oak_button");
    CHECK(std::get<2>(world.scheduled[0]) == 30);
}

TEST_CASE("a door opens both of its halves", "[interaction]") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    FakeWorld      world;
    const BlockPos lower{99, -60, 133};
    world.blocks[{lower.x, lower.y, lower.z}] =
        state_of("minecraft:oak_door", {{"facing", "north"},
                                        {"half", "lower"},
                                        {"hinge", "left"},
                                        {"open", "false"},
                                        {"powered", "false"}});
    world.blocks[{lower.x, lower.y + 1, lower.z}] =
        state_of("minecraft:oak_door", {{"facing", "north"},
                                        {"half", "upper"},
                                        {"hinge", "left"},
                                        {"open", "false"},
                                        {"powered", "false"}});

    PlayerLevel             level{world.hooks(*packs().blocks)};
    const gameplay::ItemUse rules{*packs().blocks, *packs().registries};

    REQUIRE(rules.use_on(level, at(lower.x, lower.y, lower.z)).result ==
            gameplay::UseResult::Success);
    REQUIRE(world.writes.size() == 2);
    CHECK(value_of(world.writes[0].second, "open") == "true");
    CHECK(value_of(world.writes[1].second, "open") == "true");
    CHECK(world.writes[1].first.y == lower.y + 1);
}

TEST_CASE("an iron door refuses a bare hand rather than passing", "[interaction]") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    FakeWorld      world;
    const BlockPos lower{0, -60, 0};
    world.blocks[{lower.x, lower.y, lower.z}] =
        state_of("minecraft:iron_door", {{"facing", "north"},
                                         {"half", "lower"},
                                         {"hinge", "left"},
                                         {"open", "false"},
                                         {"powered", "false"}});

    PlayerLevel             level{world.hooks(*packs().blocks)};
    const gameplay::ItemUse rules{*packs().blocks, *packs().registries};

    // Fail, not Pass: a Pass would let the held item place *through* the door,
    // which is the difference the four-valued result exists for.
    CHECK(rules.use_on(level, at(lower.x, lower.y, lower.z)).result == gameplay::UseResult::Fail);
    CHECK(world.writes.empty());
}

TEST_CASE("a write below the world floor is refused, not clamped", "[interaction]") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    FakeWorld   world;
    PlayerLevel level{world.hooks(*packs().blocks)};

    level.set_block(BlockPos{0, -5000, 0}, registry::kAirState);
    CHECK(world.writes.empty());
    CHECK(level.writes() == 0);
    CHECK(level.block_at(BlockPos{0, -5000, 0}) == registry::kAirState);
}
