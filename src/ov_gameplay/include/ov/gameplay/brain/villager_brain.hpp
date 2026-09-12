// A villager's brain: its sensors, its activities and the behaviours in each —
// the framework of brain.hpp filled in for `minecraft:villager`.
//
// ── What it does ────────────────────────────────────────────────────────────
//
//   core   the schedule consulted, panic triggered, waking, the job site and
//          the bed and the bell claimed and remembered, the walk target
//          walked to, the look target looked at, the trading player faced
//   idle   strolling, meeting another villager (gossip), finding a mate and
//          breeding when there is food and a free bed
//   work   going to the job site, working there (restocking, last_worked_at_poi),
//          farmers harvesting and replanting
//   meet   going to the bell, socialising there (gossip, golem summoning by five)
//   rest   going to the bed, sleeping (last_slept, last_woken)
//   panic  running from what frightens it; three who slept recently summon a golem
//
// What reaches past a LevelView — a baby to spawn, a golem to spawn, a crop to
// break or plant — is a `VillagerEvent` the server finishes, as births and
// eaten grass are AnimalEvents.
//
// ── Provenance ──────────────────────────────────────────────────────────────
//
// The shape — which behaviour is in which activity — is the Minecraft Wiki's
// ("Villager": schedules, gossiping, breeding, iron golem summoning; "Iron
// Golem": spawning). Every number a player can see was measured on a real
// 1.20.1 server by scripts/measure_villager_life.py; what was not is said at
// the constant. docs/provenance/cerveaux.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/brain/brain.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {
struct VillagerState;
}

namespace ov::gameplay::brain {

// ── Constants ───────────────────────────────────────────────────────────────

/// A villager wants a golem only if it slept less than this many ticks ago.
inline constexpr i64 kGolemSleptWithin = 24000;
/// After a golem is seen (or summoned), none is summoned for this long.
inline constexpr i64 kGolemDetectedTicks = 600;
/// How many villagers that want a golem it takes: panicking, gossiping.
inline constexpr i32 kGolemPanicVillagers  = 3;
inline constexpr i32 kGolemGossipVillagers = 5;
/// The box, either side of the villager, its fellows are counted in.
inline constexpr f64 kGolemCountReach = 10.0;
/// The golem is tried this many times, this far round the villager.
inline constexpr i32 kGolemSpawnAttempts   = 10;
inline constexpr i32 kGolemSpawnHorizontal = 8;
inline constexpr i32 kGolemSpawnVertical   = 6;
/// A golem this close is detected (the golem sensor).
inline constexpr f64 kGolemDetectReach = 16.0;
/// Two villagers gossip at most once in this many ticks.
inline constexpr i64 kGossipCooldown = 1200;
/// Gossiping happens within this distance (squared 5, vanilla's).
inline constexpr f64 kGossipReachSq = 5.0;
/// Food points a villager must have to breed, and what breeding eats.
inline constexpr i32 kBreedFood = 12;
/// A mate is looked for within this many blocks.
inline constexpr f64 kMateReach = 8.0;
/// A free bed for the baby is looked for within this many blocks.
inline constexpr i32 kBabyBedReach = 48;
/// A bell is a meeting point within this many blocks.
inline constexpr i32 kMeetingSearchRadius = 48;

/// Food points of what a villager eats: bread 4, carrots, potatoes and
/// beetroots 1 (the wiki's; the breeding campaign measures the threshold).
[[nodiscard]] i32 food_points(std::string_view item) noexcept;
/// What a villager picks up and keeps: the four foods, wheat, and the seeds a
/// farmer plants.
[[nodiscard]] bool wanted_item(std::string_view item) noexcept;
/// The canonical static name of a wanted item (so an inventory slot can keep
/// a view), or empty.
[[nodiscard]] std::string_view wanted_item_name(std::string_view item) noexcept;

/// Food points eaten plus in the pockets.
[[nodiscard]] i32 food_available(const VillagerState& villager) noexcept;
/// Adult, and enough food: `canBreed`.
[[nodiscard]] bool can_breed(const VillagerState& villager, bool baby) noexcept;
/// Eat from the pockets until `kBreedFood` is reached, then digest it.
void eat_for_breeding(VillagerState& villager) noexcept;
/// Add to the pockets: merged into a slot of the same item, else the first
/// empty one. What does not fit is returned.
i32 pocket(VillagerState& villager, std::string_view item, i32 count) noexcept;
/// Take from the pockets. True when there was one.
bool take_from_pocket(VillagerState& villager, std::string_view item) noexcept;

// ── Events ──────────────────────────────────────────────────────────────────

enum class VillagerEventKind : u8 {
    /// Two villagers had a baby: `self` and `other` the parents, `block` the
    /// baby's bed (its home).
    Birth,
    /// A golem is summoned at `at`.
    SummonGolem,
    /// A farmer breaks the ripe crop at `block` (the crop goes in its pockets).
    Harvest,
    /// A farmer plants `item` at `block` (on farmland).
    Plant,
    /// Breeding failed for want of a bed: entity event 13 (angry).
    NoBed,
};

struct VillagerEvent {
    VillagerEventKind    kind{VillagerEventKind::Birth};
    entity::EntityHandle self{entity::kNoEntity};
    entity::EntityHandle other{entity::kNoEntity};
    BlockPos             block{};
    Vec3d                at{};
    std::string_view     item;
};

// ── The brain ───────────────────────────────────────────────────────────────

/// A villager's brain, built once with the mob.
[[nodiscard]] std::unique_ptr<Brain> make_villager_brain(math::LegacyRandomSource& random);

/// Villagers that want a golem round `self`, `self` included; when there are
/// `required`, a golem position is sought and, found, `SummonGolem` is sent
/// and every one of them remembers a golem for `kGolemDetectedTicks`. True
/// when a golem was asked for.
bool spawn_golem_if_needed(BrainContext& context, i32 required);

/// Does this villager want a golem: slept within `kGolemSleptWithin`, and no
/// golem detected recently.
[[nodiscard]] bool wants_golem(const Memories& memories, i64 game_time) noexcept;

/// Where a summoned golem may stand, searched as the wiki describes: up to
/// `kGolemSpawnAttempts` columns within ±8 horizontally, each scanned from +6
/// down to -6 for a solid, non-glass floor under three free blocks.
[[nodiscard]] std::optional<BlockPos> golem_spawn_position(const world::LevelView& level,
                                                           BlockPos origin,
                                                           math::LegacyRandomSource& random);

}  // namespace ov::gameplay::brain
