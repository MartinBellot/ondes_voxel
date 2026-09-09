// Redstone against the numbers a real 1.20.1 server produced.
//
// The circuits here are the ones the measurement built: a sixteen-block wire, a
// repeater locked by its neighbour, a comparator in both modes against every
// pair of inputs, a chest filled item by item. The expected values are not
// derived from the same formulas the code uses — they are what the game wrote
// into its own save file. See docs/provenance/redstone.md.
#include "ov/gameplay/redstone.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<Redstone>                redstone;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        auto   blocks = registry::BlockRegistry::load(path);
        auto   regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
            out.redstone.emplace(*out.blocks, *out.registries);
        }
        return out;
    }();
    return state;
}

/// A world made of a map, and a queue of the ticks it was asked for.
class TestWorld final : public RedstoneWorld {
public:
    explicit TestWorld(const registry::BlockRegistry& blocks) : blocks_{&blocks} {}

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto it = cells_.find(key(pos));
        return it == cells_.end() ? registry::kAirState : it->second;
    }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
        ++writes;
    }

    void schedule_tick(BlockPos pos, registry::BlockId block, i32 delay,
                       world::TickPriority priority) override {
        queue.schedule(pos, block, now, delay, priority);
    }

    [[nodiscard]] bool tick_scheduled(BlockPos pos, registry::BlockId block) const override {
        return queue.is_scheduled(pos, block);
    }

    [[nodiscard]] i32 container_signal(BlockPos pos) const override {
        const auto it = containers_.find(key(pos));
        return it == containers_.end() ? -1 : it->second;
    }

    [[nodiscard]] i64 game_time() const override { return now; }

    void put(BlockPos pos, std::string_view name,
             std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
        const auto block = blocks_->find_block(name);
        REQUIRE(block.has_value());
        registry::BlockStateId state = blocks_->default_state(*block);
        for (const auto& [key_name, value] : props) {
            const auto prop = blocks_->find_property(*block, key_name);
            REQUIRE(prop.has_value());
            bool found = false;
            for (u16 i = 0; i < prop->values.size(); ++i) {
                if (prop->values[i] == value) {
                    state = blocks_->with_property(state, *prop, i);
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
        }
        set_block(pos, state);
    }

    void set_container(BlockPos pos, i32 signal) { containers_[key(pos)] = signal; }

    [[nodiscard]] std::string text(BlockPos pos) const {
        const registry::BlockStateId state = block_at(pos);
        return std::string{blocks_->block_name(blocks_->block_of(state))};
    }

    [[nodiscard]] i32 power(BlockPos pos, const Signals& signals) const {
        return signals.power_of(block_at(pos));
    }

    world::BlockTickScheduler queue;
    i64                       now{0};
    usize                     writes{0};

private:
    [[nodiscard]] static i64 key(BlockPos pos) {
        return (static_cast<i64>(pos.x) << 40) ^ (static_cast<i64>(pos.y) << 20) ^
               static_cast<i64>(pos.z);
    }

    const registry::BlockRegistry*             blocks_{nullptr};
    std::map<i64, registry::BlockStateId>      cells_;
    std::map<i64, i32>                         containers_;
};

/// Run every due tick until the circuit stops changing, the way one game tick
/// does — but across as many as the circuit needs, since these tests care about
/// where a circuit settles and not how long it took.
void settle(Redstone& redstone, TestWorld& world, i64 ticks = 64) {
    for (i64 i = 0; i < ticks; ++i) {
        ++world.now;
        std::vector<world::ScheduledTick> due;
        world.queue.drain_due(world.now, due);
        for (const world::ScheduledTick& tick : due) {
            redstone.scheduled_tick(world, tick.pos, tick.block);
        }
    }
}

#define REQUIRE_REGISTRY()                       \
    if (!loaded().redstone.has_value()) {        \
        SKIP("registry.ovpack not built");       \
    }                                            \
    Redstone& redstone = const_cast<Redstone&>(*loaded().redstone);  /* NOLINT */ \
    const Signals& signals = redstone.signals();                                  \
    TestWorld      world{*loaded().blocks};                                       \
    (void)signals

}  // namespace

TEST_CASE("what conducts redstone was measured, not guessed", "[redstone][signal]") {
    REQUIRE_REGISTRY();
    const auto state = [&](std::string_view name) {
        const auto id = loaded().blocks->find_block(name);
        REQUIRE(id.has_value());
        return loaded().blocks->default_state(*id);
    };

    // Every one of these came back out of a real save. The pairs that would be
    // wrong under any "is it solid" or "is it see-through" rule are the point:
    // a redstone lamp and a barrier conduct; glass, leaves and an observer do
    // not; and mud and soul sand conduct without being full cubes at all.
    REQUIRE(signals.is_conductor(state("minecraft:stone")));
    REQUIRE(signals.is_conductor(state("minecraft:redstone_lamp")));
    REQUIRE(signals.is_conductor(state("minecraft:barrier")));
    REQUIRE(signals.is_conductor(state("minecraft:slime_block")));
    REQUIRE(signals.is_conductor(state("minecraft:target")));
    REQUIRE(signals.is_conductor(state("minecraft:note_block")));
    REQUIRE(signals.is_conductor(state("minecraft:snow_block")));
    REQUIRE(signals.is_conductor(state("minecraft:white_shulker_box")));
    REQUIRE(signals.is_conductor(state("minecraft:mud")));
    REQUIRE(signals.is_conductor(state("minecraft:soul_sand")));

    REQUIRE_FALSE(signals.is_conductor(state("minecraft:glass")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:tinted_glass")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:oak_leaves")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:ice")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:glowstone")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:sea_lantern")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:beacon")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:observer")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:honey_block")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:oak_slab")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:dirt_path")));
    REQUIRE_FALSE(signals.is_conductor(state("minecraft:farmland")));
}

TEST_CASE("every block with a power property is accounted for",
          "[redstone][signal]") {
    REQUIRE_REGISTRY();
    // A block carrying `powered` or `power` that this model neither reads as a
    // source nor drives as a consumer is a hole. The list of allowed holes may
    // be non-empty, but only for reasons written down here; one that appears by
    // accident fails this test with its own name in the message.
    static const std::vector<std::string_view> kNotRedstone{
        // Tripwire's `powered` is set by an entity standing on the string, and
        // the signal leaves through the hook rather than through the string
        // itself. It is a source of nothing and a consumer of nothing.
        "minecraft:tripwire",
    };
    std::vector<std::string_view> holes;
    for (const std::string_view name : signals.unhandled_sources()) {
        const auto id = loaded().blocks->find_block(name);
        REQUIRE(id.has_value());
        if (redstone.consumer_rule(*id) == nullptr) {
            holes.push_back(name);
        }
    }
    for (const std::string_view name : holes) {
        INFO("block with a power property and no behaviour: " << name);
        REQUIRE(std::ranges::find(kNotRedstone, name) != kNotRedstone.end());
    }
    REQUIRE(holes.size() == kNotRedstone.size());
}

TEST_CASE("a wire loses one per block, and stops at zero", "[redstone][wire]") {
    REQUIRE_REGISTRY();
    // Measured on a real server: a redstone block at x = 0 and wire from x = 1
    // to x = 19. The game wrote 15, 14, ... 1, 0, and then zeros.
    for (i32 x = -1; x <= 21; ++x) {
        world.put(BlockPos{x, -1, 0}, "minecraft:stone");
    }
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_block");
    for (i32 x = 1; x <= 19; ++x) {
        world.put(BlockPos{x, 0, 0}, "minecraft:redstone_wire");
    }
    redstone.update_wire(world, BlockPos{1, 0, 0});

    for (i32 x = 1; x <= 19; ++x) {
        const i32 expected = x <= 15 ? 16 - x : 0;
        INFO("x = " << x);
        REQUIRE(world.power(BlockPos{x, 0, 0}, signals) == expected);
    }
}

TEST_CASE("wire crosses a wall to a repeater and not to another wire",
          "[redstone][wire][signal]") {
    REQUIRE_REGISTRY();
    for (i32 x = -1; x <= 6; ++x) {
        world.put(BlockPos{x, -1, 0}, "minecraft:stone");
    }
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_block");
    world.put(BlockPos{1, 0, 0}, "minecraft:redstone_wire");
    world.put(BlockPos{2, 0, 0}, "minecraft:stone");
    world.put(BlockPos{3, 0, 0}, "minecraft:redstone_wire");
    redstone.update_wire(world, BlockPos{1, 0, 0});
    redstone.update_wire(world, BlockPos{3, 0, 0});

    REQUIRE(world.power(BlockPos{1, 0, 0}, signals) == 15);
    // The wall is powered — a repeater beyond it would read 15 — but the wire
    // beyond it reads nothing, because every wire is silent while a wire asks.
    REQUIRE(signals.direct_signal_to(world, BlockPos{2, 0, 0}) == 15);
    REQUIRE(world.power(BlockPos{3, 0, 0}, signals) == 0);

    // The same wall, seen by a repeater: 15.
    world.put(BlockPos{3, 0, 0}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    REQUIRE(redstone.diode_input(world, BlockPos{3, 0, 0}, world.block_at(BlockPos{3, 0, 0})) ==
            15);
}

TEST_CASE("a lever powers the block it is stuck to, strongly", "[redstone][signal]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:stone");
    world.put(BlockPos{0, 1, 0}, "minecraft:lever",
              {{"face", "floor"}, {"facing", "north"}, {"powered", "true"}});

    // Strongly downwards only. The query direction points from the powered
    // block back at the source, so the block below asks with Up.
    REQUIRE(signals.strong_signal(world, BlockPos{0, 1, 0}, Direction::Up) == 15);
    REQUIRE(signals.strong_signal(world, BlockPos{0, 1, 0}, Direction::North) == 0);
    // Weakly in every direction.
    REQUIRE(signals.weak_signal(world, BlockPos{0, 1, 0}, Direction::North) == 15);
    // And the stone it stands on becomes a source of its own.
    REQUIRE(signals.direct_signal_to(world, BlockPos{0, 0, 0}) == 15);
    REQUIRE(signals.signal_at(world, BlockPos{0, 0, 0}, Direction::East) == 15);
}

TEST_CASE("a torch does not power the block it stands on", "[redstone][signal]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:stone");
    world.put(BlockPos{0, 1, 0}, "minecraft:redstone_torch", {{"lit", "true"}});

    // The block below asks with Up, and gets nothing — which is the whole
    // reason a torch on a block does not power that block and turn itself off.
    REQUIRE(signals.weak_signal(world, BlockPos{0, 1, 0}, Direction::Up) == 0);
    REQUIRE(signals.weak_signal(world, BlockPos{0, 1, 0}, Direction::North) == 15);
    // Strongly upwards: the block above a torch is a source.
    REQUIRE(signals.strong_signal(world, BlockPos{0, 1, 0}, Direction::Down) == 15);
    REQUIRE(signals.strong_signal(world, BlockPos{0, 1, 0}, Direction::Up) == 0);
}

TEST_CASE("a repeater is locked by a powered repeater at its side",
          "[redstone][repeater]") {
    REQUIRE_REGISTRY();
    // Measured layout: R faces west and is fed from the west; L sits on R's
    // north side pointing south into it, fed by a redstone block.
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_block");
    world.put(BlockPos{1, 0, 0}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    world.put(BlockPos{1, 0, -1}, "minecraft:repeater",
              {{"facing", "north"}, {"delay", "1"}, {"powered", "true"}, {"locked", "false"}});

    REQUIRE(redstone.repeater_locked(world, BlockPos{1, 0, 0}, world.block_at(BlockPos{1, 0, 0})));

    // Unpowered, it locks nothing.
    world.put(BlockPos{1, 0, -1}, "minecraft:repeater",
              {{"facing", "north"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    REQUIRE_FALSE(
        redstone.repeater_locked(world, BlockPos{1, 0, 0}, world.block_at(BlockPos{1, 0, 0})));

    // Neither does a lever, however powered: only a diode locks.
    world.put(BlockPos{1, 0, -1}, "minecraft:lever",
              {{"face", "floor"}, {"facing", "north"}, {"powered", "true"}});
    REQUIRE_FALSE(
        redstone.repeater_locked(world, BlockPos{1, 0, 0}, world.block_at(BlockPos{1, 0, 0})));
}

TEST_CASE("a locked repeater does not even schedule", "[redstone][repeater]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_block");
    world.put(BlockPos{1, 0, 0}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    world.put(BlockPos{1, 0, -1}, "minecraft:repeater",
              {{"facing", "north"}, {"delay", "1"}, {"powered", "true"}, {"locked", "false"}});

    REQUIRE_FALSE(redstone.neighbour_changed(world, BlockPos{1, 0, 0}, BlockPos{0, 0, 0}));
    REQUIRE(world.queue.pending_count() == 0);
    settle(redstone, world);
    REQUIRE_FALSE(signals.flag_of(world.block_at(BlockPos{1, 0, 0}), "powered"));
}

TEST_CASE("a repeater's delay is twice its setting, in ticks", "[redstone][repeater]") {
    REQUIRE_REGISTRY();
    for (i32 d = 1; d <= 4; ++d) {
        TestWorld one{*loaded().blocks};
        one.put(BlockPos{0, 0, 0}, "minecraft:redstone_block");
        one.put(BlockPos{1, 0, 0}, "minecraft:repeater",
                {{"facing", "west"},
                 {"delay", std::to_string(d)},
                 {"powered", "false"},
                 {"locked", "false"}});
        REQUIRE(redstone.neighbour_changed(one, BlockPos{1, 0, 0}, BlockPos{0, 0, 0}));

        std::vector<world::ScheduledTick> pending;
        one.queue.snapshot(pending);
        REQUIRE(pending.size() == 1);
        INFO("delay = " << d);
        REQUIRE(pending[0].trigger_tick == 2 * d);
        // Turning on, fed by something that is not a diode: high, not extreme.
        REQUIRE(pending[0].priority == world::TickPriority::High);
    }
}

TEST_CASE("a diode fed by a diode jumps the queue", "[redstone][repeater]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "true"}, {"locked", "false"}});
    // Both face west: A's input is on its west side and its output leaves to
    // the east, straight into B's input face.
    world.put(BlockPos{1, 0, 0}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    REQUIRE(redstone.neighbour_changed(world, BlockPos{1, 0, 0}, BlockPos{0, 0, 0}));
    std::vector<world::ScheduledTick> pending;
    world.queue.snapshot(pending);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].priority == world::TickPriority::ExtremelyHigh);
}

TEST_CASE("a comparator compares and subtracts", "[redstone][comparator]") {
    REQUIRE_REGISTRY();
    // The table the real server produced, for the pairs it was asked: compare
    // passes the back through unless the side is larger, and subtract takes the
    // side off the back. Equal inputs pass through in compare mode — which is
    // the case a wrong implementation gets backwards.
    struct Row {
        i32         back;
        i32         side;
        const char* mode;
        i32         out;
    };
    static const std::vector<Row> kRows{
        {0, 0, "compare", 0},   {0, 9, "compare", 0},   {9, 0, "compare", 9},
        {9, 9, "compare", 9},   {6, 9, "compare", 0},   {12, 3, "compare", 12},
        {15, 15, "compare", 15}, {0, 0, "subtract", 0}, {9, 0, "subtract", 9},
        {9, 9, "subtract", 0},  {12, 3, "subtract", 9}, {3, 12, "subtract", 0},
        {15, 6, "subtract", 9},
    };

    for (const Row& row : kRows) {
        TestWorld one{*loaded().blocks};
        one.put(BlockPos{0, 0, 0}, "minecraft:comparator",
                {{"facing", "east"}, {"mode", row.mode}, {"powered", "false"}});
        // Back input on the `facing` side, side input to the north.
        one.put(BlockPos{1, 0, 0}, "minecraft:redstone_wire");
        one.set_block(BlockPos{1, 0, 0},
                      signals.with_power(one.block_at(BlockPos{1, 0, 0}), row.back));
        one.put(BlockPos{0, 0, -1}, "minecraft:redstone_wire");
        one.set_block(BlockPos{0, 0, -1},
                      signals.with_power(one.block_at(BlockPos{0, 0, -1}), row.side));

        INFO(row.mode << " back=" << row.back << " side=" << row.side);
        REQUIRE(redstone.comparator_output(one, BlockPos{0, 0, 0},
                                           one.block_at(BlockPos{0, 0, 0})) == row.out);
    }
}

TEST_CASE("a container reads as one plus fourteen fourteenths of full",
          "[redstone][comparator]") {
    REQUIRE_REGISTRY();
    // A single chest is 27 slots of 64. Measured against a real chest, stone by
    // stone: 0 items reads 0, one item reads 1, and it climbs only when the
    // fill crosses a fourteenth.
    REQUIRE(Redstone::container_reading(0.0F, false) == 0);
    REQUIRE(Redstone::container_reading(1.0F / (27.0F * 64.0F), true) == 1);
    REQUIRE(Redstone::container_reading(0.5F, true) == 8);
    REQUIRE(Redstone::container_reading(1.0F, true) == 15);

    // And the comparator behind a container reports what the world says it
    // holds, in place of any signal.
    world.put(BlockPos{0, 0, 0}, "minecraft:comparator",
              {{"facing", "east"}, {"mode", "compare"}, {"powered", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:chest",
              {{"facing", "north"}, {"type", "single"}, {"waterlogged", "false"}});
    world.set_container(BlockPos{1, 0, 0}, 7);
    REQUIRE(redstone.comparator_output(world, BlockPos{0, 0, 0},
                                       world.block_at(BlockPos{0, 0, 0})) == 7);

    // Through one solid block, which is a real rule and not a convenience.
    TestWorld through{*loaded().blocks};
    through.put(BlockPos{0, 0, 0}, "minecraft:comparator",
                {{"facing", "east"}, {"mode", "compare"}, {"powered", "false"}});
    through.put(BlockPos{1, 0, 0}, "minecraft:stone");
    through.put(BlockPos{2, 0, 0}, "minecraft:chest",
                {{"facing", "north"}, {"type", "single"}, {"waterlogged", "false"}});
    through.set_container(BlockPos{2, 0, 0}, 11);
    REQUIRE(redstone.comparator_output(through, BlockPos{0, 0, 0},
                                       through.block_at(BlockPos{0, 0, 0})) == 11);
}

TEST_CASE("a torch burns out after eight changes in sixty ticks",
          "[redstone][torch]") {
    REQUIRE_REGISTRY();
    TorchHistory history;
    const BlockPos pos{0, 0, 0};
    for (i32 i = 0; i < 7; ++i) {
        INFO("toggle " << i);
        REQUIRE_FALSE(history.record_and_check(pos, i * 4));
    }
    // The eighth inside the window is the one that stops it.
    REQUIRE(history.record_and_check(pos, 28));

    // Spread past the window, it never burns out: the oldest fall out first.
    TorchHistory slow;
    for (i32 i = 0; i < 40; ++i) {
        REQUIRE_FALSE(slow.record_and_check(pos, i * 20));
    }
    // Another torch's toggles do not count against this one.
    TorchHistory shared;
    for (i32 i = 0; i < 7; ++i) {
        (void)shared.record_and_check(BlockPos{1, 0, 0}, i);
    }
    REQUIRE_FALSE(shared.record_and_check(pos, 8));
}

TEST_CASE("a torch goes out when its own block is powered, and comes back",
          "[redstone][torch]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:stone");
    world.put(BlockPos{0, 1, 0}, "minecraft:redstone_torch", {{"lit", "true"}});
    world.put(BlockPos{0, -1, 0}, "minecraft:lever",
              {{"face", "ceiling"}, {"facing", "north"}, {"powered", "true"}});

    REQUIRE(redstone.neighbour_changed(world, BlockPos{0, 1, 0}, BlockPos{0, 0, 0}));
    // Two ticks, and then it is out.
    std::vector<world::ScheduledTick> pending;
    world.queue.snapshot(pending);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].trigger_tick == 2);
    settle(redstone, world, 4);
    REQUIRE_FALSE(signals.flag_of(world.block_at(BlockPos{0, 1, 0}), "lit"));

    // Take the power away and it relights, two ticks later.
    world.put(BlockPos{0, -1, 0}, "minecraft:lever",
              {{"face", "ceiling"}, {"facing", "north"}, {"powered", "false"}});
    REQUIRE(redstone.neighbour_changed(world, BlockPos{0, 1, 0}, BlockPos{0, 0, 0}));
    settle(redstone, world, 4);
    REQUIRE(signals.flag_of(world.block_at(BlockPos{0, 1, 0}), "lit"));
}

TEST_CASE("wire climbs a block and drops off one", "[redstone][wire]") {
    REQUIRE_REGISTRY();
    //  wire at (1,1) climbs onto the stone at (1,0), because nothing solid is
    //  over the wire at (0,0)'s head.
    world.put(BlockPos{0, -1, 0}, "minecraft:stone");
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_wire");
    world.put(BlockPos{1, 0, 0}, "minecraft:stone");
    world.put(BlockPos{1, 1, 0}, "minecraft:redstone_wire");
    REQUIRE(redstone.wire_reach(world, BlockPos{0, 0, 0}, Direction::East) == WireSide::Up);

    // Put a solid block over the lower wire's head and it can no longer climb.
    world.put(BlockPos{0, 1, 0}, "minecraft:stone");
    REQUIRE(redstone.wire_reach(world, BlockPos{0, 0, 0}, Direction::East) == WireSide::None);

    // Dropping down works past something that is not solid.
    TestWorld down{*loaded().blocks};
    down.put(BlockPos{0, 0, 0}, "minecraft:redstone_wire");
    down.put(BlockPos{1, 0, 0}, "minecraft:oak_fence");
    down.put(BlockPos{1, -1, 0}, "minecraft:redstone_wire");
    REQUIRE(redstone.wire_reach(down, BlockPos{0, 0, 0}, Direction::East) == WireSide::Side);
}

TEST_CASE("a wire only points at what it is connected to", "[redstone][wire]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, -1, 0}, "minecraft:stone");
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_wire");
    // A repeater faces its input, so a wire meets it head on and never from the
    // side.
    world.put(BlockPos{1, 0, 0}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    REQUIRE(redstone.wire_reach(world, BlockPos{0, 0, 0}, Direction::East) == WireSide::Side);
    world.put(BlockPos{0, 0, 1}, "minecraft:repeater",
              {{"facing", "west"}, {"delay", "1"}, {"powered", "false"}, {"locked", "false"}});
    REQUIRE(redstone.wire_reach(world, BlockPos{0, 0, 0}, Direction::South) == WireSide::None);
}

TEST_CASE("a lamp lights at once and goes out four ticks later",
          "[redstone][consumer]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:redstone_lamp", {{"lit", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:redstone_block");
    REQUIRE(redstone.neighbour_changed(world, BlockPos{0, 0, 0}, BlockPos{1, 0, 0}));
    REQUIRE(signals.flag_of(world.block_at(BlockPos{0, 0, 0}), "lit"));

    world.put(BlockPos{1, 0, 0}, "minecraft:air");
    REQUIRE(redstone.neighbour_changed(world, BlockPos{0, 0, 0}, BlockPos{1, 0, 0}));
    // Still lit: the lamp has only scheduled its own extinction.
    REQUIRE(signals.flag_of(world.block_at(BlockPos{0, 0, 0}), "lit"));
    settle(redstone, world, 3);
    REQUIRE(signals.flag_of(world.block_at(BlockPos{0, 0, 0}), "lit"));
    settle(redstone, world, 2);
    REQUIRE_FALSE(signals.flag_of(world.block_at(BlockPos{0, 0, 0}), "lit"));
}
