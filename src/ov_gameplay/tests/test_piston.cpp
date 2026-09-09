// Pistons against a real server's answers: what moves, what refuses, and the
// bug the game kept. See docs/provenance/redstone.md.
#include "ov/gameplay/piston.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<Redstone>                redstone;
    std::optional<Pistons>                 pistons;
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
            out.pistons.emplace(*out.blocks, *out.registries, *out.redstone);
        }
        return out;
    }();
    return state;
}

class TestWorld final : public RedstoneWorld {
public:
    explicit TestWorld(const registry::BlockRegistry& blocks) : blocks_{&blocks} {}

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto it = cells_.find(key(pos));
        return it == cells_.end() ? registry::kAirState : it->second;
    }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
    }

    void schedule_tick(BlockPos pos, registry::BlockId block, i32 delay,
                       world::TickPriority priority) override {
        queue.schedule(pos, block, now, delay, priority);
    }

    [[nodiscard]] bool tick_scheduled(BlockPos pos, registry::BlockId block) const override {
        return queue.is_scheduled(pos, block);
    }

    [[nodiscard]] i32 container_signal(BlockPos) const override { return -1; }

    [[nodiscard]] i64 game_time() const override { return now; }

    void put(BlockPos pos, std::string_view name,
             std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
        const auto block = blocks_->find_block(name);
        REQUIRE(block.has_value());
        registry::BlockStateId state = blocks_->default_state(*block);
        for (const auto& [key_name, value] : props) {
            const auto prop = blocks_->find_property(*block, key_name);
            REQUIRE(prop.has_value());
            for (u16 i = 0; i < prop->values.size(); ++i) {
                if (prop->values[i] == value) {
                    state = blocks_->with_property(state, *prop, i);
                    break;
                }
            }
        }
        set_block(pos, state);
    }

    [[nodiscard]] std::string name_at(BlockPos pos) const {
        return std::string{blocks_->block_name(blocks_->block_of(block_at(pos)))};
    }

    world::BlockTickScheduler queue;
    i64                       now{0};

private:
    [[nodiscard]] static i64 key(BlockPos pos) {
        return (static_cast<i64>(pos.x) << 40) ^ (static_cast<i64>(pos.y) << 20) ^
               static_cast<i64>(pos.z);
    }

    const registry::BlockRegistry*        blocks_{nullptr};
    std::map<i64, registry::BlockStateId> cells_;
};

void settle(Redstone&, Pistons& pistons, TestWorld& world, i64 ticks = 8) {
    for (i64 i = 0; i < ticks; ++i) {
        ++world.now;
        std::vector<world::ScheduledTick> due;
        world.queue.drain_due(world.now, due);
        for (const world::ScheduledTick& tick : due) {
            pistons.scheduled_tick(world, tick.pos, tick.block);
        }
    }
}

#define REQUIRE_REGISTRY()                                                    \
    if (!loaded().pistons.has_value()) {                                      \
        SKIP("registry.ovpack not built");                                    \
    }                                                                         \
    Redstone& redstone = const_cast<Redstone&>(*loaded().redstone); /*NOLINT*/ \
    Pistons&  pistons  = const_cast<Pistons&>(*loaded().pistons);   /*NOLINT*/ \
    TestWorld world{*loaded().blocks};                                        \
    (void)redstone

}  // namespace

TEST_CASE("push reactions come from the game, not from a shape", "[piston]") {
    REQUIRE_REGISTRY();
    const auto state = [&](std::string_view name) {
        const auto id = loaded().blocks->find_block(name);
        REQUIRE(id.has_value());
        return loaded().blocks->default_state(*id);
    };

    REQUIRE(pistons.reaction_of(state("minecraft:stone")) == PushReaction::Normal);
    REQUIRE(pistons.reaction_of(state("minecraft:obsidian")) == PushReaction::Block);
    REQUIRE(pistons.reaction_of(state("minecraft:chest")) == PushReaction::Block);
    REQUIRE(pistons.reaction_of(state("minecraft:hopper")) == PushReaction::Block);
    REQUIRE(pistons.reaction_of(state("minecraft:oak_sign")) == PushReaction::Block);
    // Leaves are a full cube and a piston breaks them anyway. No rule about
    // collision shapes would have said so; the game did.
    REQUIRE(pistons.reaction_of(state("minecraft:oak_leaves")) == PushReaction::Destroy);
    REQUIRE(pistons.reaction_of(state("minecraft:cobweb")) == PushReaction::Destroy);
    REQUIRE(pistons.reaction_of(state("minecraft:comparator")) == PushReaction::Destroy);
    REQUIRE(pistons.reaction_of(state("minecraft:white_shulker_box")) == PushReaction::Destroy);
    // Slime and honey move, and stick.
    REQUIRE(pistons.reaction_of(state("minecraft:slime_block")) == PushReaction::Normal);
    REQUIRE(pistons.is_sticky_block(state("minecraft:slime_block")));
    REQUIRE(pistons.is_sticky_block(state("minecraft:honey_block")));
    REQUIRE_FALSE(pistons.is_sticky_block(state("minecraft:stone")));

    // The gaps are named, and there are not many of them.
    REQUIRE(pistons.unmeasured().size() == 103);
}

TEST_CASE("a piston pushes twelve and refuses thirteen", "[piston]") {
    REQUIRE_REGISTRY();
    const auto build = [&](usize n) {
        TestWorld one{*loaded().blocks};
        one.put(BlockPos{0, 0, 0}, "minecraft:piston",
                {{"facing", "east"}, {"extended", "false"}});
        for (usize i = 1; i <= n; ++i) {
            one.put(BlockPos{static_cast<i32>(i), 0, 0}, "minecraft:iron_block");
        }
        return one;
    };

    for (usize n = 0; n <= 12; ++n) {
        TestWorld one  = build(n);
        const auto plan = pistons.plan_push(one, BlockPos{0, 0, 0}, Direction::East, true);
        INFO("length " << n);
        REQUIRE(plan.possible);
        REQUIRE(plan.moved.size() == n);
    }
    // Thirteen is not "push twelve of them": the whole push fails.
    TestWorld thirteen = build(13);
    const auto plan     = pistons.plan_push(thirteen, BlockPos{0, 0, 0}, Direction::East, true);
    REQUIRE_FALSE(plan.possible);
    REQUIRE(plan.refusal == PushPlan::Refusal::TooMany);
    REQUIRE(plan.moved.empty());
}

TEST_CASE("an immovable block stops the push wherever it is", "[piston]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:stone");
    world.put(BlockPos{2, 0, 0}, "minecraft:obsidian");
    const auto plan = pistons.plan_push(world, BlockPos{0, 0, 0}, Direction::East, true);
    REQUIRE_FALSE(plan.possible);
    REQUIRE(plan.refusal == PushPlan::Refusal::Immovable);
}

TEST_CASE("a torch in the way breaks and lets the push through", "[piston]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:stone");
    world.put(BlockPos{2, 0, 0}, "minecraft:redstone_wire");
    const auto plan = pistons.plan_push(world, BlockPos{0, 0, 0}, Direction::East, true);
    REQUIRE(plan.possible);
    REQUIRE(plan.moved.size() == 1);
    REQUIRE(plan.destroyed.size() == 1);
}

TEST_CASE("slime drags its neighbours into the same twelve", "[piston]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:slime_block");
    world.put(BlockPos{1, 1, 0}, "minecraft:stone");
    world.put(BlockPos{1, 0, 1}, "minecraft:stone");
    const auto plan = pistons.plan_push(world, BlockPos{0, 0, 0}, Direction::East, true);
    REQUIRE(plan.possible);
    // The slime block and the two blocks stuck to its sides.
    REQUIRE(plan.moved.size() == 3);

    // Eleven in a line plus two stuck to a slime block is thirteen, and fails.
    TestWorld over{*loaded().blocks};
    over.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    over.put(BlockPos{1, 0, 0}, "minecraft:slime_block");
    over.put(BlockPos{1, 1, 0}, "minecraft:stone");
    over.put(BlockPos{1, 0, 1}, "minecraft:stone");
    for (i32 x = 2; x <= 11; ++x) {
        over.put(BlockPos{x, 0, 0}, "minecraft:iron_block");
    }
    const auto refused = pistons.plan_push(over, BlockPos{0, 0, 0}, Direction::East, true);
    REQUIRE_FALSE(refused.possible);
    REQUIRE(refused.refusal == PushPlan::Refusal::TooMany);
}

TEST_CASE("quasi-connectivity: a piston reads the block above it as its own",
          "[piston][qc]") {
    REQUIRE_REGISTRY();
    // The measured layout: a redstone block one up and one north of the piston.
    // It shares no face with the piston, and the piston extends anyway.
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    world.put(BlockPos{0, 1, -1}, "minecraft:redstone_block");
    REQUIRE(pistons.wants_extended(world, BlockPos{0, 0, 0}, Direction::East));

    // One block higher, touching neither the piston nor the space above it: no.
    TestWorld control{*loaded().blocks};
    control.put(BlockPos{0, 0, 0}, "minecraft:piston",
                {{"facing", "east"}, {"extended", "false"}});
    control.put(BlockPos{0, 2, -1}, "minecraft:redstone_block");
    REQUIRE_FALSE(pistons.wants_extended(control, BlockPos{0, 0, 0}, Direction::East));

    // A lamp in the same place is not quasi-connected: only the piston is.
    TestWorld lamp{*loaded().blocks};
    lamp.put(BlockPos{0, 0, 0}, "minecraft:redstone_lamp", {{"lit", "false"}});
    lamp.put(BlockPos{0, 1, -1}, "minecraft:redstone_block");
    REQUIRE_FALSE(redstone.consumer_powered(lamp, BlockPos{0, 0, 0}));
}

TEST_CASE("a piston does not power itself through what it pushed", "[piston][qc]") {
    REQUIRE_REGISTRY();
    // A source in front of a piston, on its `facing` side, is skipped — which is
    // what stops an extended piston from holding itself out forever.
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:redstone_block");
    REQUIRE_FALSE(pistons.wants_extended(world, BlockPos{0, 0, 0}, Direction::East));
}

TEST_CASE("extending writes the head and moves the column", "[piston]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "false"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:iron_block");
    world.put(BlockPos{0, 1, -1}, "minecraft:redstone_block");

    REQUIRE(pistons.neighbour_changed(world, BlockPos{0, 0, 0}));
    settle(redstone, pistons, world);

    REQUIRE(world.name_at(BlockPos{0, 0, 0}) == "minecraft:piston");
    REQUIRE(world.name_at(BlockPos{1, 0, 0}) == "minecraft:piston_head");
    REQUIRE(world.name_at(BlockPos{2, 0, 0}) == "minecraft:iron_block");
    REQUIRE(loaded().redstone->signals().flag_of(world.block_at(BlockPos{0, 0, 0}), "extended"));

    // An extended piston conducts nothing — measured, by powering one from the
    // side and reading a wire beyond it.
    REQUIRE_FALSE(loaded().redstone->signals().is_conductor(world.block_at(BlockPos{0, 0, 0})));
}

TEST_CASE("a sticky piston brings one block back", "[piston]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:sticky_piston",
              {{"facing", "east"}, {"extended", "true"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:piston_head",
              {{"facing", "east"}, {"type", "sticky"}, {"short", "false"}});
    world.put(BlockPos{2, 0, 0}, "minecraft:iron_block");

    REQUIRE(pistons.scheduled_tick(world, BlockPos{0, 0, 0}, loaded().redstone->ids().sticky_piston));
    REQUIRE(world.name_at(BlockPos{1, 0, 0}) == "minecraft:iron_block");
    REQUIRE(world.name_at(BlockPos{2, 0, 0}) == "minecraft:air");
    REQUIRE_FALSE(loaded().redstone->signals().flag_of(world.block_at(BlockPos{0, 0, 0}),
                                                       "extended"));
}

TEST_CASE("a plain piston leaves what it pushed where it is", "[piston]") {
    REQUIRE_REGISTRY();
    world.put(BlockPos{0, 0, 0}, "minecraft:piston", {{"facing", "east"}, {"extended", "true"}});
    world.put(BlockPos{1, 0, 0}, "minecraft:piston_head",
              {{"facing", "east"}, {"type", "normal"}, {"short", "false"}});
    world.put(BlockPos{2, 0, 0}, "minecraft:iron_block");

    REQUIRE(pistons.scheduled_tick(world, BlockPos{0, 0, 0}, loaded().redstone->ids().piston));
    REQUIRE(world.name_at(BlockPos{1, 0, 0}) == "minecraft:air");
    REQUIRE(world.name_at(BlockPos{2, 0, 0}) == "minecraft:iron_block");
}
