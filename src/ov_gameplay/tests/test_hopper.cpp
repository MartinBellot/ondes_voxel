// The hopper, against what a real 1.20.1 server did.
//
// The rate is the measured one: a chest → hopper → chest rig moved 30
// cobblestone in a span of 242 server ticks that the server itself timed, which
// is 8.07 ticks an item against a cooldown of 8 and two console round trips at
// the ends. With a lever on the hopper the same span moved **zero**. See
// docs/provenance/redstone.md §12.
//
// Everything else here is shape rather than timing, and the shapes that matter
// are the ones that look right until a comparator reads them: one item at a
// time and not a stack, merging before filling, and a remainder that is handed
// back rather than dropped.
#include "ov/gameplay/hopper.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr registry::ProtocolId kCobble{1};
constexpr registry::ProtocolId kDirt{2};
constexpr registry::ProtocolId kEgg{3};

/// A container that only gives up its last slot, and only downwards — the
/// shape a furnace has, reduced to what the rule can see.
class Furnace final : public ItemContainer {
public:
    [[nodiscard]] i32 slot_count() const override { return 3; }

    [[nodiscard]] SlotStack slot(i32 index) const override {
        return index >= 0 && index < 3 ? slots_[static_cast<usize>(index)] : SlotStack{};
    }

    void set_slot(i32 index, const SlotStack& stack) override {
        if (index >= 0 && index < 3) {
            slots_[static_cast<usize>(index)] = stack;
        }
    }

    [[nodiscard]] i32 max_stack(registry::ProtocolId) const override { return 64; }

    [[nodiscard]] bool can_take_from(i32 index, Direction face) const override {
        return index == 2 && face == Direction::Down;
    }

    std::array<SlotStack, 3> slots_{};
};

}  // namespace

TEST_CASE("a hopper moves one item at a time, never a stack", "[hopper]") {
    SimpleContainer source{27, 64};
    SimpleContainer hopper{HopperRules::kSlots, 64};
    source.set_slot(0, SlotStack{kCobble, 64, 0});

    REQUIRE(HopperRules::pull_one(hopper, source));
    CHECK(source.slot(0).count == 63);
    CHECK(hopper.slot(0).count == 1);

    // Ten more pulls, ten more items. A hopper that moved a stack would empty
    // the chest here, and every item filter ever built would stop working —
    // a filter is a race between two hoppers at one item each.
    for (i32 i = 0; i < 10; ++i) {
        REQUIRE(HopperRules::pull_one(hopper, source));
    }
    CHECK(source.slot(0).count == 53);
    CHECK(hopper.total() == 11);
}

TEST_CASE("the cooldown is eight ticks", "[hopper]") {
    // Not a behaviour this file can run — it is the caller that holds the
    // cooldown — but it is the number the whole of hopper timing rests on, and
    // it is measured. 30 items in 242 ticks is 8.07, which is 8 plus the two
    // console round trips the measurement paid at the ends.
    STATIC_REQUIRE(HopperRules::kTransferCooldown == 8);
    STATIC_REQUIRE(HopperRules::kSlots == 5);
}

TEST_CASE("a hopper tops up a stack before it opens a new slot", "[hopper]") {
    SimpleContainer into{5, 64};
    into.set_slot(0, SlotStack{kDirt, 1, 0});
    into.set_slot(1, SlotStack{kCobble, 40, 0});

    SlotStack incoming{kCobble, 10, 0};
    CHECK(HopperRules::insert(into, incoming, Direction::Up) == 10);
    CHECK(incoming.empty());
    // Merged, not spread. Spreading looks identical in a screenshot and is
    // visible the moment a comparator reads the container.
    CHECK(into.slot(1).count == 50);
    CHECK(into.slot(2).empty());
}

TEST_CASE("a stack that does not fit is handed back, not lost", "[hopper]") {
    SimpleContainer into{1, 64};
    into.set_slot(0, SlotStack{kCobble, 60, 0});

    SlotStack incoming{kCobble, 10, 0};
    CHECK(HopperRules::insert(into, incoming, Direction::Up) == 4);
    CHECK(into.slot(0).count == 64);
    // Six left over. A rule that returned success and dropped them would
    // destroy items every time a chest filled up.
    CHECK(incoming.count == 6);
    CHECK(incoming.item == kCobble);
}

TEST_CASE("a tag keeps two stacks of the same item apart", "[hopper]") {
    SimpleContainer into{2, 64};
    into.set_slot(0, SlotStack{kCobble, 1, 0x1234});

    SlotStack plain{kCobble, 1, 0};
    CHECK(HopperRules::insert(into, plain, Direction::Up) == 1);
    // Not merged into the tagged stack: a named sword does not stack with a
    // plain one, and the same has to be true of anything else with a tag.
    CHECK(into.slot(0).count == 1);
    CHECK(into.slot(1).count == 1);
    CHECK(into.slot(1).tag == 0);
}

TEST_CASE("the stack limit is asked of the container", "[hopper]") {
    SimpleContainer into{3, 16};
    SlotStack       eggs{kEgg, 40, 0};
    CHECK(HopperRules::insert(into, eggs, Direction::Up) == 40);
    CHECK(into.slot(0).count == 16);
    CHECK(into.slot(1).count == 16);
    CHECK(into.slot(2).count == 8);
}

TEST_CASE("a hopper reaching up takes from the bottom face", "[hopper]") {
    Furnace         furnace;
    SimpleContainer hopper{HopperRules::kSlots, 64};
    furnace.slots_[0] = SlotStack{kCobble, 8, 0};   // the input
    furnace.slots_[1] = SlotStack{kCobble, 8, 0};   // the fuel
    furnace.slots_[2] = SlotStack{kDirt, 8, 0};     // the output

    REQUIRE(HopperRules::pull_one(hopper, furnace));
    // The output, and nothing else. A hopper that took the fuel out of a
    // furnace would be a furnace that never smelted anything.
    CHECK(hopper.slot(0).item == kDirt);
    CHECK(furnace.slots_[2].count == 7);
    CHECK(furnace.slots_[0].count == 8);
    CHECK(furnace.slots_[1].count == 8);
}

TEST_CASE("a hopper pushes before it pulls", "[hopper]") {
    SimpleContainer above{27, 64};
    SimpleContainer hopper{HopperRules::kSlots, 64};
    SimpleContainer below{27, 64};
    above.set_slot(0, SlotStack{kCobble, 5, 0});
    hopper.set_slot(0, SlotStack{kDirt, 1, 0});

    const HopperRules::TickResult result =
        HopperRules::tick(hopper, &below, Direction::Down, &above);
    CHECK(result.pushed);
    CHECK(result.pulled);
    CHECK(result.moved());
    // The dirt went down and the cobble came in — but the cobble did **not**
    // pass through in the same tick. Pulling first would deliver a five-hopper
    // chain in one tick instead of five cooldowns.
    CHECK(below.slot(0).item == kDirt);
    CHECK(below.total() == 1);
    CHECK(hopper.total() == 1);
    CHECK(hopper.slot(0).item == kCobble);
}

TEST_CASE("a hopper with nowhere to push and nothing to pull does nothing",
          "[hopper]") {
    SimpleContainer hopper{HopperRules::kSlots, 64};
    const HopperRules::TickResult result =
        HopperRules::tick(hopper, nullptr, Direction::Down, nullptr);
    CHECK_FALSE(result.moved());
}
