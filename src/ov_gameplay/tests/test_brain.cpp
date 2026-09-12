// The brain framework (memories, schedule, behaviours, activities) and the
// gossip model, the latter against what a real 1.20.1 server measured
// (scripts/measure_villager_life.py; docs/provenance/cerveaux.md).
#include "ov/gameplay/brain/brain.hpp"
#include "ov/gameplay/brain/gossip.hpp"
#include "ov/gameplay/brain/memory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace ov;
using namespace ov::gameplay;
using namespace ov::gameplay::brain;

TEST_CASE("a memory with a time to live is forgotten when it runs out", "[brain]") {
    Memories m;
    m.set(MemoryType::GolemDetectedRecently, MemoryValue::unit(), 3);
    m.set(MemoryType::Home, MemoryValue::of_pos(BlockPos{1, 2, 3}));
    m.tick();
    m.tick();
    CHECK(m.has(MemoryType::GolemDetectedRecently));
    CHECK(m.ttl(MemoryType::GolemDetectedRecently) == 1);
    m.tick();
    CHECK_FALSE(m.has(MemoryType::GolemDetectedRecently));
    CHECK(m.pos(MemoryType::Home) == BlockPos{1, 2, 3});
    CHECK(m.ttl(MemoryType::Home) == kForever);
}

TEST_CASE("memory and dimension names are vanilla's", "[brain]") {
    CHECK(memory_info(MemoryType::JobSite).name == "minecraft:job_site");
    CHECK(memory_info(MemoryType::LastWorkedAtPoi).name == "minecraft:last_worked_at_poi");
    CHECK(memory_from_name("minecraft:meeting_point") == MemoryType::MeetingPoint);
    CHECK_FALSE(memory_from_name("minecraft:nothing").has_value());
    CHECK(memory_info(MemoryType::Home).saved);
    CHECK_FALSE(memory_info(MemoryType::WalkTarget).saved);
    CHECK(dimension_from_name("minecraft:the_nether") == Dimension::Nether);
}

TEST_CASE("the villager's schedule", "[brain]") {
    const Schedule& s = villager_schedule();
    CHECK(s.at(0) == Activity::Rest);
    CHECK(s.at(9) == Activity::Rest);
    CHECK(s.at(10) == Activity::Idle);
    CHECK(s.at(1999) == Activity::Idle);
    CHECK(s.at(2000) == Activity::Work);
    CHECK(s.at(9000) == Activity::Meet);
    CHECK(s.at(11000) == Activity::Idle);
    CHECK(s.at(12000) == Activity::Rest);
    CHECK(s.at(23999) == Activity::Rest);
    CHECK(s.at(24000 + 3000) == Activity::Work);
    CHECK(villager_baby_schedule().at(4000) == Activity::Idle);
    CHECK(villager_baby_schedule().at(7000) == Activity::Play);
}

namespace {

constexpr std::array<MemoryRequirement, 1> kNeedsHome{{{MemoryType::Home, MemoryStatus::Present}}};

class Counter final : public Behavior {
public:
    Counter(i32 min, i32 max, bool keep) : Behavior{kNeedsHome, min, max}, keep_{keep} {}
    i32 starts{0};
    i32 ticks{0};
    i32 stops{0};
    [[nodiscard]] std::string_view name() const noexcept override { return "counter"; }

protected:
    bool can_still_use(BrainContext&) override { return keep_; }
    void start(BrainContext&) override { ++starts; }
    void tick(BrainContext&) override { ++ticks; }
    void stop(BrainContext&) override { ++stops; }

private:
    bool keep_{false};
};

struct Rig {
    MobBrain                 mob;
    math::LegacyRandomSource random{42};
    GoalContext              goal{};
    Brain                    brain;
    i64                      time{0};

    Rig() {
        goal.brain  = &mob;
        goal.random = &random;
    }
    BrainContext context(i64 day = 6000) { return BrainContext{goal, brain, time, day}; }
    void         tick(i64 day = 6000) {
        BrainContext c = context(day);
        brain.tick(c);
        ++time;
    }
};

}  // namespace

TEST_CASE("a behaviour starts only when its memories are there, and runs its duration",
          "[brain]") {
    Rig   rig;
    auto  owned   = std::make_unique<Counter>(5, 5, true);
    auto* counter = owned.get();
    rig.brain.set_core(Activity::Core);
    rig.brain.add(Activity::Core, 0, std::move(owned));
    rig.tick();
    CHECK(counter->starts == 0);
    rig.mob.memories.set(MemoryType::Home, MemoryValue::of_pos(BlockPos{0, 0, 0}));
    rig.tick();  // starts at t = 1 and ticks the same tick: runs until t = 6
    CHECK(counter->starts == 1);
    CHECK(counter->running());
    for (int i = 0; i < 5; ++i) {
        rig.tick();
    }
    CHECK(counter->running());
    rig.tick();  // t = 7 > end: stopped
    CHECK_FALSE(counter->running());
    CHECK(counter->stops == 1);
    CHECK(counter->ticks == 6);
}

TEST_CASE("a behaviour that cannot still be used stops on the next tick", "[brain]") {
    Rig   rig;
    auto  owned   = std::make_unique<Counter>(60, 60, false);
    auto* counter = owned.get();
    rig.mob.memories.set(MemoryType::Home, MemoryValue::of_pos(BlockPos{0, 0, 0}));
    rig.brain.set_core(Activity::Core);
    rig.brain.add(Activity::Core, 0, std::move(owned));
    rig.tick();
    CHECK(counter->starts == 1);
    CHECK(counter->stops == 1);  // start, then stopped the same tick
    rig.tick();
    CHECK(counter->starts == 2);
}

TEST_CASE("an activity that needs a memory falls back to the default", "[brain]") {
    Rig rig;
    rig.brain.set_core(Activity::Core);
    rig.brain.set_default(Activity::Idle);
    rig.brain.require(Activity::Work, {{MemoryType::JobSite, MemoryStatus::Present}});
    rig.brain.set_schedule(&villager_schedule());
    const auto consult = [&] {
        BrainContext c = rig.context(3000);
        rig.brain.update_from_schedule(c);
    };
    consult();
    CHECK(rig.brain.current() == Activity::Idle);
    rig.mob.memories.set(MemoryType::JobSite, MemoryValue::of_pos(BlockPos{3, 0, 0}));
    rig.time += 10;
    consult();  // 10 ticks later: not consulted yet
    CHECK(rig.brain.current() == Activity::Idle);
    rig.time += 11;
    consult();
    CHECK(rig.brain.current() == Activity::Work);
    CHECK(rig.brain.is_active(Activity::Core));
}

TEST_CASE("behaviours of an activity left are stopped", "[brain]") {
    Rig   rig;
    auto  owned   = std::make_unique<Counter>(100, 100, true);
    auto* counter = owned.get();
    rig.mob.memories.set(MemoryType::Home, MemoryValue::of_pos(BlockPos{0, 0, 0}));
    rig.brain.set_core(Activity::Core);
    rig.brain.add(Activity::Rest, 0, std::move(owned));
    {
        BrainContext c = rig.context();
        rig.brain.set_active(Activity::Rest, c);
    }
    rig.tick();
    CHECK(counter->running());
    {
        BrainContext c = rig.context();
        rig.brain.set_active(Activity::Idle, c);
    }
    CHECK_FALSE(counter->running());
}

TEST_CASE("sensors sense on their own interval", "[brain]") {
    Rig             rig;
    static i32      sensed[2]{};
    sensed[0] = sensed[1] = 0;
    rig.brain.add_sensor(0, 20, rig.random);
    rig.brain.add_sensor(1, 200, rig.random);
    rig.brain.set_sense([](BrainContext&, u8 id) { ++sensed[id]; });
    for (int i = 0; i < 400; ++i) {
        rig.tick();
    }
    CHECK(sensed[0] == 20);
    CHECK(sensed[1] == 2);
}

// ── Gossip ──────────────────────────────────────────────────────────────────

namespace {
const net::Uuid kProbe{0x1141'0f39'f1ec'd5c5ULL, 0xa6e9'1e2d'1677'd906ULL};
const net::Uuid kOther{11, 22};

struct PriceCell {
    const char*                          label;
    std::array<std::pair<GossipType, i32>, 5> gossips;
    usize                                count;
    i32                                  hero;  // -1: none
    std::array<i32, 5>                   specials;
};

// Offers of the campaign: base 24 × 0.05, 10 × 0.2, 1 × 0.05, 5 × 0.05, 20 × 0.2.
constexpr std::array<std::pair<i32, f32>, 5> kOffers{
    {{24, 0.05F}, {10, 0.2F}, {1, 0.05F}, {5, 0.05F}, {20, 0.2F}}};

using G = GossipType;
// Read off Merchant Offers on a real 1.20.1 server, 18 villagers.
const std::array<PriceCell, 17> kPriceCells{{
    {"none", {}, 0, -1, {0, 0, 0, 0, 0}},
    {"minor_positive 10", {{{G::MinorPositive, 10}}}, 1, -1, {0, -2, 0, 0, -2}},
    {"minor_positive 100", {{{G::MinorPositive, 100}}}, 1, -1, {-5, -20, -5, -5, -20}},
    {"minor_positive 200", {{{G::MinorPositive, 200}}}, 1, -1, {-10, -40, -10, -10, -40}},
    {"minor_positive 250", {{{G::MinorPositive, 250}}}, 1, -1, {-12, -50, -12, -12, -50}},
    {"major_positive 20", {{{G::MajorPositive, 20}}}, 1, -1, {-5, -20, -5, -5, -20}},
    {"major_positive 100", {{{G::MajorPositive, 100}}}, 1, -1, {-25, -100, -25, -25, -100}},
    {"trading 25", {{{G::Trading, 25}}}, 1, -1, {-1, -5, -1, -1, -5}},
    {"trading 7", {{{G::Trading, 7}}}, 1, -1, {0, -1, 0, 0, -1}},
    {"minor_negative 25", {{{G::MinorNegative, 25}}}, 1, -1, {2, 5, 2, 2, 5}},
    {"major_negative 25", {{{G::MajorNegative, 25}}}, 1, -1, {7, 25, 7, 7, 25}},
    {"cured", {{{G::MajorPositive, 20}, {G::MinorPositive, 25}}}, 2, -1, {-6, -25, -6, -6, -25}},
    {"all five",
     {{{G::MajorPositive, 10}, {G::MinorPositive, 30}, {G::Trading, 12}, {G::MinorNegative, 40},
       {G::MajorNegative, 3}}},
     5, -1, {-1, -7, -1, -1, -7}},
    {"hero 0", {}, 0, 0, {-7, -3, -1, -1, -6}},
    {"hero 1", {}, 0, 1, {-8, -3, -1, -1, -7}},
    {"hero 4", {}, 0, 4, {-13, -5, -1, -2, -11}},
    {"hero 0 + minor_positive 100", {{{G::MinorPositive, 100}}}, 1, 0, {-12, -23, -6, -6, -26}},
}};
}  // namespace

TEST_CASE("reputation prices every offer as the real server did", "[brain][gossip][parity]") {
    for (const PriceCell& cell : kPriceCells) {
        Gossips g;
        for (usize i = 0; i < cell.count; ++i) {
            g.put(GossipEntry{kProbe, cell.gossips[i].first, cell.gossips[i].second});
        }
        const i32 rep = g.reputation(kProbe);
        for (usize k = 0; k < kOffers.size(); ++k) {
            i32 special = rep != 0 ? reputation_price_diff(rep, kOffers[k].second) : 0;
            if (cell.hero >= 0) {
                special += hero_price_diff(cell.hero, kOffers[k].first);
            }
            INFO(cell.label << ", offer " << k);
            CHECK(special == cell.specials[k]);
        }
        // Control: the same gossip about someone else changes nothing.
        CHECK(g.reputation(kOther) == 0);
    }
}

TEST_CASE("gossip adds, clamps at the maximum, and keeps a value read over it", "[gossip]") {
    Gossips g;
    g.add_event(kProbe, ReputationEvent::Trade);
    g.add_event(kProbe, ReputationEvent::Trade);
    CHECK(g.value(kProbe, G::Trading) == 4);  // measured: 2 then 4
    g.add_event(kProbe, ReputationEvent::VillagerHurt);
    g.add_event(kProbe, ReputationEvent::VillagerHurt);
    CHECK(g.value(kProbe, G::MinorNegative) == 50);  // measured: 25 then 50
    for (int i = 0; i < 20; ++i) {
        g.add(kProbe, G::Trading, 2);
    }
    CHECK(g.value(kProbe, G::Trading) == 25);
    g.put(GossipEntry{kOther, G::MinorPositive, 250});
    g.add(kOther, G::MinorPositive, 25);
    CHECK(g.value(kOther, G::MinorPositive) == 250);
    g.add_event(kOther, ReputationEvent::ZombieVillagerCured);
    CHECK(g.value(kOther, G::MajorPositive) == 20);
}

TEST_CASE("what a villager passes on, as measured at a bell", "[gossip][parity]") {
    // A had minor_negative 100, trading 20, major_positive 50, major_negative
    // 40, minor_positive 60; B read minor_negative 80 and major_negative 30.
    Gossips a;
    a.put(GossipEntry{kProbe, G::MinorNegative, 100});
    a.put(GossipEntry{kProbe, G::Trading, 20});
    a.put(GossipEntry{kProbe, G::MajorPositive, 50});
    a.put(GossipEntry{kProbe, G::MajorNegative, 40});
    a.put(GossipEntry{kProbe, G::MinorPositive, 60});
    for (i64 seed = 0; seed < 64; ++seed) {
        math::LegacyRandomSource random{seed};
        Gossips                  b;
        b.transfer_from(a, random, 10);
        INFO("seed " << seed);
        CHECK(b.value(kProbe, G::Trading) == 0);        // 20 - 20: nothing
        CHECK(b.value(kProbe, G::MajorPositive) == 0);  // never passed on
        const i32 mn = b.value(kProbe, G::MinorNegative);
        const i32 mj = b.value(kProbe, G::MajorNegative);
        const i32 mp = b.value(kProbe, G::MinorPositive);
        CHECK((mn == 0 || mn == 80));
        CHECK((mj == 0 || mj == 30));
        CHECK((mp == 0 || mp == 55));
    }
    // Hearing never lowers what one knew.
    math::LegacyRandomSource random{1};
    Gossips                  c;
    c.put(GossipEntry{kProbe, G::MinorNegative, 150});
    c.transfer_from(a, random, 10);
    CHECK(c.value(kProbe, G::MinorNegative) == 150);
}

TEST_CASE("a day of forgetting", "[gossip]") {
    Gossips g;
    g.put(GossipEntry{kProbe, G::MajorNegative, 50});
    g.put(GossipEntry{kProbe, G::MinorNegative, 50});
    g.put(GossipEntry{kProbe, G::MinorPositive, 50});
    g.put(GossipEntry{kProbe, G::MajorPositive, 50});
    g.put(GossipEntry{kProbe, G::Trading, 10});
    i64 last = 0;
    CHECK_FALSE(maybe_decay(g, last, 5000));  // 0: set to now, nothing forgotten (measured)
    CHECK(last == 5000);
    CHECK_FALSE(maybe_decay(g, last, 5000 + 23999));
    CHECK(maybe_decay(g, last, 5000 + 24000));
    CHECK(g.value(kProbe, G::MajorNegative) == 40);
    CHECK(g.value(kProbe, G::MinorNegative) == 30);
    CHECK(g.value(kProbe, G::MinorPositive) == 49);
    CHECK(g.value(kProbe, G::MajorPositive) == 50);
    CHECK(g.value(kProbe, G::Trading) == 8);
}
