// Status effects, against what a real 1.20.1 server did.
//
// Every table below was measured by scripts/measure_effects.py and is written
// down, with its campaign, in docs/provenance/effets.md. The periodic table is
// the one worth reading first: 522 cows, each summoned with one effect at an
// exact duration in ticks, counted once the effect had run out. This file
// replays every one of them.
#include "ov/gameplay/effects.hpp"
#include "ov/gameplay/food.hpp"
#include "ov/gameplay/mob_body.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>

using namespace ov;
using namespace ov::gameplay;

namespace {

/// A mob, as the effects see one: health, a max-health attribute, the damage
/// window of damage.hpp, and absorption.
class Mob final : public EffectTarget {
public:
    Mob(f64 max, f32 health, Body body = Body::Ordinary) : body_{body} {
        attributes_.own(Attribute::MaxHealth, max);
        state.max_health = static_cast<f32>(max);
        state.health     = health;
    }

    [[nodiscard]] Body body() const noexcept override { return body_; }
    [[nodiscard]] f32  health() const noexcept override { return state.health; }
    [[nodiscard]] f32  max_health() const noexcept override {
        return static_cast<f32>(*attributes_.value(Attribute::MaxHealth));
    }
    void heal(f32 amount) override { state.health = std::min(max_health(), state.health + amount); }
    void hurt(DamageKind kind, f32 amount, const DamageConstants& constants) override {
        (void)apply_damage(state, kind, amount, constants);
    }
    void exhaust(f32 amount) override { exhausted += amount; }
    void feed(i32 nutrition, f32 modifier) override {
        food.food       = std::min(food.food + nutrition, 20);
        food.saturation = std::min(food.saturation + static_cast<f32>(nutrition) * modifier * 2.0F,
                                   static_cast<f32>(food.food));
    }
    [[nodiscard]] f32 absorption() const noexcept override { return state.absorption; }
    void              set_absorption(f32 amount) override { state.absorption = amount; }
    [[nodiscard]] AttributeMap* attributes() noexcept override { return &attributes_; }
    void clamp_health() override { state.health = std::min(state.health, max_health()); }

    /// One tick in the game's order: the window counts down, then the
    /// effects act.
    void tick() {
        tick_health(state, DamageConstants{});
        effects.tick(*this);
    }

    HealthState   state{};
    FoodState     food{};
    f32           exhausted{0.0F};
    ActiveEffects effects{};

private:
    Body         body_;
    AttributeMap attributes_{};
};

[[nodiscard]] EffectInstance of(Effect effect, i32 duration, u8 amplifier = 0) {
    EffectInstance instance;
    instance.effect    = effect;
    instance.duration  = duration;
    instance.amplifier = amplifier;
    return instance;
}

/// Run until the effect is gone, and a little longer.
void run_out(Mob& mob) {
    for (i32 i = 0; i < 400 && !mob.effects.empty(); ++i) {
        mob.tick();
    }
    for (i32 i = 0; i < 5; ++i) {
        mob.tick();
    }
}

// ── The periodic table ──────────────────────────────────────────────────────

constexpr std::array<i32, 29> kDurations{1,  2,  3,  4,  5,  6,  7,  9,   10,  11,
                                         12, 13, 19, 20, 21, 24, 25, 26,  39,  40,
                                         41, 49, 50, 51, 99, 100, 101, 150, 151};

using Row = std::array<i32, 29>;

// Health gained (regeneration) or lost (poison, wither) over the whole
// effect, per amplifier 0..5, per duration above. Cows at 500 of 1000.
constexpr std::array<Row, 6> kRegeneration{{
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 3},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4, 6, 6},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 8, 8, 8, 12, 12},
    {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 4, 6, 6, 6, 8, 8, 8, 16, 16, 16, 25, 25},
    {0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 6, 6, 7, 8, 8, 8, 13, 13, 13, 16, 16, 17, 33, 33, 33, 50, 50},
    {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 19, 20, 21, 24, 25, 26, 39, 40, 41, 49, 50, 51, 99, 100,
     101, 150, 151},
}};

constexpr std::array<Row, 6> kPoison{{
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4, 6, 6},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 8, 8, 8, 12, 12},
    {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 8, 8, 8, 13, 13},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 5, 9, 9, 9, 13, 13},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 5, 6, 10, 10, 11, 15, 16},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 5, 6, 10, 10, 11, 15, 16},
}};

constexpr std::array<Row, 6> kWither{{
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 5, 5, 7, 7},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 4, 4, 4, 5, 5, 9, 10, 10, 15, 15},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5, 10, 10, 10, 15, 15},
    {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 10, 10, 10, 15, 15},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 5, 6, 10, 10, 11, 15, 16},
}};

[[nodiscard]] i32 replay(Effect effect, u8 amplifier, i32 duration) {
    Mob            mob{1000.0, 500.0F};
    EffectInstance instance = of(effect, duration, amplifier);
    instance.visible        = false;
    REQUIRE(mob.effects.add(instance, mob) == AddResult::Added);
    run_out(mob);
    return static_cast<i32>(mob.state.health - 500.0F);
}

}  // namespace

TEST_CASE("the periodic effects, cow by cow", "[gameplay][effects][parity]") {
    usize agree = 0;
    usize total = 0;
    for (u8 amp = 0; amp < 6; ++amp) {
        for (usize i = 0; i < kDurations.size(); ++i) {
            const i32 d = kDurations[i];
            const i32 regen  = replay(Effect::Regeneration, amp, d);
            const i32 poison = -replay(Effect::Poison, amp, d);
            const i32 wither = -replay(Effect::Wither, amp, d);
            CHECK(regen == kRegeneration[amp][i]);
            CHECK(poison == kPoison[amp][i]);
            CHECK(wither == kWither[amp][i]);
            agree += static_cast<usize>(regen == kRegeneration[amp][i]) +
                     static_cast<usize>(poison == kPoison[amp][i]) +
                     static_cast<usize>(wither == kWither[amp][i]);
            total += 3;
        }
    }
    CHECK(total == 522);
    CHECK(agree == 522);
}

TEST_CASE("the interval rule by itself", "[gameplay][effects]") {
    CHECK(acts_on(Effect::Regeneration, 0, 50));
    CHECK(!acts_on(Effect::Regeneration, 0, 49));
    CHECK(acts_on(Effect::Poison, 1, 12));
    CHECK(acts_on(Effect::Wither, 5, 7));  // 40 >> 5 = 1
    CHECK(acts_on(Effect::Poison, 5, 3));  // 25 >> 5 = 0: every tick
    // Java masks the shift: amplifier 32 is amplifier 0 again.
    CHECK(!acts_on(Effect::Regeneration, 32, 49));
    CHECK(acts_on(Effect::Hunger, 0, 7));
    CHECK(!acts_on(Effect::Speed, 0, 20));
}

TEST_CASE("poison stops at one, wither does not", "[gameplay][effects][parity]") {
    // Three cows at 3 health under poison I, IV and VI for 300 ticks: all at
    // exactly 1.0. The one under wither IV died.
    for (const u8 amp : {u8{0}, u8{3}, u8{5}}) {
        Mob mob{1000.0, 3.0F};
        REQUIRE(mob.effects.add(of(Effect::Poison, 300, amp), mob) == AddResult::Added);
        run_out(mob);
        CHECK(mob.state.health == 1.0F);
    }
    Mob mob{1000.0, 3.0F};
    REQUIRE(mob.effects.add(of(Effect::Wither, 300, 3), mob) == AddResult::Added);
    run_out(mob);
    CHECK(mob.state.dead);
}

TEST_CASE("an infinite effect counts its age instead", "[gameplay][effects][parity]") {
    // Four cows under infinite regeneration I healed 8 each in 20.05 s: the
    // finite rate, one per fifty ticks.
    Mob mob{1000.0, 500.0F};
    REQUIRE(mob.effects.add(of(Effect::Regeneration, kInfiniteDuration), mob) == AddResult::Added);
    for (i32 i = 0; i < 400; ++i) {
        mob.tick();
    }
    CHECK(mob.state.health == 508.0F);
    CHECK(mob.effects.has(Effect::Regeneration));
}

TEST_CASE("instant health and damage, and the undead inverted", "[gameplay][effects][parity]") {
    for (u8 amp = 0; amp < 6; ++amp) {
        Mob cow{1000.0, 500.0F};
        CHECK(cow.effects.add(of(Effect::InstantHealth, 20, amp), cow) == AddResult::Applied);
        CHECK(cow.state.health == 500.0F + static_cast<f32>(4 << amp));
        CHECK(cow.effects.empty());

        Mob hurt{1000.0, 500.0F};
        (void)hurt.effects.add(of(Effect::InstantDamage, 20, amp), hurt);
        CHECK(hurt.state.health == 500.0F - static_cast<f32>(6 << amp));

        // Zombies at midnight — at noon they burned and came back one off.
        Mob zombie{1000.0, 500.0F, EffectTarget::Body::Undead};
        (void)zombie.effects.add(of(Effect::InstantHealth, 20, amp), zombie);
        CHECK(zombie.state.health == 500.0F - static_cast<f32>(6 << amp));
        Mob healed{1000.0, 500.0F, EffectTarget::Body::Undead};
        (void)healed.effects.add(of(Effect::InstantDamage, 20, amp), healed);
        CHECK(healed.state.health == 500.0F + static_cast<f32>(4 << amp));
    }
}

TEST_CASE("who refuses what", "[gameplay][effects][parity]") {
    // `effect give` of every effect to a zombie, a skeleton, a spider, a cow.
    usize refused_by_undead    = 0;
    usize refused_by_arthropod = 0;
    for (i32 id = 1; id <= 33; ++id) {
        const Effect effect = *effect_from_id(id);
        refused_by_undead += accepts(EffectTarget::Body::Undead, effect) ? 0 : 1;
        refused_by_arthropod += accepts(EffectTarget::Body::Arthropod, effect) ? 0 : 1;
        CHECK(accepts(EffectTarget::Body::Ordinary, effect));
    }
    CHECK(refused_by_undead == 2);
    CHECK(!accepts(EffectTarget::Body::Undead, Effect::Regeneration));
    CHECK(!accepts(EffectTarget::Body::Undead, Effect::Poison));
    CHECK(refused_by_arthropod == 1);
    Mob zombie{20.0, 20.0F, EffectTarget::Body::Undead};
    CHECK(zombie.effects.add(of(Effect::Poison, 100), zombie) == AddResult::Immune);
}

// ── mobs-4 ──
TEST_CASE("a species' body, and what speed makes of its walk", "[gameplay][effects]") {
    // The three species measured by `effect give` (§ 7), then the wiki's lists.
    CHECK(effect_body("minecraft:zombie") == EffectTarget::Body::Undead);
    CHECK(effect_body("minecraft:skeleton") == EffectTarget::Body::Undead);
    CHECK(effect_body("minecraft:spider") == EffectTarget::Body::Arthropod);
    CHECK(effect_body("minecraft:cow") == EffectTarget::Body::Ordinary);
    CHECK(effect_body("minecraft:wither_skeleton") == EffectTarget::Body::Undead);
    CHECK(effect_body("minecraft:cave_spider") == EffectTarget::Body::Arthropod);
    CHECK(effect_body("minecraft:creeper") == EffectTarget::Body::Ordinary);
    CHECK(effect_body("") == EffectTarget::Body::Ordinary);

    // The walk law is quadratic in the attribute: Speed I (+20 %) walks 1.44
    // times as fast, Slowness I (-15 %) 0.7225 times.
    using Catch::Matchers::WithinRel;
    CHECK_THAT(walk_factor(0.23 * 1.2, 0.23), WithinRel(1.44, 1e-12));
    CHECK_THAT(walk_factor(0.23 * 0.85, 0.23), WithinRel(0.7225, 1e-12));
    CHECK(walk_factor(0.23, 0.23) == 1.0);
    CHECK(walk_factor(0.5, 0.0) == 1.0);
}

TEST_CASE("resistance takes a fifth per level", "[gameplay][effects][parity]") {
    struct Cell {
        i32              amp;
        f32              hit;
        DamageKind       kind;
        f32              lost;
    };
    const Cell cells[] = {
        {0, 10.0F, DamageKind::Generic, 8.0F},     {0, 7.0F, DamageKind::Generic, 5.6F},
        {0, 3.0F, DamageKind::Generic, 2.4F},      {0, 10.0F, DamageKind::OutOfWorld, 10.0F},
        {0, 10.0F, DamageKind::Magic, 8.0F},       {0, 10.0F, DamageKind::Starve, 10.0F},
        {1, 10.0F, DamageKind::Generic, 6.0F},     {1, 7.0F, DamageKind::Generic, 4.2F},
        {1, 3.0F, DamageKind::Generic, 1.8F},      {1, 10.0F, DamageKind::Magic, 6.0F},
        {2, 10.0F, DamageKind::Generic, 4.0F},     {2, 7.0F, DamageKind::Generic, 2.8F},
        {2, 3.0F, DamageKind::Generic, 1.2F},      {3, 10.0F, DamageKind::Generic, 2.0F},
        {3, 7.0F, DamageKind::Generic, 1.4F},      {3, 3.0F, DamageKind::Generic, 0.6F},
        {4, 10.0F, DamageKind::Generic, 0.0F},     {5, 7.0F, DamageKind::Generic, 0.0F},
        {5, 10.0F, DamageKind::Magic, 0.0F},       {5, 10.0F, DamageKind::OutOfWorld, 10.0F},
        {5, 10.0F, DamageKind::Starve, 10.0F},
    };
    for (const Cell& cell : cells) {
        HealthState state;
        state.max_health = 1000.0F;
        state.health     = 500.0F;
        (void)apply_damage(state, cell.kind, cell.hit, DamageConstants{},
                           DamageMitigation{.resistance = cell.amp});
        CHECK_THAT(static_cast<f64>(500.0F - state.health),
                   Catch::Matchers::WithinAbs(static_cast<f64>(cell.lost), 1e-4));
    }
}

TEST_CASE("absorption, before and after the window", "[gameplay][effects][parity]") {
    // Cows at 50 of 100 with absorption II (8): a hit of 3, then a hit of 30
    // thirty ticks later ended at 25; six ticks later, inside the window, at
    // 28. Without absorption: 17 and 20.
    const auto run = [](bool absorb, i32 gap) {
        Mob mob{100.0, 50.0F};
        if (absorb) {
            (void)mob.effects.add(of(Effect::Absorption, 1200, 1), mob);
            CHECK(mob.state.absorption == 8.0F);
        }
        (void)apply_damage(mob.state, DamageKind::Generic, 3.0F, DamageConstants{});
        for (i32 i = 0; i < gap; ++i) {
            mob.tick();
        }
        (void)apply_damage(mob.state, DamageKind::Generic, 30.0F, DamageConstants{});
        return mob.state.health;
    };
    CHECK(run(true, 30) == 25.0F);
    CHECK(run(true, 6) == 28.0F);
    CHECK(run(false, 30) == 17.0F);
    CHECK(run(false, 6) == 20.0F);

    // Cleared, the pool goes: measured 0 after a clear at every amplifier.
    Mob mob{100.0, 50.0F};
    (void)mob.effects.add(of(Effect::Absorption, 1200, 3), mob);
    CHECK(mob.state.absorption == 16.0F);
    (void)apply_damage(mob.state, DamageKind::Generic, 3.0F, DamageConstants{});
    CHECK(mob.state.absorption == 13.0F);
    CHECK(mob.state.health == 50.0F);
    REQUIRE(mob.effects.remove(Effect::Absorption, mob));
    CHECK(mob.state.absorption == 0.0F);
}

TEST_CASE("health boost adds to the maximum, and clamps on its way out",
          "[gameplay][effects][parity]") {
    // A cow at 10 of 10: boost II made the maximum 18 and left the health at
    // 10; healed to 18; cleared, 10 of 10.
    Mob cow{10.0, 10.0F};
    (void)cow.effects.add(of(Effect::HealthBoost, 1200, 1), cow);
    CHECK(cow.max_health() == 18.0F);
    CHECK(cow.state.health == 10.0F);
    cow.heal(32.0F);
    CHECK(cow.state.health == 18.0F);
    REQUIRE(cow.effects.remove(Effect::HealthBoost, cow));
    CHECK(cow.max_health() == 10.0F);
    CHECK(cow.state.health == 10.0F);
}

TEST_CASE("the nine modifiers, bit for bit", "[gameplay][effects][parity]") {
    struct Row {
        Effect    effect;
        u8        amp;
        Attribute attribute;
        f64       base;
        f64       amount;
        f64       total;
    };
    // `data get entity … Attributes` and `attribute … get` under each effect.
    const Row rows[] = {
        {Effect::Speed, 0, Attribute::MovementSpeed, 0.23000000417232513, 0.20000000298023224,
         0.2760000056922436},
        {Effect::Speed, 1, Attribute::MovementSpeed, 0.23000000417232513, 0.4000000059604645,
         0.32200000721216204},
        {Effect::Speed, 4, Attribute::MovementSpeed, 0.23000000417232513, 1.0000000149011612,
         0.4600000117719174},
        {Effect::Slowness, 0, Attribute::MovementSpeed, 0.23000000417232513, -0.15000000596046448,
         0.1955000021755695},
        {Effect::Slowness, 1, Attribute::MovementSpeed, 0.23000000417232513, -0.30000001192092896,
         0.16100000017881388},
        {Effect::Slowness, 4, Attribute::MovementSpeed, 0.23000000417232513, -0.7500000298023224,
         0.05749999418854701},
        {Effect::Haste, 0, Attribute::AttackSpeed, 4.0, 0.10000000149011612, 4.4000000059604645},
        {Effect::Haste, 1, Attribute::AttackSpeed, 4.0, 0.20000000298023224, 4.800000011920929},
        {Effect::Haste, 4, Attribute::AttackSpeed, 4.0, 0.5000000074505806, 6.000000029802322},
        {Effect::MiningFatigue, 0, Attribute::AttackSpeed, 4.0, -0.10000000149011612,
         3.5999999940395355},
        {Effect::MiningFatigue, 1, Attribute::AttackSpeed, 4.0, -0.20000000298023224,
         3.199999988079071},
        {Effect::MiningFatigue, 4, Attribute::AttackSpeed, 4.0, -0.5000000074505806,
         1.9999999701976776},
        {Effect::Strength, 0, Attribute::AttackDamage, 3.0, 3.0, 6.0},
        {Effect::Strength, 4, Attribute::AttackDamage, 3.0, 15.0, 18.0},
        {Effect::Weakness, 0, Attribute::AttackDamage, 3.0, -4.0, 0.0},
        {Effect::Weakness, 4, Attribute::AttackDamage, 1.0, -20.0, 0.0},
        {Effect::HealthBoost, 0, Attribute::MaxHealth, 1000.0, 4.0, 1004.0},
        {Effect::HealthBoost, 4, Attribute::MaxHealth, 20.0, 20.0, 40.0},
        {Effect::Luck, 1, Attribute::Luck, 0.0, 2.0, 2.0},
        {Effect::Unluck, 4, Attribute::Luck, 0.0, -5.0, -5.0},
        // The bot, with a base of 0.1 as the rig had written it.
        {Effect::Speed, 1, Attribute::MovementSpeed, 0.1, 0.4000000059604645, 0.14000000059604645},
        {Effect::Slowness, 4, Attribute::MovementSpeed, 0.1, -0.7500000298023224,
         0.024999997019767763},
    };
    for (const Row& row : rows) {
        Mob mob{20.0, 20.0F};
        mob.attributes()->own(row.attribute, row.base);
        REQUIRE(mob.effects.add(of(row.effect, 600, row.amp), mob) == AddResult::Added);
        const AttributeInstance* attribute = mob.attributes()->get(row.attribute);
        REQUIRE(attribute != nullptr);
        REQUIRE(attribute->modifiers().size() == 1);
        CHECK(attribute->modifiers()[0].amount == row.amount);
        CHECK(attribute->value() == row.total);
        REQUIRE(mob.effects.remove(row.effect, mob));
        CHECK(attribute->modifiers().empty());
    }

    // Exactly nine effects carry one.
    usize with = 0;
    for (i32 id = 1; id <= 33; ++id) {
        with += effect_info(*effect_from_id(id)).modifier ? 1 : 0;
    }
    CHECK(with == 9);

    std::array<char, 48> name{};
    const usize length = modifier_name(Effect::Speed, 2, name.data(), name.size());
    CHECK(std::string_view{name.data(), length} == "effect.minecraft.speed 2");
}

TEST_CASE("the replacement rules, case by case", "[gameplay][effects][parity]") {
    struct Step {
        i32 duration;
        u8  amp;
    };
    struct Case {
        std::array<Step, 3> steps;
        usize               count;
        u8                  top_amp;
        i32                 top_duration;
        i32                 hidden_amp;  // -1: nothing hidden
        i32                 hidden_duration;
    };
    constexpr i32 kInf  = kInfiniteDuration;
    const Case    cases[] = {
        // stronger shorter: speed III 5 s over speed I 30 s → hidden.
        {{{{600, 0}, {100, 2}}}, 2, 2, 100, 0, 600},
        // weaker longer: speed I 30 s under speed III 5 s → hidden.
        {{{{100, 2}, {600, 0}}}, 2, 2, 100, 0, 600},
        // stronger longer: the weaker is dropped.
        {{{{100, 0}, {600, 2}}}, 2, 2, 600, -1, 0},
        // weaker shorter: nothing.
        {{{{600, 2}, {100, 0}}}, 2, 2, 600, -1, 0},
        // equal longer: extended.
        {{{{100, 1}, {600, 1}}}, 2, 1, 600, -1, 0},
        // equal shorter: left alone.
        {{{{600, 1}, {100, 1}}}, 2, 1, 600, -1, 0},
        // infinite under stronger.
        {{{{kInf, 0}, {100, 2}}}, 2, 2, 100, 0, kInf},
        // finite under infinite: nothing.
        {{{{kInf, 2}, {600, 0}}}, 2, 2, kInf, -1, 0},
        // equal, infinite over finite.
        {{{{600, 1}, {kInf, 1}}}, 2, 1, kInf, -1, 0},
        // a chain of three.
        {{{{600, 0}, {200, 1}, {100, 2}}}, 3, 2, 100, 1, 200},
    };
    for (const Case& c : cases) {
        Mob mob{20.0, 20.0F};
        for (usize i = 0; i < c.count; ++i) {
            (void)mob.effects.add(of(Effect::Speed, c.steps[i].duration, c.steps[i].amp), mob);
        }
        const EffectInstance* top = mob.effects.get(Effect::Speed);
        REQUIRE(top != nullptr);
        CHECK(top->amplifier == c.top_amp);
        CHECK(top->duration == c.top_duration);
        const EffectInstance* below = mob.effects.hidden(Effect::Speed, 1);
        if (c.hidden_amp < 0) {
            CHECK(below == nullptr);
        } else {
            REQUIRE(below != nullptr);
            CHECK(below->amplifier == c.hidden_amp);
            CHECK(below->duration == c.hidden_duration);
        }
    }
}

TEST_CASE("the hidden effect comes back with what it has left", "[gameplay][effects][parity]") {
    // Measured on the server clock: a chain of 98 / 198 / 598 read 87 / 487
    // after 111 ticks and 385 after 213 — every level counts down together.
    Mob mob{20.0, 20.0F};
    (void)mob.effects.add(of(Effect::Speed, 600, 0), mob);
    (void)mob.effects.add(of(Effect::Speed, 200, 1), mob);
    (void)mob.effects.add(of(Effect::Speed, 100, 2), mob);
    mob.effects.clear_events();
    for (i32 i = 0; i < 111; ++i) {
        mob.tick();
    }
    const EffectInstance* top = mob.effects.get(Effect::Speed);
    REQUIRE(top != nullptr);
    CHECK(top->amplifier == 1);
    CHECK(top->duration == 89);
    CHECK(mob.effects.hidden(Effect::Speed, 1)->duration == 489);
    // The client was told once, when the top changed.
    REQUIRE(mob.effects.events().size() == 1);
    CHECK(mob.effects.events()[0].change == EffectChange::Updated);
    // And the modifier moved with it: speed II's 0.4 now.
    const AttributeMap* map = mob.attributes();
    CHECK(map->get(Attribute::MaxHealth)->modifiers().empty());
}

TEST_CASE("food effects, honey and milk", "[gameplay][effects][parity]") {
    math::XoroshiroRandomSource random{42};
    Mob                         player{20.0, 20.0F};
    REQUIRE(consume_effects(player.effects, "minecraft:golden_apple", player, random));
    REQUIRE(player.effects.get(Effect::Regeneration) != nullptr);
    CHECK(player.effects.get(Effect::Regeneration)->amplifier == 1);
    CHECK(player.effects.get(Effect::Regeneration)->duration == 100);
    CHECK(player.effects.get(Effect::Absorption)->duration == 2400);
    CHECK(player.state.absorption == 4.0F);

    REQUIRE(consume_effects(player.effects, "minecraft:spider_eye", player, random));
    CHECK(player.effects.has(Effect::Poison));
    REQUIRE(consume_effects(player.effects, "minecraft:honey_bottle", player, random));
    CHECK(!player.effects.has(Effect::Poison));
    CHECK(player.effects.has(Effect::Regeneration));
    REQUIRE(consume_effects(player.effects, "minecraft:milk_bucket", player, random));
    CHECK(player.effects.empty());
    CHECK(player.state.absorption == 0.0F);

    CHECK(food_effects("minecraft:enchanted_golden_apple").size() == 4);
    CHECK(food_effects("minecraft:pufferfish").size() == 3);
    CHECK(food_effects("minecraft:bread").empty());
    CHECK(!consume_effects(player.effects, "minecraft:bread", player, random));
}

TEST_CASE("hunger and saturation feed the target", "[gameplay][effects][parity]") {
    // Hunger I for five seconds charged 0.50001 exhaustion; hunger X for two
    // seconds, 1.99999.
    Mob mob{20.0, 20.0F};
    (void)mob.effects.add(of(Effect::Hunger, 100, 0), mob);
    run_out(mob);
    CHECK_THAT(static_cast<f64>(mob.exhausted), Catch::Matchers::WithinAbs(0.5, 1e-4));
    Mob hungrier{20.0, 20.0F};
    (void)hungrier.effects.add(of(Effect::Hunger, 40, 9), hungrier);
    run_out(hungrier);
    CHECK_THAT(static_cast<f64>(hungrier.exhausted), Catch::Matchers::WithinAbs(2.0, 1e-4));

    // Saturation I for three seconds from 4 food and none saved: 7 and 6.
    Mob fed{20.0, 20.0F};
    fed.food.food       = 4;
    fed.food.saturation = 0.0F;
    (void)fed.effects.add(of(Effect::Saturation, 60, 0), fed);
    run_out(fed);
    CHECK(fed.food.food == 7);
    CHECK(fed.food.saturation == 6.0F);
}

TEST_CASE("the save format round-trips", "[gameplay][effects]") {
    Mob mob{20.0, 20.0F};
    (void)mob.effects.add(of(Effect::Speed, 6000, 0), mob);
    (void)mob.effects.add(of(Effect::Speed, 1200, 2), mob);
    EffectInstance hidden_regen = of(Effect::Regeneration, kInfiniteDuration);
    hidden_regen.visible        = false;
    hidden_regen.show_icon      = false;
    (void)mob.effects.add(hidden_regen, mob);
    (void)mob.effects.add(of(Effect::Darkness, 600), mob);
    (void)mob.effects.add(of(Effect::Luck, 2400, 255), mob);

    const auto saved = save_effects(mob.effects);
    REQUIRE(saved);
    REQUIRE(saved->list() != nullptr);
    REQUIRE(saved->list()->size() == 4);
    const nbt::Tag& speed = (*saved->list())[0];
    // The types the real playerdata file carries.
    CHECK(speed.find("Id")->type() == nbt::TagType::Int);
    CHECK(speed.find("Amplifier")->type() == nbt::TagType::Byte);
    CHECK(speed.find("Duration")->type() == nbt::TagType::Int);
    CHECK(speed.find("ShowIcon")->type() == nbt::TagType::Byte);
    REQUIRE(speed.find("HiddenEffect") != nullptr);
    CHECK(speed.find("HiddenEffect")->find("Duration")->as_i64() == 6000);
    // Written in effect-id order: speed 1, regeneration 10, luck 26,
    // darkness 33. Looked up by Id rather than by position anyway.
    const auto by_id = [&](i64 id) -> const nbt::Tag& {
        for (const nbt::Tag& entry : *saved->list()) {
            if (entry.find("Id")->as_i64() == id) {
                return entry;
            }
        }
        FAIL("no entry with that Id");
        return (*saved->list())[0];
    };
    CHECK(by_id(33).find("FactorCalculationData") != nullptr);
    CHECK(by_id(26).find("Amplifier")->as_i64() == -1);

    Mob   again{20.0, 20.0F};
    const usize loaded = load_effects(again.effects, *saved, again);
    CHECK(loaded == 4);
    for (i32 id = 1; id <= 33; ++id) {
        const Effect effect = *effect_from_id(id);
        const auto*  a      = mob.effects.get(effect);
        const auto*  b      = again.effects.get(effect);
        REQUIRE((a == nullptr) == (b == nullptr));
        if (a != nullptr) {
            CHECK(*a == *b);
        }
    }
    CHECK(again.effects.hidden(Effect::Speed, 1)->duration == 6000);
    CHECK(again.effects.get(Effect::Luck)->amplifier == 255);

    CHECK(!save_effects(ActiveEffects{}));
}

TEST_CASE("the metadata the client is shown", "[gameplay][effects][parity]") {
    Mob mob{20.0, 20.0F};
    CHECK(mob.effects.particle_color() == 0);
    CHECK(!mob.effects.all_ambient());
    (void)mob.effects.add(of(Effect::Speed, 600), mob);
    CHECK(mob.effects.particle_color() == 0x33EBFF);
    CHECK(!mob.effects.all_ambient());
    // Hidden particles alone: no colour, and "all ambient" true — measured on
    // the player, absorption hidden sent index 11 = true and no index 10.
    Mob quiet{20.0, 20.0F};
    EffectInstance hidden = of(Effect::Absorption, 600, 1);
    hidden.visible        = false;
    (void)quiet.effects.add(hidden, quiet);
    CHECK(quiet.effects.particle_color() == 0);
    CHECK(quiet.effects.all_ambient());
}

TEST_CASE("ids are the registry's, 1-based", "[gameplay][effects]") {
    CHECK(effect_id(Effect::Speed) == 1);
    CHECK(effect_id(Effect::Darkness) == 33);
    CHECK(effect_info(Effect::Wither).name == "minecraft:wither");
    CHECK(effect_from_name("minecraft:hero_of_the_village") == Effect::HeroOfTheVillage);
    CHECK(!effect_from_id(0));
    CHECK(!effect_from_id(34));
    for (i32 id = 1; id <= 33; ++id) {
        CHECK(effect_info(*effect_from_id(id)).id == id);
    }
}

TEST_CASE("the swirl colour, against every measured case", "[gameplay][effects][parity]") {
    // Campaigns `metadata` (a cow at midnight, one effect at a time, and five
    // mixtures) and `weights` (one effect alone at amplifiers 2, 4 and 6). The
    // game drifts: speed V alone came back 0x33EAFF, not its own 0x33EBFF,
    // and so must this — four orderings of the float arithmetic were tried
    // against these 64 cases and exactly one reproduces all of them.
    struct Single {
        Effect effect;
        u32    colour;
    };
    const Single singles[] = {
        {Effect::Speed, 0x33EBFF},          {Effect::Slowness, 0x8BAFE0},
        {Effect::Haste, 0xD9C043},          {Effect::MiningFatigue, 0x4A4217},
        {Effect::Strength, 0xFFC700},       {Effect::JumpBoost, 0xFDFF84},
        {Effect::Nausea, 0x551D4A},         {Effect::Regeneration, 0xCD5CAB},
        {Effect::Resistance, 0x9146F0},     {Effect::FireResistance, 0xFF9900},
        {Effect::WaterBreathing, 0x98DAC0}, {Effect::Invisibility, 0xF6F6F6},
        {Effect::Blindness, 0x1F1F23},      {Effect::NightVision, 0xC2FF66},
        {Effect::Hunger, 0x587653},         {Effect::Weakness, 0x484D48},
        {Effect::Poison, 0x87A363},         {Effect::Wither, 0x736156},
        {Effect::HealthBoost, 0xF87D23},    {Effect::Absorption, 0x2552A5},
        {Effect::Saturation, 0xF82423},     {Effect::Glowing, 0x94A061},
        {Effect::Levitation, 0xCEFFFF},     {Effect::Luck, 0x59C106},
        {Effect::Unluck, 0xC0A44D},         {Effect::SlowFalling, 0xF3CFB9},
        {Effect::ConduitPower, 0x1DC2D1},   {Effect::DolphinsGrace, 0x88A3BE},
        {Effect::BadOmen, 0x0B6138},        {Effect::HeroOfTheVillage, 0x44FF44},
        {Effect::Darkness, 0x292721},
    };
    usize agree = 0;
    usize total = 0;
    const auto check = [&](std::initializer_list<std::pair<Effect, u8>> parts, u32 colour) {
        Mob mob{20.0, 20.0F};
        for (const auto& [effect, amp] : parts) {
            (void)mob.effects.add(of(effect, 600, amp), mob);
        }
        CHECK(mob.effects.particle_color() == colour);
        agree += mob.effects.particle_color() == colour ? 1 : 0;
        ++total;
    };
    for (const Single& single : singles) {
        CHECK(effect_info(single.effect).color == single.colour);
        check({{single.effect, 0}}, single.colour);
    }
    struct Weighted {
        Effect effect;
        u8     amp;
        u32    colour;
    };
    const Weighted weighted[] = {
        {Effect::Speed, 2, 0x33EBFF},        {Effect::Speed, 4, 0x33EAFF},
        {Effect::Speed, 6, 0x33EAFF},        {Effect::Nausea, 2, 0x551D4A},
        {Effect::Nausea, 4, 0x541D4A},       {Effect::Nausea, 6, 0x541D4A},
        {Effect::Invisibility, 2, 0xF6F6F6}, {Effect::Invisibility, 4, 0xF5F5F5},
        {Effect::Invisibility, 6, 0xF6F6F6}, {Effect::NightVision, 2, 0xC2FF66},
        {Effect::NightVision, 4, 0xC1FF66},  {Effect::NightVision, 6, 0xC1FF66},
        {Effect::Poison, 2, 0x86A263},       {Effect::Poison, 4, 0x86A362},
        {Effect::Poison, 6, 0x86A263},       {Effect::BadOmen, 2, 0x0B6138},
        {Effect::BadOmen, 4, 0x0B6038},      {Effect::BadOmen, 6, 0x0B6038},
        {Effect::SlowFalling, 2, 0xF3CFB8},  {Effect::SlowFalling, 4, 0xF2CFB9},
        {Effect::SlowFalling, 6, 0xF2CFB9},  {Effect::Levitation, 2, 0xCEFFFF},
        {Effect::Levitation, 4, 0xCDFFFF},   {Effect::Levitation, 6, 0xCEFFFF},
        {Effect::Haste, 2, 0xD8C043},        {Effect::Haste, 4, 0xD9C042},
        {Effect::Haste, 6, 0xD9C042},
    };
    for (const Weighted& w : weighted) {
        check({{w.effect, w.amp}}, w.colour);
    }
    check({{Effect::Speed, 0}, {Effect::Strength, 0}}, 0x99D97F);
    check({{Effect::Speed, 0}, {Effect::Haste, 2}}, 0xAFCA72);
    check({{Effect::Strength, 2}, {Effect::Darkness, 0}}, 0xC99F08);
    check({{Effect::Speed, 1}, {Effect::Darkness, 0}}, 0x2FA9B4);
    check({{Effect::Speed, 3}}, 0x33EBFF);
    check({{Effect::Speed, 1}}, 0x33EBFF);
    CHECK(total == 64);
    CHECK(agree == 64);
}
