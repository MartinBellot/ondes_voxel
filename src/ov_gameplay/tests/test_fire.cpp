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
/// `per_pick` random ticks each pick: 1 checks one call's rule, and
/// `FireRules::kLavaTicksPerPick` is what the server does.
[[nodiscard]] f64 lava_rate(std::string_view ring, std::string_view roof, i32 roof_dy, int picks,
                            int per_pick = 1) {
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
        for (int k = 0; k < per_pick; ++k) {
            fires += rules().lava_random_tick(level, env, lava, random);
        }
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

TEST_CASE("fire parity: lava's fires per pick, against the vanilla bench", "[fire][parity]") {
    // scripts/measure_fire.py `lava`: 16 enclosed sources per geometry,
    // randomTickSpeed 200 for 1201 ticks with a player present — 938.3 picks
    // per geometry expected. Fires counted and removed the tick after they
    // appear. Vanilla's rates, and the standard error of each (Poisson on the
    // count, over 938.3 picks).
    struct Row {
        std::string_view ring;
        std::string_view roof;
        i32              dy;
        f64              vanilla;
        i64              count;
    };
    constexpr f64  kPicksVanilla = 938.28125;
    const std::vector<Row> rows{
        {"minecraft:stone", "minecraft:oak_planks", 2, 1.3152, 1234},
        {"minecraft:stone", "minecraft:oak_planks", 3, 0.4295, 403},
        {"minecraft:oak_planks", "", 0, 2.4641, 2312},
        {"minecraft:stone", "minecraft:crafting_table", 2, 1.1446, 1074},
    };
    constexpr int kPicks = 40000;
    for (const Row& row : rows) {
        const f64 ours =
            lava_rate(row.ring, row.roof, row.dy, kPicks, FireRules::kLavaTicksPerPick);
        const f64 single = lava_rate(row.ring, row.roof, row.dy, kPicks, 1);
        const f64 se     = std::sqrt(static_cast<f64>(row.count)) / kPicksVanilla;
        INFO(row.ring << " / " << row.roof << " +" << row.dy);
        WARN(row.roof << " +" << row.dy << " ring " << row.ring << ": vanilla " << row.vanilla
                      << " ± " << se << ", ours " << ours << " (" << (ours - row.vanilla) / se
                      << " SE); one tick a pick " << single);
        CHECK(std::abs(ours - row.vanilla) < 3.0 * se);
        // The control: one random tick a pick is many errors away.
        CHECK(std::abs(single - row.vanilla) > 6.0 * se);
    }
    CHECK(lava_rate("minecraft:stone", "minecraft:stone", 2, kPicks,
                    FireRules::kLavaTicksPerPick) == 0.0);
}

TEST_CASE("fire: an entity's counter", "[fire]") {
    // Measured (`rain` campaign, four roofed cows with Fire:200): one point
    // at Fire = 200, 180, ..., 20 — the first on the first tick, ten in all.
    // Four cows in the open rain: the point at 200, then Fire = -1.
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

// ── Parity with the vanilla bench ───────────────────────────────────────────
//
// The arrays below are scripts/measure_fire.py `blocks` on the real 1.20.1
// server (data/vanilla/1.20.1/normalized/fire_blocks.json, 4051 ticks): the
// tick each thing happened, counted from the tick every fire was set. Our code
// runs the same rigs and the two samples are compared with a two-sample
// Kolmogorov-Smirnov test, each against a control that must fit worse
// (briefing, trap 14). The p-values are printed on every run.

namespace {

constexpr i64 kBenchTicks = 4051;
constexpr i64 kNever      = kBenchTicks + 1;

[[nodiscard]] f64 ks_distance(std::vector<i64> a, std::vector<i64> b) {
    std::ranges::sort(a);
    std::ranges::sort(b);
    usize i = 0;
    usize j = 0;
    f64   d = 0.0;
    while (i < a.size() && j < b.size()) {
        const i64 value = std::min(a[i], b[j]);
        while (i < a.size() && a[i] == value) {
            ++i;
        }
        while (j < b.size() && b[j] == value) {
            ++j;
        }
        d = std::max(d, std::abs(static_cast<f64>(i) / static_cast<f64>(a.size()) -
                                 static_cast<f64>(j) / static_cast<f64>(b.size())));
    }
    return d;
}

[[nodiscard]] f64 ks_p(f64 d, usize n, usize m) {
    const f64 en  = std::sqrt(static_cast<f64>(n * m) / static_cast<f64>(n + m));
    const f64 lam = (en + 0.12 + 0.11 / en) * d;
    f64       s   = 0.0;
    for (int k = 1; k <= 100; ++k) {
        s += 2.0 * ((k % 2 == 1) ? 1.0 : -1.0) * std::exp(-2.0 * k * k * lam * lam);
    }
    return std::clamp(s, 0.0, 1.0);
}

/// Vanilla's censored samples: -1 in the bench means "not by the end".
[[nodiscard]] std::vector<i64> censored(std::initializer_list<i64> values) {
    std::vector<i64> out;
    for (const i64 v : values) {
        out.push_back(v < 0 ? kNever : v);
    }
    return out;
}

/// Scaled in time, never beyond the bench's end: the control for a rate.
[[nodiscard]] std::vector<i64> stretched(const std::vector<i64>& v, f64 factor) {
    std::vector<i64> out;
    for (const i64 t : v) {
        out.push_back(t >= kNever ? kNever
                                  : std::min(kNever, static_cast<i64>(static_cast<f64>(t) * factor)));
    }
    return out;
}

/// The `burn` rig: when B stops being B.
[[nodiscard]] i64 burn_once(std::string_view kind, i64 seed) {
    TestEnv    env;
    FireRandom random{seed};
    TickLevel  level;
    level.put({0, -1, 0}, state_of("minecraft:netherrack"));
    const BlockPos b{1, 0, 0};
    const auto     block = state_of(kind);
    level.put(b, block);
    for (const BlockPos wall : {BlockPos{2, 0, 0}, BlockPos{1, 1, 0}, BlockPos{1, -1, 0},
                                BlockPos{1, 0, -1}, BlockPos{1, 0, 1}}) {
        level.put(wall, state_of("minecraft:stone"));
    }
    level.set_block(kFire, rules().state_for(level, kFire));
    level.settle(random);
    for (i64 t = 1; t <= kBenchTicks; ++t) {
        level.advance(t, env, random);
        if (blocks().block_of(level.block_at(b)) != blocks().block_of(block)) {
            return t;
        }
    }
    return kNever;
}

/// The `ignite` rig: when the empty cell between the fire and F catches.
[[nodiscard]] i64 ignite_once(std::string_view kind, i64 seed) {
    TestEnv    env;
    FireRandom random{seed};
    TickLevel  level;
    level.put({0, -1, 0}, state_of("minecraft:netherrack"));
    level.put({1, -1, 0}, state_of("minecraft:grass_block"));
    const BlockPos c{1, 0, 0};
    level.put({2, 0, 0}, state_of(kind));
    for (const BlockPos wall : {BlockPos{3, 0, 0}, BlockPos{2, 1, 0}, BlockPos{2, -1, 0},
                                BlockPos{2, 0, -1}, BlockPos{2, 0, 1}}) {
        level.put(wall, state_of("minecraft:stone"));
    }
    level.set_block(kFire, rules().state_for(level, kFire));
    level.settle(random);
    for (i64 t = 1; t <= kBenchTicks; ++t) {
        level.advance(t, env, random);
        if (rules().is_fire(level.block_at(c))) {
            return t;
        }
    }
    return kNever;
}

}  // namespace

TEST_CASE("fire parity: a fire on stone, against 64 vanilla fires", "[fire][parity]") {
    const std::vector<i64> vanilla = censored(
        {201, 213, 219, 234, 246, 247, 255, 267, 273, 273, 276, 276, 280, 284, 286, 287,
         306, 309, 310, 310, 311, 314, 318, 320, 326, 335, 339, 342, 344, 344, 345, 349,
         372, 378, 398, 404, 408, 411, 417, 419, 420, 427, 445, 468, 471, 474, 481, 492,
         493, 495, 509, 523, 543, 556, 576, 582, 582, 639, 645, 703, 707, 733, 812, 869});
    TestEnv          env;
    std::vector<i64> ours;
    std::vector<i64> dwell;
    for (i64 seed = 1; seed <= 2000; ++seed) {
        FireRandom random{seed};
        TickLevel  level;
        light_on(level, "minecraft:stone", random);
        i64 reached4 = -1;
        i64 t        = 0;
        while (level.block_at(kFire) != registry::kAirState && t < kBenchTicks) {
            ++t;
            level.advance(t, env, random);
            if (reached4 < 0 && rules().age_of(level.block_at(kFire)) == 4) {
                reached4 = t;
            }
        }
        ours.push_back(t);
        if (reached4 >= 0) {
            dwell.push_back(t - reached4);
        }
    }
    // Vanilla: all sixteen logged fires sat at age 4 for exactly one interval
    // (30..39 ticks) and went out at the next tick. So do ours, every one.
    REQUIRE(!dwell.empty());
    for (const i64 d : dwell) {
        REQUIRE(d >= 30);
        REQUIRE(d <= 39);
    }
    const f64 d       = ks_distance(vanilla, ours);
    const f64 p       = ks_p(d, vanilla.size(), ours.size());
    const f64 control = ks_distance(vanilla, stretched(ours, 1.25));
    WARN("stone life: KS " << d << " p " << p << " (vanilla mean 409, n 64); control x1.25 KS "
                           << control);
    CHECK(p > 0.005);
    CHECK(control > d);
}

TEST_CASE("fire parity: a fire on stone in the rain, against 64 vanilla fires", "[fire][parity]") {
    // scripts/measure_fire.py `rain`: the same 64 fires on stone, rain falling
    // on all of them. Several went out on their very first tick, at age 0:
    // the 0.2 + 0.03 * age roll, not the age limit.
    const std::vector<i64> vanilla = censored(
        {30,  31,  32,  34,  36,  36,  38,  38,  63,  65,  69,  71,  72,  73,  73,  74,
         78,  78,  96,  97,  98,  98,  100, 102, 103, 110, 116, 130, 131, 131, 135, 140,
         143, 151, 152, 165, 166, 168, 168, 169, 174, 174, 176, 177, 180, 181, 184, 203,
         216, 243, 253, 271, 273, 276, 280, 282, 283, 322, 344, 360, 378, 397, 403, 419});
    TestEnv wet;
    wet.rain = true;
    TestEnv          dry;
    std::vector<i64> ours;
    std::vector<i64> control;
    for (i64 seed = 1; seed <= 2000; ++seed) {
        for (const bool raining : {true, false}) {
            FireRandom random{seed};
            TickLevel  level;
            light_on(level, "minecraft:stone", random);
            i64 t = 0;
            while (level.block_at(kFire) != registry::kAirState && t < kBenchTicks) {
                ++t;
                level.advance(t, raining ? wet : dry, random);
            }
            (raining ? ours : control).push_back(t);
        }
    }
    const f64 d = ks_distance(vanilla, ours);
    const f64 p = ks_p(d, vanilla.size(), ours.size());
    const f64 c = ks_distance(vanilla, control);
    WARN("stone life in rain: KS " << d << " p " << p << " (vanilla mean 161, n 64); control (dry) KS "
                                   << c);
    CHECK(p > 0.01);
    CHECK(c > d);
}

TEST_CASE("fire parity: the burn odds, block by block", "[fire][parity]") {
    struct Row {
        std::string_view      kind;
        std::vector<i64>      vanilla;
    };
    const std::vector<Row> rows{
        {"minecraft:oak_planks", censored({34, 34, 104, 173, 178, 210, 241, 350, 392, 425, 843,
                                           911, 1155, 1284, 1961, 1982})},
        {"minecraft:oak_log", censored({-1, -1, -1, 168, 175, 190, 207, 1174, 1316, 1356, 1419,
                                        2204, 2374, 2564, 2730, 3745})},
        {"minecraft:white_wool", censored({31, 32, 34, 35, 37, 37, 67, 67, 69, 70, 72, 111, 137,
                                           213, 220, 499})},
        {"minecraft:oak_leaves", censored({30, 31, 32, 34, 65, 69, 99, 102, 103, 136, 139, 144,
                                           146, 176, 315, 471})},
        {"minecraft:bookshelf", censored({31, 37, 65, 73, 110, 141, 168, 172, 230, 374, 445, 494,
                                          542, 547, 593, 629})},
        {"minecraft:hay_block", censored({32, 34, 99, 127, 136, 143, 144, 315, 344, 348, 422,
                                          484, 620, 1115, 1148, 1934})},
        {"minecraft:coal_block", censored({-1, -1, 38, 171, 272, 957, 1008, 1037, 1163, 1438,
                                           1496, 1859, 2032, 2893, 3396, 3948})},
        {"minecraft:dried_kelp_block", censored({31, 31, 32, 33, 34, 35, 38, 38, 70, 72, 103, 212,
                                                 214, 246, 249, 320})},
        {"minecraft:oak_fence", censored({31, 69, 70, 103, 208, 212, 343, 384, 538, 758, 795,
                                          814, 839, 1035, 1062, 1221})},
    };
    for (const Row& row : rows) {
        std::vector<i64> ours;
        for (i64 seed = 1; seed <= 1000; ++seed) {
            ours.push_back(burn_once(row.kind, seed * 7919));
        }
        const f64 d       = ks_distance(row.vanilla, ours);
        const f64 p       = ks_p(d, row.vanilla.size(), ours.size());
        const f64 control = ks_distance(row.vanilla, stretched(ours, 2.0));
        INFO(row.kind);
        WARN(row.kind << ": KS " << d << " p " << p << "; control (half the odds) KS " << control);
        // One threshold for nine tests: the table fits every row at p > 0.005.
        // Wool (0.02) and dried kelp (0.06) sit lowest; the `confirm`
        // campaign re-measures them interleaved — docs/provenance/feu.md § 3.
        CHECK(p > 0.005);
        CHECK(control > d);
    }
    // Stone never burns, in either server.
    CHECK(burn_once("minecraft:stone", 1) == kNever);
}

TEST_CASE("fire parity: ignition beside the odds-5 blocks, pooled", "[fire][parity]") {
    // Planks, logs and coal: ignite odds 5 each, 48 rigs together. The pool
    // fits; the rows disagree with one another (docs/provenance/feu.md § 3).
    const std::vector<i64> vanilla = censored(
        {69,   130,  204,  466,  679,  805,  836,  898,  935,  999,  1450, 1616, 1667, 2801, 3046, 3111,
         -1,   -1,   -1,   924,  940,  1145, 1383, 1479, 2126, 2131, 2239, 2456, 2812, 3280, 3584, 3849,
         72,   73,   101,  167,  347,  356,  377,  400,  406,  452,  1045, 1218, 1543, 1745, 2430, 3461});
    std::vector<i64> ours;
    std::vector<i64> hay;
    for (i64 seed = 1; seed <= 600; ++seed) {
        ours.push_back(ignite_once("minecraft:oak_planks", seed * 104729));
        hay.push_back(ignite_once("minecraft:hay_block", seed * 104729));
    }
    const f64 d       = ks_distance(vanilla, ours);
    const f64 p       = ks_p(d, vanilla.size(), ours.size());
    const f64 control = ks_distance(vanilla, hay);
    WARN("ignite odds 5, pooled: KS " << d << " p " << p << "; control (odds 60) KS " << control);
    CHECK(p > 0.05);
    CHECK(control > d);
    // A crafting table pulls no fire: 0 of 16 in vanilla, none here.
    for (i64 seed = 1; seed <= 20; ++seed) {
        CHECK(ignite_once("minecraft:crafting_table", seed) == kNever);
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
