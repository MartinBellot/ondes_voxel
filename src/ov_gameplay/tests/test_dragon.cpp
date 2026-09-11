// The dragon's rules: the node graph, the damage, the death, the phases.
#include "ov/gameplay/dragon.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] Dragon placed_dragon(bool previously_killed = false) {
    Dragon dragon{Vec3d{0.0, 128.0, 0.0}, 0.0F, 200.0F, previously_killed};
    dragon.graph().place([](i32, i32) { return 64; });
    return dragon;
}

}  // namespace

TEST_CASE("the node rings are the documented ones", "[dragon]") {
    const DragonGraph graph;
    CHECK(graph.node(0) == BlockPos{60, 73, 0});
    CHECK(graph.node(3) == BlockPos{0, 73, 60});
    CHECK(graph.node(6) == BlockPos{-60, 73, 0});
    CHECK(graph.node(12) == BlockPos{40, 73, 0});
    CHECK(graph.node(14) == BlockPos{0, 73, 40});
    CHECK(graph.node(20) == BlockPos{20, 73, 0});
    CHECK(graph.node(21) == BlockPos{0, 73, 20});
    CHECK(graph.node(1) == BlockPos{51, 73, 30});  // 60 cos 30°, 60 sin 30°
    CHECK(graph.node(13) == BlockPos{28, 73, 28});
}

TEST_CASE("node heights: 5 above the ground, 15 for the middle ring, never under 73",
          "[dragon]") {
    DragonGraph graph;
    graph.place([](i32 x, i32) { return x > 30 ? 90 : 60; });
    CHECK(graph.node(0).y == 94);   // outer, over ground at 89: 89 + 5
    CHECK(graph.node(12).y == 104); // middle, 89 + 15
    CHECK(graph.node(20).y == 73);  // inner, 59 + 5 < 73
    CHECK(graph.node(6).y == 73);
}

TEST_CASE("every enabled node reaches every other", "[dragon]") {
    DragonGraph graph;
    graph.place([](i32, i32) { return 64; });
    std::array<u8, kDragonNodeCount> path{};
    for (const i32 crystals : {10, 0}) {
        for (usize a = 0; a < kDragonNodeCount; ++a) {
            for (usize b = 0; b < kDragonNodeCount; ++b) {
                if (!DragonGraph::enabled(a, crystals) || !DragonGraph::enabled(b, crystals)) {
                    continue;
                }
                const usize length = graph.find_path(a, b, crystals, path);
                REQUIRE(length > 0);
                CHECK(path[0] == a);
                CHECK(path[length - 1] == b);
                for (usize i = 0; i + 1 < length; ++i) {
                    CHECK((graph.links(path[i]) & (1U << path[i + 1])) != 0);
                    CHECK(DragonGraph::enabled(path[i], crystals));
                }
            }
        }
    }
}

TEST_CASE("without crystals the outer ring is out of the graph", "[dragon]") {
    DragonGraph graph;
    graph.place([](i32, i32) { return 64; });
    CHECK(graph.closest(Vec3d{60.0, 80.0, 0.0}, 10) == 0);
    CHECK(graph.closest(Vec3d{60.0, 80.0, 0.0}, 0) == 12);
    // A path to a disabled node ends at the enabled node nearest to it.
    std::array<u8, kDragonNodeCount> path{};
    const usize length = graph.find_path(20, 0, 0, path);
    REQUIRE(length > 0);
    CHECK(path[length - 1] == 12);
    // …and there is none when the start already is that node.
    CHECK(graph.find_path(12, 0, 0, path) == 0);
    // Nothing within 100 blocks: node 0.
    CHECK(graph.closest(Vec3d{1000.0, 80.0, 0.0}, 10) == 0);
}

TEST_CASE("a hit on the head is whole, anywhere else a quarter plus one", "[dragon]") {
    Dragon dragon = placed_dragon();
    CHECK(dragon.hurt(DragonPart::Head, 8.0F, DragonHurtSource::Player) == 8.0F);
    CHECK(dragon.health() == 192.0F);
    // Within the cooldown only what exceeds the last hit lands.
    CHECK(dragon.hurt(DragonPart::Head, 6.0F, DragonHurtSource::Player) == 0.0F);
    CHECK(dragon.hurt(DragonPart::Head, 12.0F, DragonHurtSource::Player) == 4.0F);
    CHECK(dragon.health() == 188.0F);
    math::LegacyRandomSource random{1};
    DragonSurroundings        world;
    for (i32 i = 0; i < 20; ++i) {
        DragonOutput out;
        dragon.tick(world, random, out);
    }
    CHECK(dragon.hurt(DragonPart::Body, 8.0F, DragonHurtSource::Player) == 3.0F);
    // Fire, a fall, a mob: nothing.
    for (i32 i = 0; i < 20; ++i) {
        DragonOutput out;
        dragon.tick(world, random, out);
    }
    CHECK(dragon.hurt(DragonPart::Head, 8.0F, DragonHurtSource::Other) == 0.0F);
    // Under a hundredth after the reduction: refused (0.001 + 0.004).
    CHECK(dragon.hurt(DragonPart::Wing1, 0.004F, DragonHurtSource::Player) == 0.0F);
}

TEST_CASE("a lethal hit in flight sends it to the portal to die", "[dragon]") {
    Dragon dragon = placed_dragon();
    CHECK(dragon.hurt(DragonPart::Head, 500.0F, DragonHurtSource::Explosion) > 0.0F);
    CHECK(dragon.health() == 1.0F);
    CHECK(dragon.phase() == DragonPhase::Dying);
    // Hits while it flies there do nothing.
    CHECK(dragon.hurt(DragonPart::Head, 500.0F, DragonHurtSource::Player) == 0.0F);

    math::LegacyRandomSource random{2};
    DragonSurroundings        world;
    world.fountain_top = 64;
    i32  experience    = 0;
    i32  drops         = 0;
    i32  ticks         = 0;
    bool dead          = false;
    std::vector<i32> drop_ticks;
    while (!dead && ticks < 5000) {
        DragonOutput out;
        dragon.tick(world, random, out);
        ++ticks;
        if (out.experience > 0) {
            experience += out.experience;
            ++drops;
            drop_ticks.push_back(dragon.death_time());
        }
        dead = out.dead;
    }
    REQUIRE(dead);
    CHECK(dragon.death_time() == kDragonDeathAnimation);
    CHECK(experience == 12000);
    // Ten drops of 960 from tick 155, every five; the last with 2400 more.
    CHECK(drops == 10);
    CHECK(drop_ticks.front() == 155);
    CHECK(drop_ticks.back() == 200);
    // The animation rises 0.1 a tick.
    CHECK(dragon.position().y > 64.0 + 19.0);
}

TEST_CASE("a later dragon is worth 500", "[dragon]") {
    Dragon dragon = placed_dragon(true);
    dragon.set_phase(DragonPhase::SittingScanning);
    (void)dragon.hurt(DragonPart::Head, 500.0F, DragonHurtSource::Player);
    CHECK(dragon.health() == 0.0F);  // killed on its perch: dies where it sits
    math::LegacyRandomSource random{3};
    DragonSurroundings        world;
    i32                       experience = 0;
    for (i32 i = 0; i < kDragonDeathAnimation; ++i) {
        DragonOutput out;
        dragon.tick(world, random, out);
        experience += out.experience;
    }
    CHECK(experience == 500);
}

TEST_CASE("a perched dragon: arrows bounce, fifty damage ends the perch", "[dragon]") {
    Dragon dragon = placed_dragon();
    dragon.set_phase(DragonPhase::SittingScanning);
    CHECK(dragon.hurt(DragonPart::Head, 9.0F, DragonHurtSource::PlayerProjectile) == 0.0F);
    math::LegacyRandomSource random{4};
    DragonSurroundings        world;
    i32 hits = 0;
    while (dragon.phase() == DragonPhase::SittingScanning && hits < 10) {
        (void)dragon.hurt(DragonPart::Head, 12.0F, DragonHurtSource::Player);
        ++hits;
        if (dragon.phase() != DragonPhase::SittingScanning) {
            break;
        }
        for (i32 i = 0; i < 11; ++i) {
            DragonOutput out;
            dragon.tick(world, random, out);
        }
    }
    // 12, 24, 36, 48, 60: the fifth exceeds fifty.
    CHECK(hits == 5);
    CHECK(dragon.phase() == DragonPhase::Takeoff);
    CHECK(dragon.sitting_damage() == 0.0F);
}

TEST_CASE("a destroyed crystal turns the holding pattern into a strafe, and a fireball",
          "[dragon]") {
    Dragon dragon = placed_dragon();
    const std::array<DragonPlayer, 1> players{DragonPlayer{7, Vec3d{30.0, 70.0, 0.0}, 1.62}};
    DragonSurroundings                world;
    world.crystals = 9;
    world.players  = players;
    dragon.crystal_destroyed(7);
    CHECK(dragon.phase() == DragonPhase::StrafePlayer);
    CHECK(dragon.attack_target() == std::optional<i32>{7});
    math::LegacyRandomSource random{5};
    bool                     fired = false;
    for (i32 i = 0; i < 4000 && !fired; ++i) {
        DragonOutput out;
        dragon.tick(world, random, out);
        if (out.fireball) {
            fired = true;
            CHECK(std::abs(out.fireball_direction.length() - 1.0) < 1e-9);
            CHECK(dragon.phase() == DragonPhase::HoldingPattern);
        }
    }
    CHECK(fired);
}

TEST_CASE("the perch: scanning, the roar, four breaths, the takeoff", "[dragon]") {
    Dragon dragon = placed_dragon();
    const std::array<DragonPlayer, 1> players{DragonPlayer{3, Vec3d{6.0, 64.0, 0.5}, 1.62}};
    DragonSurroundings                world;
    world.crystals     = 0;
    world.players      = players;
    world.fountain_top = 64;
    dragon.set_phase(DragonPhase::Landing);
    math::LegacyRandomSource random{6};
    std::map<DragonPhase, i32> entered;
    i32                        breaths = 0;
    i32                        stops   = 0;
    i32                        roars   = 0;
    for (i32 i = 0; i < 6000 && dragon.phase() != DragonPhase::Takeoff; ++i) {
        const DragonPhase before = dragon.phase();
        DragonOutput      out;
        dragon.tick(world, random, out);
        breaths += out.breath_start ? 1 : 0;
        stops += out.breath_stop ? 1 : 0;
        roars += out.roar ? 1 : 0;
        if (dragon.phase() != before) {
            ++entered[dragon.phase()];
        }
    }
    CHECK(entered[DragonPhase::SittingScanning] == 4);
    CHECK(entered[DragonPhase::SittingAttacking] == 4);
    CHECK(entered[DragonPhase::SittingFlaming] == 4);
    CHECK(breaths == 4);
    CHECK(stops == 4);
    CHECK(roars == 4);
    CHECK(dragon.phase() == DragonPhase::Takeoff);
}

TEST_CASE("a perched dragon with nobody in sight takes off after five seconds", "[dragon]") {
    Dragon dragon = placed_dragon();
    const std::array<DragonPlayer, 1> players{DragonPlayer{3, Vec3d{100.5, 49.0, 0.5}, 1.62}};
    DragonSurroundings                world;
    world.players = players;
    world.sees    = [](Vec3d, Vec3d) { return false; };
    dragon.set_phase(DragonPhase::SittingScanning);
    math::LegacyRandomSource random{7};
    i32                      ticks = 0;
    while (dragon.phase() == DragonPhase::SittingScanning && ticks < 1000) {
        DragonOutput out;
        dragon.tick(world, random, out);
        ++ticks;
    }
    CHECK(ticks == 100);
    CHECK(dragon.phase() == DragonPhase::Takeoff);
    // In sight, it charges instead.
    Dragon other = placed_dragon();
    other.set_phase(DragonPhase::SittingScanning);
    world.sees = {};
    ticks      = 0;
    while (other.phase() == DragonPhase::SittingScanning && ticks < 1000) {
        DragonOutput out;
        other.tick(world, random, out);
        ++ticks;
    }
    CHECK(other.phase() == DragonPhase::ChargingPlayer);
}

TEST_CASE("the holding pattern stays round the island and perches now and then", "[dragon]") {
    Dragon dragon = placed_dragon();
    const std::array<DragonPlayer, 1> players{DragonPlayer{3, Vec3d{100.5, 49.0, 0.5}, 1.62}};
    DragonSurroundings                world;
    world.crystals     = 10;
    world.players      = players;
    world.fountain_top = 64;
    math::LegacyRandomSource random{8};
    i32                      approaches = 0;
    f64                      widest     = 0.0;
    for (i32 i = 0; i < 24000; ++i) {
        const DragonPhase before = dragon.phase();
        DragonOutput      out;
        dragon.tick(world, random, out);
        if (dragon.phase() == DragonPhase::LandingApproach && before != dragon.phase()) {
            ++approaches;
        }
        if (dragon.phase() == DragonPhase::HoldingPattern) {
            const Vec3d p = dragon.position();
            widest        = std::max(widest, std::sqrt(p.x * p.x + p.z * p.z));
            CHECK(p.y > 40.0);
            CHECK(p.y < 180.0);
        }
    }
    CHECK(approaches > 0);
    // Measured on the real server: 91.6 at the widest over 90 s — the ring is
    // 60, the targets up to 20 above it, and a strafe carries it out towards
    // the player at x = 100. The distributions are compared end to end
    // (docs/provenance/dragon.md § 3); this only says it does not fly away.
    CHECK(widest < 140.0);
    CHECK(widest > 40.0);
}

// Not a check: the tool the flight constants were fitted with (hidden, run by
// hand with `test_ov_gameplay "[.fit]"`). One hour of game time per candidate,
// the holding pattern with ten crystals and the probe on the spawn platform, as
// measured; prints the distributions docs/provenance/dragon.md § 2.3 compares.
TEST_CASE("fit: the holding pattern's distributions per flight candidate", "[.fit]") {
    struct Candidate {
        f32 gain;
        f32 turn;
        f64 thrust;
        f64 slowdown;
    };
    const std::array<Candidate, 6> candidates{{
        {0.10F, 10.0F, 0.12, 1.0}, {0.07F, 10.0F, 0.13, 0.8}, {0.08F, 10.0F, 0.13, 1.0},
        {0.10F, 10.0F, 0.13, 1.2}, {0.06F, 10.0F, 0.14, 1.0}, {0.08F, 12.0F, 0.14, 1.2},
    }};
    const std::array<DragonPlayer, 1> players{DragonPlayer{3, Vec3d{100.5, 49.0, 0.5}, 1.62}};
    for (const Candidate& c : candidates) {
        DragonFlight flight;
        flight.turn_gain     = c.gain;
        flight.turn          = c.turn;
        flight.thrust        = c.thrust;
        flight.drag          = 0.9;
        flight.turn_slowdown = c.slowdown;
        Dragon dragon{Vec3d{0.0, 128.0, 0.0}, 0.0F, 200.0F, false, flight};
        dragon.graph().place([](i32, i32) { return 64; });
        DragonSurroundings world;
        world.crystals     = 10;
        world.players      = players;
        world.fountain_top = 67;
        // Measured: the probe on the platform is out of the perched dragon's
        // sight — it takes off rather than charge down to y 49 — and in plain
        // view of the dragon in flight (its strafes fire). Low vantage points
        // are hidden by the island's rim; high ones are not.
        world.sees = [](Vec3d from, Vec3d) { return from.y > 70.0; };
        math::LegacyRandomSource random{11};
        std::vector<f64>         radius;
        std::vector<f64>         height;
        std::vector<f64>         speed;
        std::vector<Vec3d>       track;
        std::vector<bool>        holding;
        std::array<i32, 11>      in_phase{};
        for (i32 i = 0; i < 72000; ++i) {
            DragonOutput out;
            dragon.tick(world, random, out);
            track.push_back(dragon.position());
            holding.push_back(dragon.phase() == DragonPhase::HoldingPattern);
            ++in_phase[static_cast<usize>(dragon.phase())];
        }
        std::printf("  ticks per phase:");
        for (usize p = 0; p < in_phase.size(); ++p) {
            if (in_phase[p] > 0) {
                std::printf(" %s %d", std::string{dragon_phase_name(static_cast<DragonPhase>(p))}.c_str(),
                            in_phase[p]);
            }
        }
        std::printf("\n");
        for (usize i = 0; i + 10 < track.size(); ++i) {
            if (!holding[i]) {
                continue;
            }
            const Vec3d p = track[i];
            const Vec3d q = track[i + 10];
            radius.push_back(std::sqrt(p.x * p.x + p.z * p.z));
            height.push_back(p.y);
            speed.push_back(std::sqrt((q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z)) / 10.0);
        }
        const auto pct = [](std::vector<f64> v, f64 q) {
            std::ranges::sort(v);
            return v.empty() ? 0.0 : v[static_cast<usize>(q * static_cast<f64>(v.size() - 1))];
        };
        std::printf("gain %.2f turn %.1f thrust %.2f slow %.1f | radius %.1f/%.1f/%.1f | height "
                    "%.1f/%.1f/%.1f | speed %.2f/%.2f/%.2f | n %zu\n",
                    static_cast<f64>(c.gain), static_cast<f64>(c.turn), c.thrust, c.slowdown,
                    pct(radius, 0.1),
                    pct(radius, 0.5), pct(radius, 0.9), pct(height, 0.1), pct(height, 0.5),
                    pct(height, 0.9), pct(speed, 0.1), pct(speed, 0.5), pct(speed, 0.9),
                    radius.size());
    }
    SUCCEED();
}

TEST_CASE("the wire yaw is the heading turned half round", "[dragon]") {
    const Dragon dragon{Vec3d{}, 30.0F, 200.0F, false};
    CHECK_THAT(dragon.wire_yaw(), Catch::Matchers::WithinAbs(-150.0, 1e-4));
}
