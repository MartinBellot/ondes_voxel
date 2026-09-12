// The wandering trader's offers against 200 traders of a real 1.20.1 server,
// its despawn counter, and the spawner's chance schedule
// (docs/provenance/cerveaux.md § 9).
#include "ov/gameplay/wandering_trader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {
[[nodiscard]] const Listing* find(std::span<const Listing> pool, std::string_view item) {
    const auto it = std::ranges::find_if(pool, [&](const Listing& l) { return l.item == item; });
    return it == pool.end() ? nullptr : &*it;
}
}  // namespace

TEST_CASE("the trader's two pools, as 200 real traders showed them", "[trader][parity]") {
    const auto ordinary = wandering_trader_offers();
    const auto rare     = wandering_trader_rare_offers();
    CHECK(ordinary.size() == 64);
    CHECK(rare.size() == 6);
    std::set<std::string_view> names;
    for (const Listing& l : ordinary) {
        names.insert(l.item);
        CHECK(l.xp == 1);
        CHECK(l.multiplier == 0.05F);
    }
    CHECK(names.size() == 64);
    // Spot checks, one of each price class seen.
    struct Seen {
        std::string_view item;
        i32              count, emeralds, max;
    };
    for (const Seen& s : {Seen{"minecraft:acacia_sapling", 1, 5, 8}, Seen{"minecraft:glowstone", 1, 2, 5},
                          Seen{"minecraft:slime_ball", 1, 4, 5}, Seen{"minecraft:lily_of_the_valley", 1, 1, 7},
                          Seen{"minecraft:pumpkin", 1, 1, 4}, Seen{"minecraft:sand", 8, 1, 8},
                          Seen{"minecraft:red_sand", 4, 1, 6}, Seen{"minecraft:cyan_dye", 3, 1, 12},
                          Seen{"minecraft:moss_block", 2, 1, 5}, Seen{"minecraft:kelp", 1, 3, 12}}) {
        const Listing* l = find(ordinary, s.item);
        INFO(s.item);
        REQUIRE(l != nullptr);
        CHECK(l->count == s.count);
        CHECK(l->emeralds == s.emeralds);
        CHECK(l->max_uses == s.max);
    }
    for (const Seen& s : {Seen{"minecraft:blue_ice", 1, 6, 6}, Seen{"minecraft:gunpowder", 1, 1, 8},
                          Seen{"minecraft:podzol", 3, 3, 6}, Seen{"minecraft:pufferfish_bucket", 1, 5, 4}}) {
        const Listing* l = find(rare, s.item);
        INFO(s.item);
        REQUIRE(l != nullptr);
        CHECK(l->count == s.count);
        CHECK(l->emeralds == s.emeralds);
        CHECK(l->max_uses == s.max);
    }
}

TEST_CASE("a trader has five distinct ordinary offers and one rare", "[trader]") {
    for (i64 seed = 0; seed < 50; ++seed) {
        math::LegacyRandomSource random{seed};
        VillagerState            trader;
        init_wandering_trader(trader, random);
        REQUIRE(trader.offers.size() == 6);
        CHECK(trader.wandering);
        std::set<std::string_view> first;
        for (usize i = 0; i < 5; ++i) {
            first.insert(trader.offers[i].result.item);
            CHECK(find(wandering_trader_offers(), trader.offers[i].result.item) != nullptr);
        }
        CHECK(first.size() == 5);
        CHECK(find(wandering_trader_rare_offers(), trader.offers[5].result.item) != nullptr);
        CHECK(trader.offers[0].cost_a.item == "minecraft:emerald");
    }
}

TEST_CASE("DespawnDelay counts down one a tick and the trader leaves at 0", "[trader][parity]") {
    VillagerState trader;
    trader.despawn_delay = 100;
    i32 ticks = 0;
    while (!tick_despawn(trader)) {
        ++ticks;
        REQUIRE(ticks < 200);
    }
    CHECK(ticks == 99);  // left on the hundredth tick (measured: gone after 94 read 4)
    VillagerState summoned;  // no DespawnDelay: never leaves
    for (int i = 0; i < 1000; ++i) {
        CHECK_FALSE(tick_despawn(summoned));
    }
}

TEST_CASE("the spawner: every 24000 ticks, a chance of 25, 50, then 75", "[trader]") {
    TraderSpawner            spawner;
    math::LegacyRandomSource random{3};
    i32                      tries = 0;
    for (i32 t = 0; t < 24000 * 3; ++t) {
        if (spawner.tick(random, true)) {
            ++tries;
        }
    }
    CHECK(spawner.spawn_chance == 75);  // three days, no trader reported: 25 → 50 → 75, capped
    CHECK(tries <= 3);
    spawner.came();
    CHECK(spawner.spawn_chance == 25);
    TraderSpawner off;  // doTraderSpawning false: never
    for (i32 t = 0; t < 24000 * 2; ++t) {
        CHECK_FALSE(off.tick(random, false));
    }
}
