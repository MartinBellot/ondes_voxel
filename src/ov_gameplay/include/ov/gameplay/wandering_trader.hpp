// The wandering trader: its offers, and when one comes.
//
// ── Offers ──────────────────────────────────────────────────────────────────
//
// Measured, not read: 200 traders summoned on a real 1.20.1 server, their
// `Offers` relisted (scripts/measure_villager_life.py `trader`). Every trader
// has six offers — five distinct ones from a pool of 64, then one from a pool
// of 6 — each `xp` 1, multiplier 0.05, paying an orb. The 64 were drawn about
// 15.6 times each: uniform. docs/provenance/cerveaux.md § 9.
//
// The order the five are listed in is NOT established: the villager's
// `HashSet` bucket order (trading.hpp, `listed_before`) is assumed, and named.
//
// ── Coming ──────────────────────────────────────────────────────────────────
//
// The Minecraft Wiki's spawner ("Wandering Trader", Spawning): a delay of
// 24000 ticks counted down 1200 at a time; when it runs out, the chance (25,
// then 50, then 75 %, back to 25 after a trader came) is rolled, then one in
// ten, for one player. Random, and not measured here.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/trading.hpp"
#include "ov/gameplay/villager_state.hpp"
#include "ov/math/random.hpp"

#include <span>
#include <string_view>

namespace ov::gameplay {

/// The kind of `minecraft:wandering_trader`, or null for any other type:
/// `movement_speed` 0.7 (read off a real server's `Attributes`), a creature.
[[nodiscard]] const MobKind* wandering_trader_kind(std::string_view type_name) noexcept;

/// Its goals (the wiki's): float, run from what frightens a villager, face a
/// trading player, stroll at 0.35, look at players. The wander towards a
/// chosen place is not done — named.
void install_wandering_trader_goals(GoalSelector& selector, const MobKind& kind, i32 look_type);

/// The 64 ordinary offers, and the 6 rare.
[[nodiscard]] std::span<const Listing> wandering_trader_offers() noexcept;
[[nodiscard]] std::span<const Listing> wandering_trader_rare_offers() noexcept;

inline constexpr usize kTraderOrdinaryOffers = 5;
/// `DespawnDelay` of a trader that came by itself (the wiki); a summoned one
/// has 0 and never leaves (measured: `DespawnDelay:100` counted down one a
/// tick, gone at 0).
inline constexpr i32 kTraderDespawnDelay = 48000;

/// Make a VillagerState a wandering trader's: no type, no job, the six offers.
void init_wandering_trader(VillagerState& trader, math::LegacyRandomSource& random);

/// One tick of `DespawnDelay`: true when the trader leaves now. A trader that
/// is trading does not leave; 0 means it never does.
[[nodiscard]] bool tick_despawn(VillagerState& trader) noexcept;

/// The spawner's state (`WanderingTraderSpawnDelay`, `WanderingTraderSpawnChance`
/// in level.dat).
struct TraderSpawner {
    i32 tick_delay{1200};
    i32 spawn_delay{24000};
    i32 spawn_chance{25};

    /// One tick. True when a trader should be tried for now (the 1-in-10 and
    /// the place are the caller's); `came` must then be reported.
    [[nodiscard]] bool tick(math::LegacyRandomSource& random, bool allowed) noexcept;
    /// The trader came: the chance falls back to 25.
    void came() noexcept { spawn_chance = 25; }
};

}  // namespace ov::gameplay
