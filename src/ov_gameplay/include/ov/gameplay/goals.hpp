// What a mob wants, and which wants may be served at once.
//
// Vanilla's mobs do not have a state machine. They have a *bag* of goals, each
// with a priority and a set of controls it needs — move, look, jump, target —
// and a selector that every tick works out which subset may run together. Two
// goals that both need the move control cannot; a goal that only looks may run
// alongside one that only walks. That is why a cow can wander and watch a
// player at the same time, and why it stops wandering the moment it panics.
//
// Reproducing that shape rather than a switch statement is not stylistic. It
// is what makes "the mob is fleeing *and* looking at what it flees from"
// expressible without writing a state for it, and it is why adding a behaviour
// is adding a class rather than editing every other behaviour.
//
// ── Layering ────────────────────────────────────────────────────────────────
//
// A goal is handed a `world::LevelView&` and an `entity::EntityWorld&`, never a
// server. That is the rule from CLAUDE.md § 3 and it is what lets every goal
// here be tested against a hand-built stub level, which the unit tests do.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/animal.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/pathfinding.hpp"
#include "ov/gameplay/villager_state.hpp"  // ── villagers ──
#include "ov/gameplay/mob_attack.hpp"      // ── mobs-3 ──
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/world/level.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The controls a goal takes while it runs.
///
/// A bitmask rather than an enum set, because the selector tests "do the flags
/// this goal wants overlap the flags already taken" once per goal per tick and
/// that is an AND.
enum class GoalFlag : u8 {
    None   = 0,
    /// Walking, swimming, flying — anything that sets where the body goes.
    Move   = 1U << 0U,
    /// Where the head points.
    Look   = 1U << 1U,
    /// Jumping.
    Jump   = 1U << 2U,
    /// Choosing what to attack. Held by the target goals only.
    Target = 1U << 3U,
};

[[nodiscard]] constexpr GoalFlag operator|(GoalFlag a, GoalFlag b) noexcept {
    return static_cast<GoalFlag>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr bool overlaps(GoalFlag a, GoalFlag b) noexcept {
    return (static_cast<u8>(a) & static_cast<u8>(b)) != 0;
}

/// Everything a goal is allowed to see and change.
///
/// Assembled by the caller on the stack each tick. Nothing here is owned: the
/// level and the entity world outlive the brain, and the brain outlives the
/// context.
struct MobBrain;

struct GoalContext {
    world::LevelView*     level{nullptr};
    const CollisionWorld* collisions{nullptr};
    entity::EntityWorld*  entities{nullptr};
    entity::EntityHandle  self{entity::kNoEntity};
    MobBrain*             brain{nullptr};
    i64                   tick{0};

    /// The mob's own generator. Explicit and per-mob so that two mobs ticking
    /// in a different order still each get their own stream — a shared source
    /// would make the world depend on iteration order, which is exactly what
    /// determinism forbids.
    math::LegacyRandomSource* random{nullptr};

    // ── husbandry ──
    /// The players an animal may be tempted by, this tick. Empty is legal.
    std::span<const Tempter> tempters{};
    /// Where births, eggs and eaten grass go for the caller to finish. Null:
    /// the animal still does them, and nobody hears.
    std::vector<AnimalEvent>* animal_events{nullptr};
    /// Another mob's brain, or null for an entity that has none. A mate has to
    /// be *in love*, and that is not on an EntityState.
    MobBrain* (*brain_of)(entity::EntityWorld& world, entity::EntityHandle handle){nullptr};
    // ── end husbandry ──

    // ── villagers ──
    /// The time of day and the hostile types (villager.hpp). Declared here by
    /// its elaborated name so this header does not need the villager rules.
    const struct VillagerWorld* villagers{nullptr};

    // ── mobs-3 ──
    /// The players a hostile mob may hunt this tick (mob_attack.hpp). Empty:
    /// no player is ever a target — Peaceful, or nobody in survival.
    std::span<const Quarry> quarries{};
    /// Where a landed swing goes for the caller to finish. Null: the mob
    /// still swings on its cooldown, and nothing is hurt.
    std::vector<MobAttack>* attacks{nullptr};
    /// The protocol id of `minecraft:villager`, for `kVillagerQuarry`. -1:
    /// no villager is ever a target.
    i32 villager_type{-1};

    [[nodiscard]] entity::EntityState*       state() noexcept;
    [[nodiscard]] const entity::EntityState* state() const noexcept;
};

/// The per-mob state the goals share.
///
/// Deliberately a plain struct: a goal that wanted to hide something from
/// another goal would be a goal that has become a state machine.
struct MobBrain {
    /// What the mob is trying to walk to, if anything, and the route there.
    PathFinder   finder;
    Path         path;
    PathFollower follower;

    /// Recomputed at most this often. A path per mob per tick is the single
    /// easiest way to make a server with two hundred mobs stop ticking, and
    /// vanilla does not do it either.
    i64 next_path_tick{0};

    /// Where the head is being pointed. Set by whichever look goal is running
    /// and cleared by the mob at the top of each tick, so a goal that has
    /// quietly stopped cannot leave the head aimed at something.
    Vec3d look_at{};
    bool  has_look{false};

    /// The mob's quarry. `kNoEntity` when it has none.
    entity::EntityHandle target{entity::kNoEntity};
    i64                  target_forgotten_at{0};
    /// ── mobs-3 ── The quarry when it is a player: its wire id, 0 for none.
    /// Exclusive with `target` — players are not in the entity world.
    i32 target_player{0};

    /// Movement speed multiplier the running move goal asked for. Applied by
    /// the mob's own tick, not by the goal, so two goals cannot both push.
    f64  speed{0.0};
    bool wants_move{false};
    bool wants_jump{false};

    /// How the body moves and how big it is, from the measured tables.
    MobSize       size{};
    PathAbilities abilities{};

    /// The parent this mob follows. Here rather than in the goal so that a
    /// calf keeps its mother across the goal stopping and starting again.
    entity::EntityHandle parent{entity::kNoEntity};
    // ── husbandry ──
    /// Age, love, fleece, saddle, egg: see animal.hpp. The goals read it; the
    /// mob's own tick ages it.
    AnimalState animal{};
    // ── villagers ──
    /// Type, profession, level, claims, offers: see villager_state.hpp.
    /// `active` is false on every mob that is not a villager.
    VillagerState villager{};

    explicit MobBrain(usize path_capacity = 2048) : finder{path_capacity} {
        path.steps.reserve(256);
    }
};

/// One thing a mob might want to do.
class Goal {
public:
    Goal()                       = default;
    Goal(const Goal&)            = delete;
    Goal& operator=(const Goal&) = delete;
    Goal(Goal&&)                 = delete;
    Goal& operator=(Goal&&)      = delete;
    virtual ~Goal()              = default;

    /// May this goal start now?
    [[nodiscard]] virtual bool can_use(GoalContext& context) = 0;

    /// May a running goal keep going? Defaults to `can_use`, which is right for
    /// most and wrong for the ones that must finish what they started — a mob
    /// walking somewhere does not stop because the reason went away mid-step.
    [[nodiscard]] virtual bool can_continue_to_use(GoalContext& context) {
        return can_use(context);
    }

    virtual void start(GoalContext&) {}
    virtual void stop(GoalContext&) {}
    virtual void tick(GoalContext&) {}

    /// Which controls this goal holds while it runs.
    [[nodiscard]] virtual GoalFlag flags() const noexcept = 0;

    /// May a higher-priority goal take this one's controls away mid-flight?
    ///
    /// True for almost everything, and the default is what makes a panicking
    /// animal stop wandering on the tick it is frightened rather than at the
    /// end of whatever stroll it had begun. A goal that must finish what it
    /// started says so by returning false.
    [[nodiscard]] virtual bool interruptible() const noexcept { return true; }

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

/// The priority queue, and the rule for who runs.
///
/// The rule, in order, once per tick:
///
///   1. every running goal whose `can_continue_to_use` is false stops, and
///      releases its flags;
///   2. every stopped goal is offered a start, in priority order. It starts if
///      it can be used and every control it wants is either free or held by a
///      goal that is both lower-priority and interruptible — and in that second
///      case the holder is stopped first.
///
/// That eviction is the part worth being careful about. A selector that merely
/// waited for a control to be released would let a chicken finish its stroll
/// before panicking, which is visibly not what the game does; the priority
/// number only means anything because a lower one can take a control away from
/// a higher one that already has it.
///
/// The flags are checked *before* `can_use` is called, deliberately: `can_use`
/// draws random numbers on several goals, and testing it for a goal that could
/// not have started anyway would make the mob's RNG stream depend on which
/// other goals happened to be running.
class GoalSelector {
public:
    /// Lower priority runs first. Ties keep insertion order, which makes the
    /// tick deterministic without the caller having to make priorities unique.
    void add(i32 priority, std::unique_ptr<Goal> goal);

    void tick(GoalContext& context);

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

    /// The names of the goals running right now, for tests and for logs.
    void running(std::vector<std::string_view>& out) const;

    [[nodiscard]] bool is_running(std::string_view name) const;

    /// The goal with this name, or null. Borrowed: the selector owns it.
    ///
    /// Exists so a mob can reach a goal it has to feed from outside — a panic
    /// goal has to be told that something hurt the mob, and a damage event is
    /// not something a goal can see for itself.
    [[nodiscard]] Goal* find(std::string_view name) noexcept;

private:
    struct Entry {
        i32                   priority{0};
        u32                   order{0};
        std::unique_ptr<Goal> goal;
        bool                  running{false};
    };

    /// Which entry holds each control, or -1. Indexed by the bit position of
    /// GoalFlag, so four slots: move, look, jump, target.
    static constexpr usize kFlagCount = 4;

    std::vector<Entry> entries_;
    u32                next_order_{0};
};

// ── The goals themselves ────────────────────────────────────────────────────
//
// Enough of them for the eight mobs this milestone ships to be credible, and
// no more. Each is one behaviour; a mob is the list of them it was given.

/// Keeps a mob's head above water. Priority 0 on everything that drowns.
class FloatGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Jump; }
    [[nodiscard]] std::string_view name() const noexcept override { return "float"; }
};

/// Walk somewhere, at random, now and then.
///
/// The floor of mob behaviour: without it a cow stands exactly where it spawned
/// forever, which reads as a broken server rather than a calm one.
class RandomStrollGoal final : public Goal {
public:
    /// `interval` is the mean number of ticks between attempts; the draw is
    /// `next_int(interval)`, so a mob does not move on a metronome.
    explicit RandomStrollGoal(f64 speed = 1.0, i32 interval = 120, i32 radius = 10) noexcept
        : speed_{speed}, interval_{interval}, radius_{radius} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "stroll"; }

private:
    f64      speed_{1.0};
    i32      interval_{120};
    i32      radius_{10};
    BlockPos wanted_{};
};

/// Look at nothing in particular. Costs one control and buys most of the
/// difference between "a model" and "a creature".
class RandomLookGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Look; }
    [[nodiscard]] std::string_view name() const noexcept override { return "random_look"; }

private:
    f64 dx_{0.0};
    f64 dz_{0.0};
    i32 remaining_{0};
};

/// Watch the nearest entity of a given type within a radius.
class LookAtEntityGoal final : public Goal {
public:
    /// `type` is a protocol entity-type id; -1 watches anything.
    LookAtEntityGoal(i32 type, f64 radius, f32 probability) noexcept
        : type_{type}, radius_{radius}, probability_{probability} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Look; }
    [[nodiscard]] std::string_view name() const noexcept override { return "look_at"; }

private:
    i32                  type_{-1};
    f64                  radius_{8.0};
    f32                  probability_{0.02F};
    entity::EntityHandle watched_{entity::kNoEntity};
    i32                  remaining_{0};
};

/// Walk to the target and hit it.
///
/// Holds Move and Look together: a mob that chases without turning to face what
/// it chases is the single most obvious sign that a goal system is not really
/// one.
class MeleeAttackGoal final : public Goal {
public:
    /// `hold_at` (── mobs-2 ──): a ranged attacker stops closing in inside
    /// this distance — 15 for a skeleton's bow, 10 for a witch. 0 for melee.
    /// `strikes` (── mobs-3 ──): a swing in reach becomes a `MobAttack`. False
    /// for the mobs whose attack is something else — a creeper swells, a
    /// skeleton shoots, a witch throws.
    explicit MeleeAttackGoal(f64 speed = 1.0, i32 cooldown = 20, f64 hold_at = 0.0,
                             bool strikes = true) noexcept
        : speed_{speed}, cooldown_{cooldown}, hold_at_{hold_at}, strikes_{strikes} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override {
        return GoalFlag::Move | GoalFlag::Look;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "melee"; }

    /// Ticks left before this goal will swing again. For tests.
    [[nodiscard]] i32 cooldown_left() const noexcept { return ticks_until_attack_; }

private:
    f64 speed_{1.0};
    i32 cooldown_{20};
    f64  hold_at_{0.0};  // ── mobs-2 ──
    bool strikes_{true};  // ── mobs-3 ──
    i32 ticks_until_attack_{0};
    i64 next_repath_{0};
};

/// Pick the nearest entity of a type as a target.
///
/// Holds the Target control alone, so it runs alongside whatever is moving the
/// body — which is the whole reason `Target` is a separate flag.
class NearestAttackableTargetGoal final : public Goal {
public:
    NearestAttackableTargetGoal(i32 type, f64 radius, bool must_see = true) noexcept
        : type_{type}, radius_{radius}, must_see_{must_see} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Target; }
    [[nodiscard]] std::string_view name() const noexcept override { return "target"; }

private:
    i32                  type_{-1};
    f64                  radius_{16.0};
    bool                 must_see_{true};
    entity::EntityHandle found_{entity::kNoEntity};
    i32                  found_player_{0};  // ── mobs-3 ──
};

/// ── mobs-3 ── The quarry carrying a wire id, or null.
[[nodiscard]] const Quarry* find_quarry(std::span<const Quarry> quarries, i32 network_id) noexcept;

/// Run away from whatever last hurt this mob.
class PanicGoal final : public Goal {
public:
    explicit PanicGoal(f64 speed = 1.25, i32 radius = 7) noexcept
        : speed_{speed}, radius_{radius} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "panic"; }

    /// Set by whatever applies damage. A goal cannot see a damage event, so the
    /// event is recorded here and the goal reads it — which also means a test
    /// can panic a cow without hitting it.
    void frighten(i32 ticks) noexcept { frightened_ = ticks; }

    [[nodiscard]] i32 frightened() const noexcept { return frightened_; }

private:
    f64      speed_{1.25};
    i32      radius_{7};
    i32      frightened_{0};
    BlockPos away_{};
};

/// Get out of the sun. What makes a skeleton hide under a tree at dawn.
class AvoidSunGoal final : public Goal {
public:
    explicit AvoidSunGoal(f64 speed = 1.0) noexcept : speed_{speed} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "avoid_sun"; }

    /// Whether the sky is currently a problem. Supplied from outside because a
    /// LevelView knows blocks and not the time of day or the weather.
    void set_daylight(bool daylight) noexcept { daylight_ = daylight; }

private:
    f64      speed_{1.0};
    bool     daylight_{false};
    BlockPos shade_{};
};

/// A baby stays near its parent.
class FollowParentGoal final : public Goal {
public:
    explicit FollowParentGoal(f64 speed = 1.0, f64 radius = 8.0) noexcept
        : speed_{speed}, radius_{radius} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "follow_parent"; }

private:
    f64 speed_{1.0};
    f64 radius_{8.0};
};

/// Two animals in love find each other and produce a third.
///
/// ── husbandry ── The mate is an adult of the same type, *in love*, whose box
/// grown by `reach` on every axis meets this one's (measured: a pair 8.5 blocks
/// apart finds each other and 9 does not — the cube, not a sphere of 8; see
/// docs/provenance/elevage.md). The birth itself is an `AnimalEvent` the caller
/// finishes: what a calf *is* is the registry's answer, and the caller holds it.
class BreedGoal final : public Goal {
public:
    explicit BreedGoal(f64 speed = 1.0, f64 reach = 8.0) noexcept
        : speed_{speed}, reach_{reach} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override {
        return GoalFlag::Move | GoalFlag::Look;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "breed"; }

    [[nodiscard]] entity::EntityHandle mate() const noexcept { return mate_; }

private:
    f64                  speed_{1.0};
    f64                  reach_{8.0};
    entity::EntityHandle mate_{entity::kNoEntity};
    i32                  loops_{0};
};

/// Ask the brain to walk to a block. Shared by every move goal, so that
/// "recompute the path at most every N ticks" is written once.
///
/// Returns false when no route exists at all, which the caller reads as "this
/// goal cannot be used".
[[nodiscard]] bool move_to(GoalContext& context, BlockPos destination, f64 speed,
                          f32 max_range = 64.0F);

/// A type id meaning "this mob has no legitimate quarry here".
///
/// Distinct from -1, which means *anything*. The difference is not academic: a
/// hostile mob given -1 targets the nearest entity of any kind, so a skeleton
/// and a spider standing two blocks apart lock onto each other and never move
/// again. Measured on our own server before this existed — six of eight mobs
/// walked, and those two travelled exactly zero blocks in eighty seconds.
inline constexpr i32 kNoQuarry = -2;

/// The nearest live entity of a type within a radius, or `kNoEntity`.
///
/// `type` of -1 matches anything; `kNoQuarry` (or any value below -1) matches
/// nothing. Ties are broken by insertion order, which is the entity world's own
/// order, so two mobs at the same distance are chosen deterministically.
[[nodiscard]] entity::EntityHandle nearest_entity(const entity::EntityWorld& world,
                                                  entity::EntityHandle self, i32 type,
                                                  f64 radius);

/// Is there a clear line between two entities' eyes?
///
/// The predicate every vanilla target goal is gated on, and the reason the mob
/// oracle in scripts/measure_mobs.py has to hurt a zombie to make it chase
/// something through a maze: a wall denies sight, and a target that cannot be
/// seen is dropped the same tick.
[[nodiscard]] bool has_line_of_sight(const CollisionWorld& collisions,
                                     const entity::EntityState& from,
                                     const entity::EntityState& to);

}  // namespace ov::gameplay
