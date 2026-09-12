// A villager's brain in a real entity world: its day (work, meet, rest, wake),
// gossip between two that meet, three frightened sleepers calling a golem,
// breeding with food and a free bed, a farmer's harvest — each with the
// control the real server was measured against (docs/provenance/cerveaux.md).
#include "ov/gameplay/brain/brain.hpp"
#include "ov/gameplay/brain/villager_brain.hpp"
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/village_mobs.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;
namespace b = ov::gameplay::brain;

namespace {

[[nodiscard]] std::filesystem::path data_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(data_path() / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(data_path() / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] i32 type_id(std::string_view name) {
    const auto types = registries()->find("minecraft:entity_type").value();
    return static_cast<i32>(registries()->protocol_id(types, name).value());
}

/// Grass below y = 0, air above, and placed states.
class Ground final : public world::LevelView {
public:
    explicit Ground(const registry::BlockRegistry& registry) : registry_{&registry} {
        grass_ = registry.default_state(registry.find_block("minecraft:grass_block").value());
    }
    void place(BlockPos pos, std::string_view block,
               std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
        const registry::BlockId id = registry_->find_block(block).value();
        const std::vector<std::pair<std::string_view, std::string_view>> list{props};
        const registry::BlockStateId state =
            list.empty() ? registry_->default_state(id) : registry_->state_for(id, list).value();
        std::erase_if(placed_, [&](const auto& p) { return p.first == pos; });
        placed_.emplace_back(pos, state);
    }
    void bed(BlockPos foot) {  // facing south: the head is one block +z
        // `occupied` spelled out: a partial state starts from the first one,
        // and a bed's first is occupied — no villager claims it.
        place(foot, "minecraft:red_bed",
              {{"facing", "south"}, {"part", "foot"}, {"occupied", "false"}});
        place(BlockPos{foot.x, foot.y, foot.z + 1}, "minecraft:red_bed",
              {{"facing", "south"}, {"part", "head"}, {"occupied", "false"}});
    }
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        for (const auto& [at, state] : placed_) {
            if (at == pos) {
                return state;
            }
        }
        return pos.y < 0 ? grass_ : registry::kAirState;
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry*                           registry_;
    registry::BlockStateId                                   grass_{};
    std::vector<std::pair<BlockPos, registry::BlockStateId>> placed_;
};

struct Floor {
    registry::BlockStateId grass{};
    static registry::BlockStateId look_up(void* context, i32, i32 y, i32) {
        return y < 0 ? static_cast<const Floor*>(context)->grass : registry::BlockStateId{0};
    }
};

struct Village {
    Floor                            floor;
    CollisionWorld                   collisions;
    Ground                           level;
    entity::EntityWorld              world;
    VillagerWorld                    time;
    std::vector<HostileSight>        hostiles;
    std::vector<b::VillagerEvent>    events;
    std::vector<b::VillagerEvent>    seen;
    i64                              tick{0};

    Village()
        : floor{blocks()->default_state(blocks()->find_block("minecraft:grass_block").value())},
          collisions{*blocks(), &Floor::look_up, &floor},
          level{*blocks()},
          world{*registries()} {
        time.day_time        = 6000;
        time.events          = &events;
        time.iron_golem_type = type_id("minecraft:iron_golem");
        time.villager_type   = type_id("minecraft:villager");
    }

    entity::EntityHandle villager(Vec3d at) {
        const auto handle = world.spawn("minecraft:villager", at, net::Uuid{}).value();
        entity::EntityState* state = world.mutable_state(handle);
        world.set_logic(handle, std::make_unique<Mob>(*mob_kind("minecraft:villager"), state->width,
                                                      state->height, state->network_id));
        return handle;
    }
    MobBrain& brain_of(entity::EntityHandle handle) {
        return dynamic_cast<Mob*>(world.logic(handle))->mutable_brain();
    }
    VillagerState& state_of(entity::EntityHandle handle) { return brain_of(handle).villager; }
    const entity::EntityState& body(entity::EntityHandle handle) { return *world.state(handle); }

    /// `day` moves with the clock, as the daylight cycle does.
    void step(i32 ticks = 1, bool daylight = false) {
        for (i32 i = 0; i < ticks; ++i) {
            time.game_time = tick;
            time.hostiles  = hostiles;
            MobContext context{&collisions, &level, true};
            context.villagers = &time;
            world.tick(entity::TickContext{tick++, &context});
            seen.insert(seen.end(), events.begin(), events.end());
            events.clear();
            if (daylight) {
                ++time.day_time;
            }
        }
    }
    [[nodiscard]] usize count(b::VillagerEventKind kind) const {
        return static_cast<usize>(std::ranges::count_if(
            seen, [&](const b::VillagerEvent& e) { return e.kind == kind; }));
    }
};

}  // namespace

TEST_CASE("a villager claims a bed and a bell, sleeps at rest and wakes after day 10",
          "[villager][brain]") {
    Village v;
    v.level.bed(BlockPos{3, 0, 0});
    v.level.place(BlockPos{-4, 0, 0}, "minecraft:bell");
    const auto villager = v.villager(Vec3d{0.5, 0.0, 3.5});
    v.step(60);
    const MobBrain& mind = v.brain_of(villager);
    REQUIRE(mind.memories.pos(b::MemoryType::Home) == BlockPos{3, 0, 1});  // the head
    REQUIRE(mind.memories.pos(b::MemoryType::MeetingPoint) == BlockPos{-4, 0, 0});

    // Rest from 12000: to bed, asleep, last_slept.
    v.time.day_time = 11990;
    i32 ticks       = 0;
    while (!v.state_of(villager).sleeping && ticks < 400) {
        v.step(1, true);
        ++ticks;
    }
    {
        // What the brain was doing, printed if it never lay down.
        auto* held = dynamic_cast<b::BrainGoal*>(
            const_cast<GoalSelector&>(dynamic_cast<Mob*>(v.world.logic(villager))->goals())
                .find("brain"));
        std::vector<std::string_view> running;
        if (held != nullptr) {
            held->brain().running(running);
        }
        std::string doing;
        for (const std::string_view name : running) {
            doing += std::string{name} + " ";
        }
        const auto  walk_to = mind.memories.pos(b::MemoryType::WalkTarget);
        const Vec3d at      = v.body(villager).position;
        INFO("activity " << (held ? b::activity_name(held->brain().current()) : "?")
                         << ", running: " << doing << ", at " << at.x << " " << at.y << " "
                         << at.z << ", walk target "
                         << (walk_to ? std::to_string(walk_to->x) + " " + std::to_string(walk_to->y) +
                                           " " + std::to_string(walk_to->z)
                                     : std::string{"none"}));
        CHECK(v.state_of(villager).sleeping);
    }
    CHECK(v.time.day_time >= 12000);
    CHECK(mind.memories.has(b::MemoryType::LastSlept));
    CHECK(v.brain_of(villager).memories.number(b::MemoryType::LastSlept).value() <= v.tick);

    // Idle from day 10: awake within the schedule's 20-tick gate (measured
    // last_woken at day 19).
    v.time.day_time = 24000 - 5;
    ticks           = 0;
    while (v.state_of(villager).sleeping && ticks < 100) {
        v.step(1, true);
        ++ticks;
    }
    CHECK_FALSE(v.state_of(villager).sleeping);
    const i64 day = v.time.day_time % 24000;
    CHECK(day >= 10);
    CHECK(day <= 10 + 21);
    CHECK(mind.memories.has(b::MemoryType::LastWoken));
}

TEST_CASE("a librarian works at its lectern in working hours", "[villager][brain]") {
    Village v;
    v.level.place(BlockPos{3, 0, 0}, "minecraft:lectern");
    const auto villager = v.villager(Vec3d{0.5, 0.0, 0.5});
    v.time.day_time     = 3000;
    v.step(1400);
    CHECK(v.state_of(villager).profession == Profession::Librarian);
    const MobBrain& mind = v.brain_of(villager);
    CHECK(mind.memories.has(b::MemoryType::JobSite));
    CHECK(mind.memories.has(b::MemoryType::LastWorkedAtPoi));
    // Control: at night the same villager does not work.
    Village night;
    night.level.place(BlockPos{3, 0, 0}, "minecraft:lectern");
    const auto other    = night.villager(Vec3d{0.5, 0.0, 0.5});
    night.time.day_time = 14000;
    night.step(1400);
    CHECK_FALSE(night.brain_of(other).memories.has(b::MemoryType::LastWorkedAtPoi));
}

TEST_CASE("two villagers who meet pass their gossip on, less the transfer loss",
          "[villager][brain][gossip]") {
    const net::Uuid player{0xABCD, 0x1234};
    Village         v;
    const auto      a = v.villager(Vec3d{0.5, 0.0, 0.5});
    const auto      c = v.villager(Vec3d{3.5, 0.0, 0.5});
    v.state_of(a).gossips.put(b::GossipEntry{player, b::GossipType::MinorNegative, 100});
    v.state_of(a).gossips.put(b::GossipEntry{player, b::GossipType::Trading, 20});
    i32 ticks = 0;
    while (v.state_of(c).gossips.empty() && v.state_of(a).gossips.value(player, b::GossipType::MinorNegative) == 100 &&
           ticks < 2400) {
        v.step();
        ++ticks;
        if (!v.state_of(c).gossips.empty()) {
            break;
        }
    }
    // Measured at a bell: 80 (100 - 20) arrived; trading 20 (20 - 20) never does.
    CHECK(v.state_of(c).gossips.value(player, b::GossipType::MinorNegative) == 80);
    CHECK(v.state_of(c).gossips.value(player, b::GossipType::Trading) == 0);
}

namespace {

void frightened_sleepers(Village& v, i32 count, bool slept) {
    v.hostiles.push_back(HostileSight{type_id("minecraft:zombie"), 8.0F});
    for (i32 k = 0; k < count; ++k) {
        const auto h = v.villager(Vec3d{0.5 + 2.0 * k, 0.0, 3.5});
        if (slept) {
            v.brain_of(h).memories.set(b::MemoryType::LastSlept, b::MemoryValue::of_number(0));
        }
    }
    (void)v.world.spawn("minecraft:zombie", Vec3d{2.5, 0.0, 0.5}, net::Uuid{}).value();
}

}  // namespace

TEST_CASE("three villagers who slept recently summon a golem when frightened",
          "[villager][brain][golem]") {
    Village v;
    frightened_sleepers(v, 3, true);
    v.step(600);
    CHECK(v.count(b::VillagerEventKind::SummonGolem) == 1);  // then golem_detected_recently
    const auto golem = std::ranges::find_if(v.seen, [](const b::VillagerEvent& e) {
        return e.kind == b::VillagerEventKind::SummonGolem;
    });
    REQUIRE(golem != v.seen.end());
    CHECK(golem->block.y == 0);  // on the ground

    // Controls, each measured on the real server: no golem.
    Village unslept;
    frightened_sleepers(unslept, 3, false);
    unslept.step(600);
    CHECK(unslept.count(b::VillagerEventKind::SummonGolem) == 0);
    Village two;
    frightened_sleepers(two, 2, true);
    two.step(600);
    CHECK(two.count(b::VillagerEventKind::SummonGolem) == 0);
    Village calm;
    for (i32 k = 0; k < 3; ++k) {
        calm.brain_of(calm.villager(Vec3d{0.5 + 2.0 * k, 0.0, 3.5}))
            .memories.set(b::MemoryType::LastSlept, b::MemoryValue::of_number(0));
    }
    calm.step(600);
    CHECK(calm.count(b::VillagerEventKind::SummonGolem) == 0);
}

TEST_CASE("golem spawn position: on a solid floor under three free blocks",
          "[villager][golem]") {
    Ground                   level{*blocks()};
    math::LegacyRandomSource random{7};
    for (int i = 0; i < 50; ++i) {
        const auto at = b::golem_spawn_position(level, BlockPos{0, 0, 0}, random);
        REQUIRE(at.has_value());
        CHECK(at->y == 0);
        CHECK(std::abs(at->x) <= 8);
        CHECK(std::abs(at->z) <= 8);
    }
    // Over glass there is nowhere.
    Ground glass{*blocks()};
    for (i32 x = -9; x <= 9; ++x) {
        for (i32 z = -9; z <= 9; ++z) {
            glass.place(BlockPos{x, -1, z}, "minecraft:glass");
        }
    }
    CHECK_FALSE(b::golem_spawn_position(glass, BlockPos{0, 0, 0}, random).has_value());
}

namespace {

void couple(Village& v, i32 bread, i32 beds) {
    for (i32 k = 0; k < beds; ++k) {
        v.level.bed(BlockPos{-6 + 3 * k, 0, -6});
    }
    for (const f64 x : {0.5, 3.5}) {
        const auto h = v.villager(Vec3d{x, 0.0, 2.5});
        v.state_of(h).inventory[0] = {"minecraft:bread", bread};
    }
}

}  // namespace

TEST_CASE("breeding: 12 food points and a free bed make a baby", "[villager][brain][breed]") {
    Village v;
    couple(v, 3, 3);
    v.step(1600);
    CHECK(v.count(b::VillagerEventKind::Birth) == 1);
    for (const entity::EntityHandle h : v.world.handles()) {
        if (MobBrain* mind = mob_brain_of(v.world, h); mind != nullptr && mind->villager.active) {
            CHECK(mind->villager.inventory[0].count == 0);  // measured: all 3 bread eaten
            // Set to 6000 at the birth, then counting down a tick at a time
            // like any animal's age (measured: the parents read 3342 later on).
            CHECK(mind->animal.age > 0);
            CHECK(mind->animal.age <= kParentCooldown);
        }
    }
    // Controls: 8 points, and no free bed (two beds for two villagers). The
    // latter still eats (measured: the bread was gone, no baby).
    Village hungry;
    couple(hungry, 2, 3);
    hungry.step(1600);
    CHECK(hungry.count(b::VillagerEventKind::Birth) == 0);
    Village crowded;
    couple(crowded, 3, 2);
    crowded.step(1600);
    CHECK(crowded.count(b::VillagerEventKind::Birth) == 0);
    CHECK(crowded.count(b::VillagerEventKind::NoBed) == 1);
}

TEST_CASE("food points and pockets", "[villager][breed]") {
    VillagerState v;
    v.inventory[0] = {"minecraft:potato", 12};
    v.inventory[1] = {"minecraft:beetroot", 12};
    CHECK(b::food_available(v) == 24);
    CHECK(b::can_breed(v, false));
    CHECK_FALSE(b::can_breed(v, true));
    b::eat_for_breeding(v);
    // Measured: the 12 potatoes (first slot) went, the beetroots stayed.
    CHECK(v.inventory[0].count == 0);
    CHECK(v.inventory[1].count == 12);
    CHECK(v.food_level == 0);
    VillagerState w;
    w.inventory[0] = {"minecraft:bread", 6};
    b::eat_for_breeding(w);
    CHECK(w.inventory[0].count == 3);  // measured: 3 left of 6
    CHECK(b::pocket(w, "minecraft:wheat_seeds", 70) == 0);
    CHECK(b::pocket(w, "minecraft:dirt", 1) == 1);  // not something a villager keeps
}

TEST_CASE("a farmer breaks the ripe wheat round its feet and plants its seeds",
          "[villager][brain][farm]") {
    Village v;
    v.level.place(BlockPos{0, 0, -3}, "minecraft:composter");
    for (i32 x = -1; x <= 1; ++x) {
        v.level.place(BlockPos{x, -1, 1}, "minecraft:farmland");
        v.level.place(BlockPos{x, 0, 1}, "minecraft:wheat", {{"age", "7"}});
    }
    v.level.place(BlockPos{2, -1, 1}, "minecraft:farmland");
    const auto farmer   = v.villager(Vec3d{0.5, 0.0, 0.5});
    VillagerState& s    = v.state_of(farmer);
    s.inventory[0]      = {"minecraft:wheat_seeds", 4};
    v.time.day_time     = 3000;
    v.step(1200);
    CHECK(s.profession == Profession::Farmer);
    CHECK(v.count(b::VillagerEventKind::Harvest) >= 1);
    // Control: a librarian never harvests.
    Village lib;
    lib.level.place(BlockPos{0, 0, -3}, "minecraft:lectern");
    for (i32 x = -1; x <= 1; ++x) {
        lib.level.place(BlockPos{x, -1, 1}, "minecraft:farmland");
        lib.level.place(BlockPos{x, 0, 1}, "minecraft:wheat", {{"age", "7"}});
    }
    (void)lib.villager(Vec3d{0.5, 0.0, 0.5});
    lib.time.day_time = 3000;
    lib.step(1200);
    CHECK(lib.count(b::VillagerEventKind::Harvest) == 0);
}

TEST_CASE("the iron golem's kind and goals", "[golem]") {
    const MobKind* golem = mob_kind("minecraft:iron_golem");
    REQUIRE(golem != nullptr);
    CHECK(golem->category == MobCategory::Misc);
    CHECK(golem->follow_range == 16.0);
    GoalSelector selector;
    install_goals(selector, *golem, -1, kNoQuarry);
    CHECK(selector.find("defend_village") != nullptr);
    CHECK(selector.find("golem_target") != nullptr);
    CHECK(selector.find("melee") != nullptr);
}
