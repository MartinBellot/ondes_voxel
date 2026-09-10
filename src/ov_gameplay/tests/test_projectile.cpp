// Projectiles, against a real 1.20.1 server.
//
// Every expected number below is a vanilla entity's own `Motion`, `Pos` or a
// target's `Health`, read by scripts/measure_projectiles.py and written into
// data/vanilla/1.20.1/normalized/projectile_*.json. See
// docs/provenance/projectiles.md.
#include "tnt_gravity_fixture.hpp"

#include "ov/gameplay/projectile.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <memory>

using namespace ov;
using namespace ov::gameplay;
using namespace ov::test;
using Catch::Approx;

namespace {

#define REQUIRE_PACK()                                              \
    if (pack_blocks() == nullptr || pack_registries() == nullptr) { \
        WARN("registry.ovpack missing");                            \
        return;                                                     \
    }

/// A world of air over nothing: nothing to hit.
registry::BlockStateId empty_lookup(void*, i32, i32, i32) {
    return registry::kAirState;
}

/// A level that is air above y = -61 and stone at or below it.
registry::BlockStateId floor_lookup(void* context, i32 x, i32 y, i32 z) {
    return static_cast<TestLevel*>(context)->block_at(BlockPos{x, y, z});
}

entity::EntityState at(Vec3d position, Vec3d velocity) {
    entity::EntityState state;
    state.network_id = 7;
    state.position   = position;
    state.velocity   = velocity;
    return state;
}

}  // namespace

TEST_CASE("an arrow moves, then drags 0.99F, then falls 0.05F", "[gameplay][projectile][parity]") {
    REQUIRE_PACK();
    // Measured: summoned at (0.5, 150, 0.5) with Motion (0.6, 0.4, -0.3). The
    // first two samples a witness TNT labels ages 1 and 2.
    const CollisionWorld world{*pack_blocks(), &empty_lookup, nullptr};
    ProjectileWorld      targets;
    ProjectileData       data;
    data.kind = ProjectileKind::Arrow;
    entity::EntityState state = at({0.5, 150.0, 0.5}, {0.6, 0.4, -0.3});

    REQUIRE_FALSE(step_projectile(state, data, world, nullptr, targets));
    REQUIRE(state.position.x == 1.1);
    REQUIRE(state.position.y == 150.4);
    REQUIRE(state.velocity.x == 0.5940000057220459);
    REQUIRE(state.velocity.y == 0.34600000306963924);
    REQUIRE(state.velocity.z == -0.29700000286102296);

    REQUIRE_FALSE(step_projectile(state, data, world, nullptr, targets));
    REQUIRE(state.position.x == 1.694000005722046);
    REQUIRE(state.position.y == 150.74600000306964);
    REQUIRE(state.position.z == -0.09700000286102295);
    REQUIRE(state.velocity.x == 0.5880600113296509);
    REQUIRE(state.velocity.y == 0.29254000559359794);
    REQUIRE(state.velocity.z == -0.29403000566482546);

    REQUIRE_FALSE(step_projectile(state, data, world, nullptr, targets));
    REQUIRE(state.velocity.x == 0.5821794168245317);
    REQUIRE(state.velocity.y == 0.23961460758248282);
    REQUIRE(state.position.x == 2.282060017051697);
    REQUIRE(state.position.y == Approx(151.03854000866323).margin(1e-12));
}

TEST_CASE("thrown items fall 0.03F, a bottle 0.07F, a potion 0.05F",
          "[gameplay][projectile][parity]") {
    REQUIRE_PACK();
    const CollisionWorld world{*pack_blocks(), &empty_lookup, nullptr};
    ProjectileWorld      targets;
    const std::map<ProjectileKind, f64> after_one{
        {ProjectileKind::Snowball, 0.36600000448524955},
        {ProjectileKind::Egg, 0.36600000448524955},
        {ProjectileKind::EnderPearl, 0.36600000448524955},
        {ProjectileKind::ExperienceBottle, 0.3260000035166741},
        {ProjectileKind::Trident, 0.34600000306963924},
    };
    for (const auto& [kind, vy] : after_one) {
        ProjectileData data;
        data.kind                 = kind;
        entity::EntityState state = at({0.5, 150.0, 8.5}, {0.6, 0.4, -0.3});
        REQUIRE_FALSE(step_projectile(state, data, world, nullptr, targets));
        REQUIRE(state.velocity.x == 0.5940000057220459);
        REQUIRE(state.velocity.y == vy);
    }
    // The snowball's second transition, as measured.
    ProjectileData data;
    data.kind                 = ProjectileKind::Snowball;
    entity::EntityState state = at({1.1, 150.4, 8.2}, {0.5940000057220459, 0.36600000448524955,
                                                       -0.29700000286102296});
    REQUIRE_FALSE(step_projectile(state, data, world, nullptr, targets));
    REQUIRE(state.velocity.y == 0.33234000860139734);
    REQUIRE(state.position.y == 150.76600000448525);
}

TEST_CASE("in water an arrow drags 0.6F, a snowball 0.8F, a trident 0.99F",
          "[gameplay][projectile][parity]") {
    REQUIRE_PACK();
    TestLevel water{*pack_blocks(), -200};
    const auto water_state = state_of("minecraft:water");
    for (i32 x = -1; x <= 3; ++x) {
        for (i32 y = 100; y <= 112; ++y) {
            for (i32 z = -42; z <= -28; ++z) {
                water.set_block(BlockPos{x, y, z}, water_state);
            }
        }
    }
    const CollisionWorld world{*pack_blocks(), &floor_lookup, &water};
    ProjectileWorld      targets;
    targets.water = pack_blocks()->find_block("minecraft:water").value_or(registry::BlockId{0});
    REQUIRE(targets.water.value() != 0);

    struct Case {
        ProjectileKind kind;
        Vec3d          start;
        Vec3d          v0;
        Vec3d          v1;
    };
    // Measured transitions from the first sample (age 1) to the next.
    const Case cases[] = {
        {ProjectileKind::Arrow, {1.1, 105.5, -35.4},
         {0.36000001430511475, -0.05000000074505806, 0.06000000238418579},
         {0.21600001716613804, -0.08000000238418581, 0.036000002861023006}},
        {ProjectileKind::Snowball, {1.1, 105.5, -31.4},
         {0.48000000715255736, -0.029999999329447746, 0.0800000011920929},
         {0.38400001144409185, -0.053999999150633804, 0.06400000190734866}},
        {ProjectileKind::Trident, {1.1, 109.0, -39.4},
         {0.5940000057220459, -0.05000000074505806, 0.09900000095367432},
         {0.5880600113296509, -0.0995000019595027, 0.09801000188827516}},
    };
    for (const Case& c : cases) {
        ProjectileData data;
        data.kind                 = c.kind;
        entity::EntityState state = at(c.start, c.v0);
        REQUIRE_FALSE(step_projectile(state, data, world, &water, targets));
        REQUIRE(state.velocity.x == c.v1.x);
        REQUIRE(state.velocity.y == c.v1.y);
        REQUIRE(state.velocity.z == c.v1.z);
    }
}

TEST_CASE("an arrow sticks 0.05F short of the face, keeping its last travel",
          "[gameplay][projectile][parity]") {
    REQUIRE_PACK();
    TestLevel level{*pack_blocks(), -61};
    // A stone wall at x = 10.
    for (i32 y = -60; y <= -56; ++y) {
        for (i32 z = -2; z <= 6; ++z) {
            level.set_block(BlockPos{10, y, z}, state_of("minecraft:stone"));
        }
    }
    const CollisionWorld world{*pack_blocks(), &floor_lookup, &level};
    ProjectileWorld      targets;

    SECTION("straight down onto the ground") {
        ProjectileData data;
        entity::EntityState state = at({2.5, -55.0, 0.5}, {0.0, -1.3, 0.0});
        std::optional<ProjectileEvent> event;
        for (int i = 0; i < 4 && !event; ++i) {
            event = step_projectile(state, data, world, &level, targets);
        }
        REQUIRE(event);
        REQUIRE(event->kind == ProjectileEvent::Kind::Stuck);
        REQUIRE(data.in_ground);
        REQUIRE(state.position.y == -59.94999999925494);
        REQUIRE(state.velocity.y == -1.0294762709270207);
    }
    SECTION("flat into a wall") {
        ProjectileData data;
        entity::EntityState state = at({5.5, -57.5, 4.5}, {1.7, 0.0, 0.0});
        std::optional<ProjectileEvent> event;
        for (int i = 0; i < 4 && !event; ++i) {
            event = step_projectile(state, data, world, &level, targets);
        }
        REQUIRE(event);
        REQUIRE(data.in_ground);
        REQUIRE(state.position.x == Approx(9.950088916881048).margin(1e-12));
        REQUIRE(state.position.y == Approx(-57.61372419795296).margin(1e-12));
        REQUIRE(state.velocity.x == Approx(1.1058299946022032).margin(1e-15));
        REQUIRE(state.velocity.y == Approx(-0.11603773069454587).margin(1e-15));
    }
    SECTION("it stays 1200 ticks, then goes") {
        ProjectileData data;
        data.in_ground            = true;
        entity::EntityState state = at({2.5, -59.95, 0.5}, {});
        for (int i = 1; i < kArrowDespawnTicks; ++i) {
            REQUIRE_FALSE(step_projectile(state, data, world, &level, targets));
        }
        const auto event = step_projectile(state, data, world, &level, targets);
        REQUIRE(event);
        REQUIRE(event->kind == ProjectileEvent::Kind::Expired);
    }
    SECTION("a snowball breaks instead") {
        ProjectileData data;
        data.kind                 = ProjectileKind::Snowball;
        entity::EntityState state = at({5.5, -57.5, 2.5}, {1.7, 0.0, 0.0});
        std::optional<ProjectileEvent> event;
        for (int i = 0; i < 4 && !event; ++i) {
            event = step_projectile(state, data, world, &level, targets);
        }
        REQUIRE(event);
        REQUIRE(event->kind == ProjectileEvent::Kind::Broke);
        REQUIRE(state.removed);
    }
}

TEST_CASE("an arrow hits a box grown by 0.3 on its first tick", "[gameplay][projectile]") {
    REQUIRE_PACK();
    const CollisionWorld world{*pack_blocks(), &empty_lookup, nullptr};
    ProjectileWorld      targets;
    // The damage campaign's cow: 0.9 wide at x = 10.5, the arrow from 9.65.
    targets.targets.push_back(
        ProjectileTarget{42, AABB::from_entity({10.5, -60.0, 0.5}, 0.9, 1.4), false, false});
    for (const f64 speed : {0.3, 0.7, 1.0, 3.0}) {
        ProjectileData      data;
        entity::EntityState state = at({9.65, -59.3, 0.5}, {speed, 0.0, 0.0});
        const auto          event = step_projectile(state, data, world, nullptr, targets);
        REQUIRE(event);
        REQUIRE(event->kind == ProjectileEvent::Kind::HitEntity);
        REQUIRE(event->target == 42);
        REQUIRE(event->velocity.x == speed);
    }
    SECTION("but not its owner, until it has cleared them") {
        ProjectileData data;
        data.owner                = 42;
        entity::EntityState state = at({9.65, -59.3, 0.5}, {1.0, 0.0, 0.0});
        REQUIRE_FALSE(step_projectile(state, data, world, nullptr, targets));
    }
}

TEST_CASE("arrow damage is ceil(speed x base), a crit adds [0, dmg/2+2)",
          "[gameplay][projectile][parity]") {
    // Measured, one fresh cow per shot, hit on the first tick.
    struct Case {
        f64 speed;
        f64 base;
        i32 lost;
    };
    const Case cases[] = {{1.0, 2.0, 2}, {1.3, 2.0, 3}, {1.9, 2.0, 4}, {2.6, 2.0, 6},
                         {3.0, 2.0, 6}, {1.3, 3.5, 5}, {0.7, 0.5, 1}};
    math::LegacyRandomSource random{1};
    for (const Case& c : cases) {
        REQUIRE(arrow_damage(Vec3d{c.speed, 0.0, 0.0}, c.base, false, random) == c.lost);
    }
    // 24 crits at speed 3: vanilla read 6..10, every value present.
    std::map<i32, int> seen;
    for (int i = 0; i < 2000; ++i) {
        ++seen[arrow_damage(Vec3d{3.0, 0.0, 0.0}, 2.0, true, random)];
    }
    REQUIRE(seen.begin()->first == 6);
    REQUIRE(seen.rbegin()->first == 10);
    REQUIRE(seen.size() == 5);
    REQUIRE(snowball_damage(true) == 3.0F);
    REQUIRE(snowball_damage(false) == 0.0F);
}

TEST_CASE("a bow's power is (f^2 + 2f)/3 of t/20, in floats", "[gameplay][projectile]") {
    REQUIRE(bow_power(0) == 0.0F);
    REQUIRE(bow_power(20) == 1.0F);
    REQUIRE(bow_power(40) == 1.0F);
    const f32 f = 10.0F / 20.0F;
    REQUIRE(bow_power(10) == (f * f + f * 2.0F) / 3.0F);
    REQUIRE(bow_power(2) < kBowMinimumPower);
    REQUIRE(bow_power(3) >= kBowMinimumPower);
    REQUIRE(crossbow_charge_ticks(0) == 25);
    REQUIRE(crossbow_charge_ticks(3) == 10);
}

TEST_CASE("the look vector uses the float sine table", "[gameplay][projectile]") {
    // Yaw -90 looks along +x; pitch -90 straight up.
    const Vec3d east = look_direction(-90.0F, 0.0F);
    REQUIRE(east.x == Approx(1.0).margin(1e-6));
    REQUIRE(std::abs(east.z) < 1e-6);
    const Vec3d up = look_direction(0.0F, -90.0F);
    REQUIRE(up.y == Approx(1.0).margin(1e-6));

    math::LegacyRandomSource random{99};
    f64                      worst = 0.0;
    for (int i = 0; i < 1000; ++i) {
        const Vec3d v = shoot_velocity(east, 3.0F, 1.0F, random);
        // Triangular jitter of half-width 0.0172275 per axis, scaled by 3.
        worst = std::max({worst, std::abs(v.y), std::abs(v.z)});
    }
    REQUIRE(worst <= 3.0 * 0.0172275);
    REQUIRE(worst > 3.0 * 0.0172275 * 0.8);
}

TEST_CASE("an egg hatches one chicken in eight, four in 256", "[gameplay][projectile]") {
    math::LegacyRandomSource random{2024};
    std::map<i32, int>       counts;
    constexpr int            kEggs = 256'000;
    for (int i = 0; i < kEggs; ++i) {
        ++counts[egg_chickens(random)];
    }
    REQUIRE(counts.size() == 3);
    REQUIRE(static_cast<f64>(counts[0]) / kEggs == Approx(7.0 / 8.0).margin(0.005));
    REQUIRE(static_cast<f64>(counts[4]) / kEggs == Approx(1.0 / 256.0).margin(0.0008));
}
