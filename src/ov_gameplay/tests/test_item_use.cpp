// Right-clicking, against a real 1.20.1 server's own Block Update packets.
//
// Every expectation below is a state id the server sent after a bot clicked
// once. Not a guess checked against a list of candidates — the packet carries
// the state, so the answer is the game's.
#include "ov/gameplay/item_use.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <utility>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
            "registry.ovpack";
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

[[nodiscard]] const registry::BlockRegistry& blocks() { return *packs().blocks; }

[[nodiscard]] registry::BlockStateId default_state(std::string_view name) {
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

/// A world that is a hash map. Enough to run every rule in item_use.cpp, which
/// is the whole point of taking a LevelWriter: no server anywhere near it.
class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                 is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape    shape() const override { return {}; }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
    }
    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue,
                       world::TickPriority) override {
        scheduled_.emplace_back(key(pos), std::string{what}, delay);
    }
    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue) const override {
        for (const auto& [where, name, _] : scheduled_) {
            if (where == key(pos) && name == what) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

    [[nodiscard]] std::string_view name_at(BlockPos pos) const {
        return ::blocks().block_name(::blocks().block_of(block_at(pos)));
    }
    [[nodiscard]] std::string_view property_at(BlockPos pos, std::string_view property) const {
        const auto state = block_at(pos);
        const auto found = ::blocks().find_property(::blocks().block_of(state), property);
        return found ? ::blocks().property_value(state, *found) : std::string_view{};
    }
    [[nodiscard]] usize scheduled() const { return scheduled_.size(); }

private:
    using Key = std::tuple<i32, i32, i32>;
    [[nodiscard]] static Key key(BlockPos pos) { return {pos.x, pos.y, pos.z}; }
    std::map<Key, registry::BlockStateId>      cells_;
    std::vector<std::tuple<Key, std::string, i64>> scheduled_;
};

[[nodiscard]] UseContext at(BlockPos pos, std::string_view item = {}) {
    UseContext context;
    context.position = pos;
    context.face     = 1;
    context.item     = item;
    return context;
}

constexpr BlockPos kHere{1, -60, 0};
constexpr BlockPos kAbove{1, -59, 0};

}  // namespace

TEST_CASE("a wooden door opens and an iron one does not", "[gameplay][item_use]") {
    if (!packs().blocks) {
        WARN("registry.ovpack is missing");
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel level;
    level.set_block(kHere, default_state("minecraft:oak_door"));
    // The upper half, so the two-block rule has something to move.
    //
    // Every property is named, not just `half`. A partial set resolves — it does
    // not fail — and the state it resolves to carries whatever the other
    // properties happened to land on: asking for `{half: upper}` alone gave a
    // door that was already open, and the test then read a door being closed as
    // a door that had not moved.
    const std::pair<std::string_view, std::string_view> upper_props[] = {
        {"facing", "north"}, {"half", "upper"}, {"hinge", "left"},
        {"open", "false"},   {"powered", "false"}};
    level.set_block(kAbove, state_of("minecraft:oak_door", upper_props));

    const UseOutcome out = rules.use_on(level, at(kHere));
    REQUIRE(out.result == UseResult::Success);
    REQUIRE(level.property_at(kHere, "open") == "true");
    // Both halves, or the door is drawn half open and nothing can walk through.
    REQUIRE(level.property_at(kAbove, "open") == "true");

    MapLevel iron;
    iron.set_block(kHere, default_state("minecraft:iron_door"));
    const UseOutcome refused = iron.block_at(kHere) == registry::kAirState
                                   ? UseOutcome{}
                                   : rules.use_on(iron, at(kHere));
    // Measured: an iron door clicked bare-handed came back at open=false.
    REQUIRE(refused.result == UseResult::Fail);
    REQUIRE(iron.property_at(kHere, "open") == "false");
}

TEST_CASE("a trapdoor and a gate open, an iron trapdoor does not",
          "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    for (const char* name : {"minecraft:oak_trapdoor", "minecraft:oak_fence_gate"}) {
        MapLevel level;
        level.set_block(kHere, default_state(name));
        REQUIRE(rules.use_on(level, at(kHere)).result == UseResult::Success);
        INFO(name);
        REQUIRE(level.property_at(kHere, "open") == "true");
    }

    MapLevel iron;
    iron.set_block(kHere, default_state("minecraft:iron_trapdoor"));
    REQUIRE(rules.use_on(iron, at(kHere)).result == UseResult::Fail);
    REQUIRE(iron.property_at(kHere, "open") == "false");
}

TEST_CASE("switches switch and buttons schedule their own release",
          "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel lever;
    lever.set_block(kHere, default_state("minecraft:lever"));
    REQUIRE(rules.use_on(lever, at(kHere)).result == UseResult::Success);
    REQUIRE(lever.property_at(kHere, "powered") == "true");
    // A lever stays where it is put; nothing is scheduled.
    REQUIRE(lever.scheduled() == 0);

    for (const char* name : {"minecraft:stone_button", "minecraft:oak_button"}) {
        MapLevel level;
        level.set_block(kHere, default_state(name));
        REQUIRE(rules.use_on(level, at(kHere)).result == UseResult::Success);
        INFO(name);
        REQUIRE(level.property_at(kHere, "powered") == "true");
        // A button comes back up on its own, so it must ask to be woken.
        REQUIRE(level.has_scheduled_tick(kHere, name, world::TickQueue::Block));
    }
}

TEST_CASE("the cycling blocks cycle the way the server showed",
          "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel notes;
    notes.set_block(kHere, default_state("minecraft:note_block"));
    REQUIRE(rules.use_on(notes, at(kHere)).result == UseResult::Success);
    REQUIRE(notes.property_at(kHere, "note") == "1");

    MapLevel repeater;
    repeater.set_block(kHere, default_state("minecraft:repeater"));
    // Measured: delay 1 became delay 2.
    REQUIRE(rules.use_on(repeater, at(kHere)).result == UseResult::Success);
    REQUIRE(repeater.property_at(kHere, "delay") == "2");
    // And it wraps from four back to one rather than to five.
    for (int i = 0; i < 3; ++i) {
        (void)rules.use_on(repeater, at(kHere));
    }
    REQUIRE(repeater.property_at(kHere, "delay") == "1");

    MapLevel comparator;
    comparator.set_block(kHere, default_state("minecraft:comparator"));
    REQUIRE(rules.use_on(comparator, at(kHere)).result == UseResult::Success);
    REQUIRE(comparator.property_at(kHere, "mode") == "subtract");

    MapLevel detector;
    detector.set_block(kHere, default_state("minecraft:daylight_detector"));
    REQUIRE(rules.use_on(detector, at(kHere)).result == UseResult::Success);
    REQUIRE(detector.property_at(kHere, "inverted") == "true");
}

TEST_CASE("a hoe tills three blocks and roughs up two", "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    // Measured, one click each. Dirt, grass and a path become farmland; coarse
    // dirt and rooted dirt become plain dirt instead. A table that sends all
    // five to farmland is wrong on two of them.
    const std::pair<const char*, const char*> cases[] = {
        {"minecraft:dirt", "minecraft:farmland"},
        {"minecraft:grass_block", "minecraft:farmland"},
        {"minecraft:dirt_path", "minecraft:farmland"},
        {"minecraft:coarse_dirt", "minecraft:dirt"},
        {"minecraft:rooted_dirt", "minecraft:dirt"},
    };
    for (const auto& [from, to] : cases) {
        MapLevel level;
        level.set_block(kHere, default_state(from));
        const UseOutcome out = rules.use_on(level, at(kHere, "minecraft:diamond_hoe"));
        INFO(from);
        REQUIRE(out.result == UseResult::Success);
        REQUIRE(out.item_damage == 1);
        REQUIRE(level.name_at(kHere) == to);
    }

    // A block with something on top of it is not tilled.
    MapLevel blocked;
    blocked.set_block(kHere, default_state("minecraft:dirt"));
    blocked.set_block(kAbove, default_state("minecraft:stone"));
    REQUIRE(rules.use_on(blocked, at(kHere, "minecraft:diamond_hoe")).result == UseResult::Pass);
    REQUIRE(blocked.name_at(kHere) == "minecraft:dirt");
}

TEST_CASE("a shovel makes a path and puts a campfire out", "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel level;
    level.set_block(kHere, default_state("minecraft:grass_block"));
    REQUIRE(rules.use_on(level, at(kHere, "minecraft:diamond_shovel")).result ==
            UseResult::Success);
    REQUIRE(level.name_at(kHere) == "minecraft:dirt_path");

    MapLevel fire;
    fire.set_block(kHere, default_state("minecraft:campfire"));
    REQUIRE(rules.use_on(fire, at(kHere, "minecraft:diamond_shovel")).result ==
            UseResult::Success);
    REQUIRE(fire.property_at(kHere, "lit") == "false");
}

TEST_CASE("an axe strips, scrapes and dewaxes", "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel log;
    log.set_block(kHere, default_state("minecraft:oak_log"));
    REQUIRE(rules.use_on(log, at(kHere, "minecraft:diamond_axe")).result == UseResult::Success);
    REQUIRE(log.name_at(kHere) == "minecraft:stripped_oak_log");
    // The axis survives: measured as stripped_oak_log[axis=y].
    REQUIRE(log.property_at(kHere, "axis") == "y");
    // A log already stripped is not stripped twice.
    REQUIRE(rules.use_on(log, at(kHere, "minecraft:diamond_axe")).result == UseResult::Pass);

    MapLevel copper;
    copper.set_block(kHere, default_state("minecraft:oxidized_copper"));
    REQUIRE(rules.use_on(copper, at(kHere, "minecraft:diamond_axe")).result ==
            UseResult::Success);
    REQUIRE(copper.name_at(kHere) == "minecraft:weathered_copper");

    MapLevel waxed;
    waxed.set_block(kHere, default_state("minecraft:waxed_copper_block"));
    REQUIRE(rules.use_on(waxed, at(kHere, "minecraft:diamond_axe")).result ==
            UseResult::Success);
    REQUIRE(waxed.name_at(kHere) == "minecraft:copper_block");
}

TEST_CASE("flint and steel lights a fire and primes a tnt", "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel level;
    level.set_block(kHere, default_state("minecraft:stone"));
    const UseOutcome lit = rules.use_on(level, at(kHere, "minecraft:flint_and_steel"));
    REQUIRE(lit.result == UseResult::Success);
    REQUIRE(lit.item_damage == 1);
    // Measured: fire[age=0], one block along the clicked face.
    REQUIRE(level.name_at(kAbove) == "minecraft:fire");
    REQUIRE(level.property_at(kAbove, "age") == "0");

    MapLevel tnt;
    tnt.set_block(kHere, default_state("minecraft:tnt"));
    const UseOutcome primed = rules.use_on(tnt, at(kHere, "minecraft:flint_and_steel"));
    REQUIRE(primed.result == UseResult::Success);
    // Measured: the block became air and an entity appeared. Spawning it is the
    // caller's, which is why the outcome names it instead of doing it.
    REQUIRE(tnt.block_at(kHere) == registry::kAirState);
    REQUIRE(primed.spawn_primed_tnt);
    REQUIRE(primed.tnt_position == kHere);

    // A bare hand does not prime a tnt. Measured: it came back unchanged.
    MapLevel bare;
    bare.set_block(kHere, default_state("minecraft:tnt"));
    REQUIRE_FALSE(rules.use_on(bare, at(kHere)).spawn_primed_tnt);
    REQUIRE(bare.name_at(kHere) == "minecraft:tnt");
}

TEST_CASE("a candle goes out to a bare hand and comes back to a flint",
          "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    const std::pair<std::string_view, std::string_view> lit_pair[] = {{"lit", "true"}};
    MapLevel                                            level;
    level.set_block(kHere, state_of("minecraft:candle", lit_pair));
    REQUIRE(rules.use_on(level, at(kHere)).result == UseResult::Success);
    REQUIRE(level.property_at(kHere, "lit") == "false");

    REQUIRE(rules.use_on(level, at(kHere, "minecraft:flint_and_steel")).result ==
            UseResult::Success);
    REQUIRE(level.property_at(kHere, "lit") == "true");
}

TEST_CASE("a chest names its screen rather than opening one",
          "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel level;
    level.set_block(kHere, default_state("minecraft:chest"));
    const UseOutcome out = rules.use_on(level, at(kHere, "minecraft:diamond"));
    REQUIRE(out.result == UseResult::Success);
    REQUIRE(out.screen == ScreenKind::Chest);
    REQUIRE(out.screen_position == kHere);

    // Sneaking with something in hand puts the item first, so the chest never
    // gets a look in. Without this rule no container in the game can be opened
    // while holding anything.
    UseContext sneaking = at(kHere, "minecraft:diamond");
    sneaking.sneaking   = true;
    REQUIRE(rules.use_on(level, sneaking).screen == ScreenKind::None);

    // Sneaking with an *empty* hand still opens it.
    UseContext empty_handed = at(kHere);
    empty_handed.sneaking   = true;
    REQUIRE(rules.use_on(level, empty_handed).screen == ScreenKind::Chest);

    MapLevel furnace;
    furnace.set_block(kHere, default_state("minecraft:blast_furnace"));
    REQUIRE(rules.use_on(furnace, at(kHere)).screen == ScreenKind::BlastFurnace);
}

TEST_CASE("cake loses a bite at a time and then vanishes", "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    MapLevel level;
    level.set_block(kHere, default_state("minecraft:cake"));
    REQUIRE(rules.use_on(level, at(kHere)).result == UseResult::Consume);
    REQUIRE(level.property_at(kHere, "bites") == "1");
    for (int i = 0; i < 5; ++i) {
        (void)rules.use_on(level, at(kHere));
    }
    REQUIRE(level.property_at(kHere, "bites") == "6");
    (void)rules.use_on(level, at(kHere));
    REQUIRE(level.block_at(kHere) == registry::kAirState);
}

TEST_CASE("what is not finished says so", "[gameplay][item_use]") {
    if (!packs().blocks) {
        return;
    }
    const ItemUse rules{blocks(), *packs().registries};

    // A bucket does not act through Use Item On at all — it is Item.use, with a
    // server-side ray trace that can see a fluid the block ray went through.
    // Named rather than quietly doing nothing.
    MapLevel level;
    level.set_block(kHere, default_state("minecraft:water"));
    const UseOutcome bucket = rules.use_on(level, at(kHere, "minecraft:bucket"));
    REQUIRE(bucket.result == UseResult::Pass);
    REQUIRE_FALSE(bucket.unsupported.empty());

    MapLevel egg;
    egg.set_block(kHere, default_state("minecraft:dragon_egg"));
    REQUIRE_FALSE(rules.use_on(egg, at(kHere)).unsupported.empty());
}

TEST_CASE("eating takes thirty-two ticks and can be given up",
          "[gameplay][item_use]") {
    REQUIRE(use_duration_ticks("minecraft:bread") == 32);
    REQUIRE(use_duration_ticks("minecraft:dried_kelp") == 16);
    REQUIRE(use_duration_ticks("minecraft:honey_bottle") == 40);
    REQUIRE(use_duration_ticks("minecraft:cobblestone") == 0);

    UseInProgress use;
    REQUIRE(begin_use(use, "minecraft:bread", 10, 20));
    REQUIRE(use.active());
    for (int i = 0; i < 31; ++i) {
        REQUIRE_FALSE(tick_use(use));
    }
    REQUIRE(tick_use(use));
    REQUIRE_FALSE(use.active());

    // A full player cannot eat bread, and can always eat a golden apple.
    UseInProgress full;
    REQUIRE_FALSE(begin_use(full, "minecraft:bread", 20, 20));
    REQUIRE(begin_use(full, "minecraft:golden_apple", 20, 20));

    // Letting go at tick 31 has eaten nothing.
    UseInProgress abandoned;
    REQUIRE(begin_use(abandoned, "minecraft:bread", 10, 20));
    for (int i = 0; i < 31; ++i) {
        (void)tick_use(abandoned);
    }
    cancel_use(abandoned);
    REQUIRE_FALSE(abandoned.active());
    REQUIRE_FALSE(tick_use(abandoned));
}
