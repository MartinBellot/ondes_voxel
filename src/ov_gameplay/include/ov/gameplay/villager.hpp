// Villagers: who they are, which block gives them a job, when they work and
// sleep, and what they run from.
//
// Not vanilla's brain. Vanilla drives a villager with a "brain" of memories,
// sensors and scheduled activities; this is the goal system every other mob
// here runs, with a handful of villager goals that reproduce the behaviours a
// player can see and that were measured: claiming a job block, taking the
// profession, losing it, working and restocking, running from a zombie. What
// the brain does and this does not — gossip, golems, the bell, meeting,
// breeding, farming, raids — is named in docs/provenance/villageois.md.
//
// ── Layering ────────────────────────────────────────────────────────────────
//
// Goals read a LevelView and the entity world. What they cannot know — the
// time of day and which entity types are hostile — the caller hands them in a
// `VillagerWorld`, as MobContext hands a mob its tempters.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/villager_state.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

// ── Identity ────────────────────────────────────────────────────────────────

[[nodiscard]] std::string_view villager_type_name(VillagerType type) noexcept;
[[nodiscard]] std::string_view profession_name(Profession profession) noexcept;
[[nodiscard]] std::optional<VillagerType> villager_type_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<Profession>   profession_from_name(std::string_view name) noexcept;

/// The profession a job block gives, or nothing for a block that is not one.
/// The thirteen `acquirable_job_site` POI types, each with its block; the
/// leatherworker's is any of the four cauldrons. Each of the thirteen was
/// measured: a villager with no job three blocks from it took that profession.
[[nodiscard]] std::optional<Profession> profession_of_job_block(std::string_view block) noexcept;

/// The block that gives a profession (the empty cauldron for the
/// leatherworker). Empty for none and nitwit.
[[nodiscard]] std::string_view job_block_of(Profession profession) noexcept;

// ── Measured constants ──────────────────────────────────────────────────────

/// How far a villager with no job looks for a free job block, as a distance
/// between block positions. See villageois.md for the bracket.
inline constexpr i32 kJobSearchRadius = 48;
/// The same for a bed. Not measured; the job site's radius.
inline constexpr i32 kBedSearchRadius = 48;
/// A villager takes its profession when its feet are this close to the job
/// block's centre. Not measured; the claim's timing is.
inline constexpr f64 kJobSiteReach = 2.0;
/// Blocks examined per tick by one villager's scan, and the pause after a scan
/// that found nothing.
inline constexpr i32 kScanBudget     = 4096;
inline constexpr i64 kScanPause      = 200;
/// Vertical reach of the scan, either side. The claim campaign has everything
/// at one height; this bounds the cost and is named.
inline constexpr i32 kScanHalfHeight = 8;
/// Measured: `movement_speed` 0.5 (entities.json).
inline constexpr f64 kVillagerSpeedAttribute = 0.5;
/// "Shake head": the unhappy counter's start. Captured: 40, then 39, 38 …
inline constexpr i32 kUnhappyTicks = 40;
/// How long a hit villager runs. Measured bracket: running speed until 2.07 s
/// after the hit, walking speed again by 4.12 s; 60 ticks is inside it.
inline constexpr i32 kHurtPanicTicks = 60;

// ── Time of day ─────────────────────────────────────────────────────────────

/// A villager's day, from the wiki's schedule for an adult: work from 2000,
/// meet from 9000, idle from 11000, rest from 12000 to the next day's 10.
enum class Activity : u8 { Idle, Work, Meet, Rest };
[[nodiscard]] Activity activity_at(i64 day_time) noexcept;

// ── What the goals are handed ───────────────────────────────────────────────

/// A hostile type and how close it must come to make a villager run.
struct HostileSight {
    i32 type{-1};
    f32 distance{8.0F};
};

struct VillagerWorld {
    i64 day_time{6000};
    i64 game_time{0};
    std::span<const HostileSight> hostiles{};
};

// ── The species ─────────────────────────────────────────────────────────────

/// The MobKind of `minecraft:villager`, or null for any other type. Consulted
/// by `mob_kind` after the table of the eight.
[[nodiscard]] const MobKind* villager_mob_kind(std::string_view type_name) noexcept;

/// The goals a villager runs.
void install_villager_goals(GoalSelector& selector, const MobKind& kind, i32 look_type);

/// A fresh villager: the state switched on and its generator seeded.
void init_villager(VillagerState& villager, i64 seed) noexcept;

/// Change the profession, as vanilla's `setVillagerData` does: a different
/// profession undraws the offers.
void set_profession(VillagerState& villager, Profession profession) noexcept;

/// Once per tick, before the goals: the job site still there, the job lost
/// when it is not and nothing was ever traded, the scan for a job and a bed,
/// the level-up timer, the day.
void tick_villager(VillagerState& villager, const entity::EntityState& self_state,
                   entity::EntityHandle self, entity::EntityWorld& entities,
                   const world::LevelView* level, const VillagerWorld* world, bool baby, i64 tick);

/// One step of the scan for free job blocks and beds. Public for the tests.
/// `claimed` answers whether another villager holds a position.
void scan_step(VillagerState& villager, const world::LevelView& level, BlockPos feet,
               bool (*claimed)(entity::EntityWorld&, entity::EntityHandle, BlockPos),
               entity::EntityWorld& entities, entity::EntityHandle self, i32 budget);

/// True when another villager has this position as its job site, its potential
/// job site or its bed.
[[nodiscard]] bool claimed_by_other(entity::EntityWorld& entities, entity::EntityHandle self,
                                    BlockPos pos);

// ── Goals ───────────────────────────────────────────────────────────────────

/// Run from a hostile within its sight distance, or after being hurt.
class VillagerPanicGoal final : public Goal {
public:
    explicit VillagerPanicGoal(f64 speed) noexcept : speed_{speed} {}
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "villager_panic"; }

private:
    [[nodiscard]] bool pick_away(GoalContext& context);

    f64      speed_{0.1};
    BlockPos away_{};
};

/// Stand still and face the player who is trading.
class TradeWithPlayerGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override {
        return GoalFlag::Move | GoalFlag::Look;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "trade"; }
};

/// Find a free job block, walk to it, take its profession.
class AcquireJobSiteGoal final : public Goal {
public:
    explicit AcquireJobSiteGoal(f64 speed) noexcept : speed_{speed} {}
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "acquire_job"; }

private:
    f64 speed_{0.1};
};

/// In working hours, go to the job site; there, restock when allowed.
class WorkAtJobSiteGoal final : public Goal {
public:
    explicit WorkAtJobSiteGoal(f64 speed) noexcept : speed_{speed} {}
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "work"; }

    /// Restocks this goal carried out, for tests.
    [[nodiscard]] i32 restocks() const noexcept { return restocks_; }

private:
    f64 speed_{0.1};
    i32 restocks_{0};
};

/// At rest time, go to the bed and lie in it until morning.
class SleepInBedGoal final : public Goal {
public:
    explicit SleepInBedGoal(f64 speed) noexcept : speed_{speed} {}
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override {
        return GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "sleep"; }

private:
    f64 speed_{0.1};
};

}  // namespace ov::gameplay
