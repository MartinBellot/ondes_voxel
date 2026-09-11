// Fire, against a hash-map world with its own block-tick queue.
//
// The rules take a `LevelWriter&`, so none of this needs a server. The rigs
// below rebuild scripts/measure_fire.py's benches block for block and run them
// through the same code the server runs; the numbers asserted are the rules'
// own consequences (the wiki's formulas). What vanilla gave on the same rigs is
// in docs/provenance/feu.md.
#include "ov/gameplay/fire.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
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

[[nodiscard]] const FireRules& rules() {
    static const FireRules fire{*packs().blocks, *packs().registries};
    return fire;
}

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks().default_state(*id);
    for (const auto& [key, value] : props) {
        const auto property = blocks().find_property(*id, key);
        REQUIRE(property.has_value());
        bool found = false;
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks().with_property(state, *property, static_cast<u16>(i));
                found = true;
            }
        }
        REQUIRE(found);
    }
    return state;
}

[[nodiscard]] std::string name_of(registry::BlockStateId state) {
    return std::string{blocks().block_name(blocks().block_of(state))};
}

[[nodiscard]] std::string value_of(registry::BlockStateId state, std::string_view property) {
    const auto p = blocks().find_property(blocks().block_of(state), property);
    return p ? std::string{blocks().property_value(state, *p)} : std::string{};
}

/// A world with a real block-tick queue: `advance` runs every tick due, in
/// order, the way the server's drain does, and notifies around each write.
class TickLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return {}; }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
        changed_.push_back(pos);
        ++writes;
    }
    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue,
                       world::TickPriority) override {
        queue_.emplace(std::tuple{now_ + delay, serial_++, pos.x, pos.y, pos.z}, std::string{what});
    }
    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue) const override {
        for (const auto& [k, name] : queue_) {
            if (std::get<2>(k) == pos.x && std::get<3>(k) == pos.y && std::get<4>(k) == pos.z &&
                name == what) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return now_; }

    /// Place without notifying: how a rig is built before it starts.
    void put(BlockPos pos, registry::BlockStateId state) { cells_[key(pos)] = state; }

    /// Notify around everything written, as `WorldTicks::settle` does.
    void settle(FireRandom& random) {
        for (int wave = 0; wave < 64 && !changed_.empty(); ++wave) {
            std::vector<BlockPos> now;
            now.swap(changed_);
            for (const BlockPos pos : now) {
                rules().neighbour_changed(*this, pos, random);
                for (u8 i = 0; i < kDirectionCount; ++i) {
                    rules().neighbour_changed(*this, pos.offset(static_cast<Direction>(i)), random);
                }
            }
        }
    }

    /// Run the queue up to and including `until`.
    void advance(i64 until, FireEnvironment& env, FireRandom& random) {
        while (!queue_.empty() && std::get<0>(queue_.begin()->first) <= until) {
            const auto [k, name] = *queue_.begin();
            queue_.erase(queue_.begin());
            now_ = std::get<0>(k);
            (void)rules().scheduled_tick(*this, env,
                                         BlockPos{std::get<2>(k), std::get<3>(k), std::get<4>(k)},
                                         random);
            settle(random);
        }
        now_ = until;
    }

    [[nodiscard]] usize pending() const noexcept { return queue_.size(); }

    usize writes{0};

private:
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos p) { return {p.x, p.y, p.z}; }
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells_;
    std::map<std::tuple<i64, u64, i32, i32, i32>, std::string>  queue_;
    std::vector<BlockPos>                                       changed_;
    i64                                                         now_{0};
    u64                                                         serial_{0};
};

class TestEnv final : public FireEnvironment {
public:
    bool rules_on{true};
    bool rain{false};
    bool humid{false};
    i32  level{2};

    [[nodiscard]] bool fire_tick() const override { return rules_on; }
    [[nodiscard]] bool is_raining() const override { return rain; }
    [[nodiscard]] bool is_raining_at(BlockPos) const override { return rain; }
    [[nodiscard]] bool increased_burnout(BlockPos) const override { return humid; }
    [[nodiscard]] i32  difficulty() const override { return level; }
    void               prime_tnt(BlockPos pos) override { primed.push_back(pos); }

    std::vector<BlockPos> primed;
};

constexpr BlockPos kFire{0, 0, 0};

/// A fire placed through the rules on `support`, nothing flammable nearby.
void light_on(TickLevel& level, std::string_view support, FireRandom& random) {
    level.put(kFire.offset(Direction::Down), state_of(support));
    level.set_block(kFire, rules().state_for(level, kFire));
    level.settle(random);
}

}  // namespace

TEST_CASE("fire: the flammability table resolves against 1.20.1", "[fire]") {
    REQUIRE(packs().blocks.has_value());
    CHECK(rules().unknown_names() == 0);
    const auto odds = [](std::string_view name) {
        return rules().odds(blocks().block_of(state_of(name)));
    };
    CHECK(odds("minecraft:oak_planks").ignite == 5);
    CHECK(odds("minecraft:oak_planks").burn == 20);
    CHECK(odds("minecraft:oak_log").burn == 5);
    CHECK(odds("minecraft:white_wool").ignite == 30);
    CHECK(odds("minecraft:white_wool").burn == 60);
    CHECK(odds("minecraft:tnt").burn == 100);
    CHECK(odds("minecraft:hay_block").ignite == 60);
    CHECK(odds("minecraft:crimson_planks").ignite == 0);
    CHECK(odds("minecraft:stone").ignite == 0);
    // A waterlogged slab does not burn, and a dry one does.
    CHECK(rules().can_burn(state_of("minecraft:oak_slab")));
    CHECK_FALSE(rules().can_burn(state_of("minecraft:oak_slab", {{"waterlogged", "true"}})));
}

TEST_CASE("fire: the shape a new fire takes", "[fire]") {
    TickLevel level;
    // On stone: the default state, every face false.
    level.put({0, -1, 0}, state_of("minecraft:stone"));
    CHECK(rules().state_for(level, kFire) == state_of("minecraft:fire"));
    // In the air against a plank to the north: north=true.
    TickLevel hanging;
    hanging.put({0, 0, -1}, state_of("minecraft:oak_planks"));
    const auto shaped = rules().state_for(hanging, kFire);
    CHECK(value_of(shaped, "north") == "true");
    CHECK(value_of(shaped, "south") == "false");
    CHECK(rules().placement(hanging, kFire).has_value());
    // Over soul soil: soul fire.
    TickLevel soul;
    soul.put({0, -1, 0}, state_of("minecraft:soul_soil"));
    CHECK(name_of(rules().state_for(soul, kFire)) == "minecraft:soul_fire");
    // In the open air, nothing to hold it: refused.
    TickLevel open;
    CHECK_FALSE(rules().placement(open, kFire).has_value());
    // Not into a block.
    CHECK_FALSE(rules().placement(level, {0, -1, 0}).has_value());
}

TEST_CASE("fire: a written fire asks for one tick, 30 to 39 away", "[fire]") {
    FireRandom random{7};
    TickLevel  level;
    light_on(level, "minecraft:stone", random);
    CHECK(level.pending() == 1);
    CHECK(level.has_scheduled_tick(kFire, "minecraft:fire", world::TickQueue::Block));
    for (int i = 0; i < 2000; ++i) {
        const i64 d = FireRules::tick_delay(random);
        REQUIRE(d >= 30);
        REQUIRE(d <= 39);
    }
}

TEST_CASE("fire: doFireTick off freezes a fire", "[fire]") {
    FireRandom random{11};
    TestEnv    env;
    env.rules_on = false;
    TickLevel level;
    light_on(level, "minecraft:stone", random);
    level.advance(20000, env, random);
    CHECK(name_of(level.block_at(kFire)) == "minecraft:fire");
    CHECK(rules().age_of(level.block_at(kFire)) == 0);
    CHECK(level.pending() == 1);  // still rescheduling itself
}

TEST_CASE("fire: netherrack burns for ever, stone goes out after age 3", "[fire]") {
    TestEnv env;
    for (u64 seed = 1; seed <= 20; ++seed) {
        FireRandom random{static_cast<i64>(seed)};
        TickLevel  forever;
        light_on(forever, "minecraft:netherrack", random);
        forever.advance(20000, env, random);
        REQUIRE(name_of(forever.block_at(kFire)) == "minecraft:fire");
        CHECK(rules().age_of(forever.block_at(kFire)) == 15);

        TickLevel stone;
        light_on(stone, "minecraft:stone", random);
        stone.advance(20000, env, random);
        CHECK(stone.block_at(kFire) == registry::kAirState);
    }
}

TEST_CASE("fire: losing its support puts a fire out", "[fire]") {
    FireRandom random{3};
    TickLevel  level;
    light_on(level, "minecraft:stone", random);
    level.set_block({0, -1, 0}, registry::kAirState);
    level.settle(random);
    CHECK(level.block_at(kFire) == registry::kAirState);
}

TEST_CASE("fire: soul soil turns a fire into soul fire", "[fire]") {
    FireRandom random{5};
    TickLevel  level;
    light_on(level, "minecraft:stone", random);
    level.set_block({0, -1, 0}, state_of("minecraft:soul_soil"));
    level.settle(random);
    CHECK(name_of(level.block_at(kFire)) == "minecraft:soul_fire");
}

TEST_CASE("fire: a plank beside a fire is eaten, and never spreads from its walls", "[fire]") {
    TestEnv env;
    int     gone = 0;
    for (u64 seed = 1; seed <= 40; ++seed) {
        FireRandom random{static_cast<i64>(seed)};
        TickLevel  level;
        // The bench's `burn` rig: B east of a fire on netherrack, stone on
        // B's five other faces.
        level.put({0, -1, 0}, state_of("minecraft:netherrack"));
        const BlockPos b{1, 0, 0};
        level.put(b, state_of("minecraft:oak_planks"));
        for (const BlockPos wall : {BlockPos{2, 0, 0}, BlockPos{1, 1, 0}, BlockPos{1, -1, 0},
                                    BlockPos{1, 0, -1}, BlockPos{1, 0, 1}}) {
            level.put(wall, state_of("minecraft:stone"));
        }
        level.set_block(kFire, rules().state_for(level, kFire));
        level.settle(random);
        level.advance(8000, env, random);
        const std::string now = name_of(level.block_at(b));
        if (now != "minecraft:oak_planks") {
            ++gone;
            CHECK((now == "minecraft:air" || now == "minecraft:fire"));
        }
        // Nothing else was ever set alight: only B touches the fire.
        CHECK(name_of(level.block_at({-1, 0, 0})) == "minecraft:air");
        CHECK(name_of(level.block_at({0, 1, 0})) == "minecraft:air");
    }
    // p = 20/300 per fire tick, 34.5 ticks apart: eaten within 8000 ticks
    // with probability 1 - (14/15)^231, all forty.
    CHECK(gone == 40);
}

TEST_CASE("fire: a burnt TNT is primed", "[fire]") {
    TestEnv env;
    int     primed = 0;
    for (u64 seed = 1; seed <= 10; ++seed) {
        FireRandom random{static_cast<i64>(seed)};
        TickLevel  level;
        level.put({0, -1, 0}, state_of("minecraft:netherrack"));
        level.put({1, 0, 0}, state_of("minecraft:tnt"));
        level.set_block(kFire, rules().state_for(level, kFire));
        level.settle(random);
        env.primed.clear();
        level.advance(4000, env, random);
        primed += static_cast<int>(env.primed.size());
    }
    CHECK(primed == 10);
}

TEST_CASE("fire: rain puts out fires on stone and not on netherrack", "[fire]") {
    TestEnv wet;
    wet.rain = true;
    TestEnv dry;
    i64     wet_total = 0;
    i64     dry_total = 0;
    for (u64 seed = 1; seed <= 200; ++seed) {
        for (const bool raining : {true, false}) {
            FireRandom random{static_cast<i64>(seed)};
            TickLevel  level;
            light_on(level, "minecraft:stone", random);
            i64 t = 0;
            while (level.block_at(kFire) != registry::kAirState && t < 20000) {
                t += 1;
                level.advance(t, raining ? wet : dry, random);
            }
            (raining ? wet_total : dry_total) += t;
        }
        FireRandom random{static_cast<i64>(seed)};
        TickLevel  forever;
        light_on(forever, "minecraft:netherrack", random);
        forever.advance(5000, wet, random);
        REQUIRE(name_of(forever.block_at(kFire)) == "minecraft:fire");
    }
    // Dry: the fire goes out on the tick its age passes 3, ~450 ticks. Wet: at
    // least one in five of its ticks drowns it, ~160. The ratio vanilla gives
    // is the rain campaign's, docs/provenance/feu.md; this only says "shorter".
    CHECK(wet_total * 2 < dry_total);
}

namespace {

/// A lava source ringed at its own level, and a roof, as the bench's rigs.
[[nodiscard]] f64 lava_rate(std::string_view ring, std::string_view roof, i32 roof_dy, int picks) {
    TestEnv    env;
    FireRandom random{99};
    TickLevel  level;
    const BlockPos lava{0, 0, 0};
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            level.put(lava.offset(dx, -1, dz), state_of("minecraft:grass_block"));
            level.put(lava.offset(dx, 0, dz), state_of(ring));
            if (!roof.empty()) {
                level.put(lava.offset(dx, roof_dy, dz), state_of(roof));
            }
        }
    }
    level.put(lava, state_of("minecraft:lava"));
    i64 fires = 0;
    for (int i = 0; i < picks; ++i) {
        fires += rules().lava_random_tick(level, env, lava, random);
        // Removed at once, as the measurement's tick function does.
        for (i32 dy = 1; dy <= 3; ++dy) {
            for (i32 dz = -2; dz <= 2; ++dz) {
                for (i32 dx = -2; dx <= 2; ++dx) {
                    if (rules().is_fire(level.block_at(lava.offset(dx, dy, dz)))) {
                        level.put(lava.offset(dx, dy, dz), registry::kAirState);
                    }
                }
            }
        }
    }
    return static_cast<f64>(fires) / static_cast<f64>(picks);
}

}  // namespace

TEST_CASE("fire: lava's random tick, geometry by geometry", "[fire]") {
    constexpr int kPicks = 60000;
    // Roof two above: every upward step lands under a plank — 2/3.
    CHECK(std::abs(lava_rate("minecraft:stone", "minecraft:oak_planks", 2, kPicks) - 2.0 / 3.0) <
          0.01);
    // Roof three above, 3x3: only the second step reaches it, inside 7/9 x 7/9.
    CHECK(std::abs(lava_rate("minecraft:stone", "minecraft:oak_planks", 3, kPicks) -
                   49.0 / 243.0) < 0.01);
    // A stone roof lights nothing.
    CHECK(lava_rate("minecraft:stone", "minecraft:stone", 2, kPicks) == 0.0);
}

TEST_CASE("fire: an entity's counter", "[fire]") {
    SECTION("burning in the open: one point on every twentieth") {
        EntityFire fire{.remaining = 200};
        std::vector<int> hits;
        for (int t = 0; t < 200; ++t) {
            const FireDamage d = tick_entity_fire(fire, {});
            if (d.on_fire > 0.0F) {
                hits.push_back(t);
            }
        }
        CHECK(hits.size() == 10);
        CHECK(hits.front() == 0);
        CHECK(hits[1] == 20);
        CHECK(fire.remaining == -1);  // out, and reset to the grace
    }
    SECTION("a mob in fire lights on its first tick, and the counter holds") {
        EntityFire fire{};
        const FireContact in{.in_fire = true};
        const FireDamage  first = tick_entity_fire(fire, in);
        CHECK(first.in_fire == 1.0F);
        CHECK(fire.remaining == 160);
        (void)tick_entity_fire(fire, in);
        CHECK(fire.remaining == 160);
    }
    SECTION("a player waits twenty ticks in fire") {
        EntityFire fire{.remaining = -20, .immune_ticks = 20};
        const FireContact in{.in_fire = true};
        int lit_at = -1;
        for (int t = 0; t < 40 && lit_at < 0; ++t) {
            (void)tick_entity_fire(fire, in);
            if (fire.on_fire()) {
                lit_at = t;
            }
        }
        CHECK(lit_at == 19);
    }
    SECTION("water puts it out") {
        EntityFire fire{.remaining = 200};
        (void)tick_entity_fire(fire, FireContact{.wet = true, .in_water = true});
        CHECK_FALSE(fire.on_fire());
        CHECK(fire.remaining == -1);
    }
    SECTION("lava: fifteen seconds and four points, no on_fire") {
        EntityFire fire{};
        const FireDamage d = tick_entity_fire(fire, FireContact{.in_lava = true});
        CHECK(d.lava == 4.0F);
        CHECK(d.on_fire == 0.0F);
        CHECK(fire.remaining == 300);
    }
    SECTION("a campfire hurts and does not light") {
        EntityFire fire{};
        const FireDamage d = tick_entity_fire(fire, FireContact{.campfire = 2});
        CHECK(d.in_fire == 2.0F);
        CHECK_FALSE(fire.on_fire());
    }
    CHECK(fire_resistance_blocks(DamageKind::OnFire));
    CHECK(fire_resistance_blocks(DamageKind::Lava));
    CHECK(fire_resistance_blocks(DamageKind::InFire));
    CHECK_FALSE(fire_resistance_blocks(DamageKind::Fall));
}

TEST_CASE("fire: the sun, by the draw", "[fire]") {
    FireRandom random{1234};
    constexpr int kDraws = 200000;
    int noon = 0;
    int dim  = 0;
    for (int i = 0; i < kDraws; ++i) {
        noon += sun_burns(15, 0, true, false, random) ? 1 : 0;
        dim += sun_burns(13, 2, true, false, random) ? 1 : 0;
    }
    // (1.0 - 0.4) * 2 / 30 = 0.04 at 15; at 13, f = 0.6190, 0.01460.
    CHECK(std::abs(static_cast<f64>(noon) / kDraws - 0.04) < 0.002);
    CHECK(std::abs(static_cast<f64>(dim) / kDraws - 0.0146) < 0.0015);
    CHECK_FALSE(sun_burns(15, 4, true, false, random));   // night
    CHECK_FALSE(sun_burns(10, 0, true, false, random));   // 0.4 < 0.5
    for (int i = 0; i < 1000; ++i) {
        REQUIRE_FALSE(sun_burns(15, 0, true, true, random));   // wet
        REQUIRE_FALSE(sun_burns(15, 0, false, false, random)); // no sky
    }
}

TEST_CASE("fire: a campfire cooks in 600 ticks and cools by two", "[fire]") {
    std::array<CampfireSlot, 4> slots{};
    slots[0] = CampfireSlot{.item = 1, .progress = 0, .total = 600};
    int done_at = -1;
    for (int t = 1; t <= 700 && done_at < 0; ++t) {
        if (tick_campfire(slots, true) & 1U) {
            done_at = t;
        }
    }
    CHECK(done_at == 600);
    slots[1] = CampfireSlot{.item = 1, .progress = 10, .total = 600};
    (void)tick_campfire(slots, false);
    CHECK(slots[1].progress == 8);
}
