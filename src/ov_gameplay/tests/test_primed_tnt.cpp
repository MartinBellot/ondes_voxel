// Primed TNT, creepers and detonations, against a real 1.20.1 server.
//
// The trajectory numbers are the vanilla entity's own `Motion` and `Pos`,
// each read beside its `Fuse` so that the tick they belong to is exact —
// scripts/measure_tnt_gravity.py tnt_motion, 7900 samples. See
// docs/provenance/tnt-et-gravite.md.
#include "tnt_gravity_fixture.hpp"

#include "ov/gameplay/redstone.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

using namespace ov;
using namespace ov::gameplay;
using namespace ov::test;
using Catch::Approx;

#define REQUIRE_PACK()                                              \
    if (pack_blocks() == nullptr || pack_registries() == nullptr) { \
        WARN("registry.ovpack missing");                            \
        return;                                                     \
    }

TEST_CASE("a primed TNT is thrown up 0.2F and sideways 0.02", "[gameplay][tnt][parity]") {
    // Captured on a redstone-primed TNT: (104, 1600, 120) in 1/8000ths. The
    // magnitude is the number; the direction is drawn.
    math::LegacyRandomSource rng{12345};
    std::set<int>            quadrants;
    for (int i = 0; i < 1000; ++i) {
        const Vec3d v = tnt_prime_velocity(rng);
        REQUIRE(v.y == static_cast<f64>(0.2F));
        REQUIRE(std::hypot(v.x, v.z) == Approx(0.02).margin(1e-15));
        quadrants.insert((v.x > 0 ? 1 : 0) + (v.z > 0 ? 2 : 0));
    }
    REQUIRE(quadrants.size() == 4);
    // The wire truncates each component to a whole 1/8000th, so the captured
    // (104, 120) can sit up to one unit short on each axis — 0.019849 for a
    // true 0.02. It cannot sit further than that.
    const f64 captured = std::hypot(104.0, 120.0) / 8000.0;
    REQUIRE(captured <= 0.02);
    REQUIRE(captured >= std::hypot(104.0 + 1.0, 120.0 + 1.0) / 8000.0 - 0.0005);
    REQUIRE(std::hypot(105.0, 121.0) / 8000.0 >= 0.02);
}

TEST_CASE("a primed TNT hops, lands and slides the way vanilla's does",
          "[gameplay][tnt][parity]") {
    REQUIRE_PACK();
    // Ten TNT on stone at y = -60, five priming each: every sample of one tick
    // agreed to the last digit, so one column is enough to freeze.
    struct Sample {
        i32  n;
        f64  y;
        f64  vy;
        f64  horizontal;
        bool on_ground;
    };
    constexpr std::array<Sample, 14> kVanilla{{
        {1, -59.83999999701977, 0.1568000029206276, 0.0196, false},
        {2, -59.72319999409914, 0.11446400286221503, 0.019208, false},
        {3, -59.648735991236926, 0.07297472280497072, 0.01882384, false},
        {4, -59.61576126843195, 0.03231522834887131, 0.01844736, false},
        {5, -59.62344604008308, -0.0075310762181061185, 0.01807842, false},
        {6, -59.67097711630119, -0.046580454693744, 0.01771685, false},
        {7, -59.75755757099493, -0.08484884559986913, 0.01736251, false},
        {8, -59.882406416594804, -0.12235186868787176, 0.01701526, false},
        {9, -60.0, 0.0, 0.01167247, true},
        {10, -60.0, 0.0, 0.00800731, true},
        {11, -60.0, 0.0, 0.00549302, true},
        {12, -60.0, 0.0, 0.00376821, true},
        {13, -60.0, 0.0, 0.00258499, true},
        {14, -60.0, 0.0, 0.0017733, true},
    }};

    TestLevel      level{*pack_blocks(), -61};
    CollisionWorld collisions{*pack_blocks(), &TestLevel::look_up, &level};
    entity::EntityState state;
    state.width    = 0.98F;
    state.height   = 0.98F;
    state.position = Vec3d{0.5, -60.0, 24.5};
    state.velocity = Vec3d{kTntPrimeSpread, kTntPrimeUp, 0.0};
    for (const Sample& sample : kVanilla) {
        state = step_block_entity(state, BlockEntityMotion{}, collisions);
        INFO("tick " << sample.n);
        REQUIRE(state.position.y == Approx(sample.y).margin(1e-9));
        REQUIRE(state.velocity.y == Approx(sample.vy).margin(1e-12));
        REQUIRE(std::hypot(state.velocity.x, state.velocity.z) ==
                Approx(sample.horizontal).margin(5e-9));
        REQUIRE(state.on_ground == sample.on_ground);
    }
}

TEST_CASE("in the air a primed TNT follows gravity 0.04 and drag 0.98", "[gameplay][tnt][parity]") {
    REQUIRE_PACK();
    // The air half of the same campaign: 3950 samples, the model agreeing to
    // 3e-9 on the vertical. Reproduced here as the closed recurrence for all
    // eighty ticks of a fuse.
    TestLevel      level{*pack_blocks(), -200};
    CollisionWorld collisions{*pack_blocks(), &TestLevel::look_up, &level};
    entity::EntityState state;
    state.width    = 0.98F;
    state.height   = 0.98F;
    state.position = Vec3d{0.5, 100.0, 0.5};
    state.velocity = Vec3d{0.0, kTntPrimeUp, kTntPrimeSpread};
    f64 vy = kTntPrimeUp;
    f64 y  = 100.0;
    for (int n = 1; n <= 80; ++n) {
        vy -= 0.04;
        y += vy;
        vy *= 0.98;
        state = step_block_entity(state, BlockEntityMotion{}, collisions);
        REQUIRE(state.velocity.y == Approx(vy).margin(1e-12));
        REQUIRE(state.position.y == Approx(y).margin(1e-9));
        REQUIRE(state.velocity.z == Approx(0.02 * std::pow(0.98, n)).margin(1e-15));
    }
}

TEST_CASE("the fuse runs eighty ticks and the charge goes off a sixteenth up",
          "[gameplay][tnt]") {
    REQUIRE_PACK();
    TestLevel           level{*pack_blocks(), -61};
    entity::EntityWorld world{*pack_registries()};
    FallingBlocks       rules{*pack_blocks(), *pack_registries()};
    Simulation          sim{level, rules, world};
    const auto handle = world.spawn("minecraft:tnt", Vec3d{0.5, -60.0, 0.5}, net::Uuid{}).value();
    world.set_logic(handle, std::make_unique<PrimedTntLogic>(kTntFuseTicks, sim.blasts));
    for (int t = 0; t < 79; ++t) {
        sim.step();
    }
    REQUIRE(world.alive(handle));
    REQUIRE(sim.blasts.blasts.empty());
    sim.step();
    REQUIRE_FALSE(world.alive(handle));
    REQUIRE(sim.blasts.blasts.size() == 1);
    // Captured: -59.93874999880791, which is 0.98F × 0.0625 widened.
    REQUIRE(sim.blasts.blasts[0].centre.y == -59.93874999880791);
    REQUIRE(sim.blasts.blasts[0].power == kTntPower);
}

TEST_CASE("a chained fuse lands in the measured ten to twenty-nine", "[gameplay][tnt][parity]") {
    REQUIRE_PACK();
    // Measured: 384 ring TNT lit by one charge, every fuse between 10 and 29,
    // all twenty values present.
    const Explosions         explosions{*pack_blocks()};
    math::LegacyRandomSource rng{7};
    std::set<i32>            seen;
    for (int i = 0; i < 4000; ++i) {
        const i32 fuse = explosions.chained_fuse(kTntFuseTicks, rng);
        REQUIRE(fuse >= 10);
        REQUIRE(fuse <= 29);
        seen.insert(fuse);
    }
    REQUIRE(seen.size() == 20);
}

TEST_CASE("redstone lights a TNT block and nothing else does", "[gameplay][tnt]") {
    REQUIRE_PACK();
    const Redstone redstone{*pack_blocks(), *pack_registries()};
    TestLevel      level{*pack_blocks(), -61};
    const auto     tnt = pack_blocks()->find_block("minecraft:tnt").value();
    level.place(BlockPos{0, -50, 0}, state_of("minecraft:tnt"));
    REQUIRE_FALSE(tnt_lit_by_signal(redstone.signals(), level, BlockPos{0, -50, 0}, tnt));
    level.place(BlockPos{1, -50, 0}, state_of("minecraft:redstone_block"));
    REQUIRE(tnt_lit_by_signal(redstone.signals(), level, BlockPos{0, -50, 0}, tnt));
    // A powered neighbour that is not TNT is not lit, whatever the signal.
    level.place(BlockPos{0, -49, 0}, state_of("minecraft:stone"));
    REQUIRE_FALSE(tnt_lit_by_signal(redstone.signals(), level, BlockPos{0, -49, 0}, tnt));
}

TEST_CASE("a creeper counts to thirty at a close target and backs off from a far one",
          "[gameplay][creeper]") {
    CreeperSwell creeper;
    // Out of reach: nothing.
    for (int t = 0; t < 40; ++t) {
        REQUIRE_FALSE(tick_creeper(creeper, 5.0, true).explode);
    }
    REQUIRE(creeper.swell == 0);

    // Two blocks away and in sight: thirty ticks, the measured fuse.
    int ticks = 0;
    CreeperStep step;
    do {
        step = tick_creeper(creeper, 2.0, true);
        ++ticks;
    } while (!step.explode && ticks < 100);
    REQUIRE(ticks == kCreeperFuseTicks);

    // Swelling, then the target walks past seven blocks: it shrinks back.
    CreeperSwell second;
    for (int t = 0; t < 10; ++t) {
        (void)tick_creeper(second, 2.5, true);
    }
    REQUIRE(second.swell == 10);
    REQUIRE(tick_creeper(second, 8.0, true).direction_changed);
    REQUIRE(second.swell == 9);
    // Between three and seven it keeps going once started — the hysteresis.
    CreeperSwell third;
    (void)tick_creeper(third, 2.0, true);
    REQUIRE_FALSE(tick_creeper(third, 6.0, true).direction_changed);
    REQUIRE(third.swell == 2);

    // Lit by flint and steel: no target needed.
    CreeperSwell lit;
    lit.ignited = true;
    int lit_ticks = 0;
    while (!tick_creeper(lit, std::nullopt, false).explode && lit_ticks < 100) {
        ++lit_ticks;
    }
    REQUIRE(lit_ticks + 1 == kCreeperFuseTicks);

    CreeperSwell charged;
    charged.powered = true;
    REQUIRE(creeper_power(charged) == 6.0F);
    REQUIRE(creeper_power(CreeperSwell{}) == 3.0F);
}

namespace {

/// A solid box of dirt with a TNT block at its centre, as the crater bench
/// built it on the vanilla server.
void dirt_box(TestLevel& level, i32 half, i32 up) {
    const auto dirt = state_of("minecraft:dirt");
    for (i32 y = -up; y <= up; ++y) {
        for (i32 z = -half; z <= half; ++z) {
            for (i32 x = -half; x <= half; ++x) {
                level.place(BlockPos{x, y, z}, dirt);
            }
        }
    }
}

}  // namespace

TEST_CASE("TNT drops everything it breaks and a creeper one in three", "[gameplay][tnt][parity]") {
    REQUIRE_PACK();
    // Measured: TNT 1754 items for 1754 blocks (explosions.md § 5); a creeper
    // 333 for 989, 0.337, over twelve shots each.
    const Explosions  explosions{*pack_blocks()};
    const LootTables  loot{*pack_blocks(), *pack_registries()};
    const auto        tnt = pack_blocks()->find_block("minecraft:tnt").value();
    math::LegacyRandomSource    rng{99};
    math::XoroshiroRandomSource loot_rng{1, 2};
    Detonation                  detonation;

    for (const auto& [power, interaction, low, high] :
         {std::tuple{kTntPower, BlockInteraction::Destroy, 1.0, 1.0},
          std::tuple{kCreeperPower, kCreeperInteraction, 0.28, 0.39}}) {
        usize broken = 0;
        usize items  = 0;
        for (int shot = 0; shot < 12; ++shot) {
            TestLevel level{*pack_blocks(), -200};
            dirt_box(level, 8, 5);
            ExplosionSpec spec;
            spec.centre      = Vec3d{0.5, 0.06125, 0.5};
            spec.power       = power;
            spec.interaction = interaction;
            collect_detonation(level, explosions, spec, rng, detonation);
            destroy_detonation(level, explosions, &loot, spec, tnt, rng, loot_rng, detonation);
            broken += detonation.blocks.size();
            for (const auto& dropped : detonation.drops) {
                items += static_cast<usize>(dropped.drop.count);
            }
            for (const BlockPos pos : detonation.blocks) {
                REQUIRE(level.block_at(pos) == registry::kAirState);
            }
        }
        const f64 ratio = static_cast<f64>(items) / static_cast<f64>(broken);
        INFO("power " << power << ": " << items << " items for " << broken << " blocks");
        REQUIRE(ratio >= low);
        REQUIRE(ratio <= high);
    }
}

TEST_CASE("an explosion lights the TNT it reaches instead of breaking it", "[gameplay][tnt]") {
    REQUIRE_PACK();
    const Explosions  explosions{*pack_blocks()};
    const LootTables  loot{*pack_blocks(), *pack_registries()};
    const auto        tnt = pack_blocks()->find_block("minecraft:tnt").value();
    TestLevel         level{*pack_blocks(), -200};
    // The chain bench: a ring of TNT two blocks out, three high.
    usize ring = 0;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -2; dz <= 2; ++dz) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) == 2) {
                    level.place(BlockPos{dx, -40 + dy, dz}, state_of("minecraft:tnt"));
                    ++ring;
                }
            }
        }
    }
    math::LegacyRandomSource    rng{5};
    math::XoroshiroRandomSource loot_rng{3, 4};
    Detonation                  detonation;
    ExplosionSpec               spec;
    spec.centre = Vec3d{0.5, -40.0 + 0.06125, 0.5};
    collect_detonation(level, explosions, spec, rng, detonation);
    destroy_detonation(level, explosions, &loot, spec, tnt, rng, loot_rng, detonation);
    // Measured: 48 of 48, every trial.
    REQUIRE(ring == 48);
    REQUIRE(detonation.primed.size() == 48);
    REQUIRE(detonation.drops.empty());
    for (const auto& primed : detonation.primed) {
        REQUIRE(primed.fuse >= 10);
        REQUIRE(primed.fuse <= 29);
        REQUIRE(level.block_at(primed.pos) == registry::kAirState);
    }
    // And the air the rays crossed is listed apart, for the packet.
    REQUIRE(detonation.air.size() > detonation.blocks.size());
}
