// Mob behaviour, as the entity world sees it.
//
// ov_entity holds state and a `tick` it knows nothing about; what an entity
// actually *does* is a rule, and rules live here. The bridge is
// `TickContext::user`, an opaque pointer ov_entity cannot name — the same idiom
// CollisionWorld already uses for the world it reads. That is what lets layer 8
// tick behaviour written at layer 9 without either knowing about the other.
#pragma once

#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/animal.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/gameplay/pathfinding.hpp"
#include "ov/gameplay/spawning.hpp"
#include "ov/gameplay/walk_speed.hpp"  // ── mobs-2 ──
#include "ov/math/random.hpp"
#include "ov/world/level.hpp"

#include <memory>
#include <string_view>

namespace ov::gameplay {

/// What mob behaviour is handed each tick, through TickContext::user.
///
/// The caller builds one on the stack per tick and points the context at it.
/// Nothing here is owned: the collision world is a view over blocks the tick
/// thread already holds, and so is the level.
struct MobContext {
    const CollisionWorld* world{nullptr};

    /// The blocks a goal reads. Null is legal — a mob with no level cannot
    /// path, and falls without thinking, which is what `FallingMob` did before
    /// any of this existed.
    world::LevelView* level{nullptr};

    /// True while the sky would burn an undead mob. Supplied by the caller
    /// because a LevelView knows blocks and not the time of day.
    bool daylight{false};

    // ── husbandry ──
    /// The players an animal may follow for their food. Empty is legal.
    std::span<const Tempter> tempters{};
    /// Births, eggs and eaten grass, for the caller to finish. Null is legal.
    std::vector<AnimalEvent>* animal_events{nullptr};

    // ── villagers ──
    /// The time of day and the hostile types. Null: villagers neither work,
    /// sleep nor run from anything.
    const VillagerWorld* villagers{nullptr};

    // ── mobs-3 ── the players hostile mobs hunt, where their swings go, and
    // the villager's type id (goals.hpp, GoalContext).
    std::span<const Quarry> quarries{};
    std::vector<MobAttack>* attacks{nullptr};
    i32                     villager_type{-1};

    // ── tame ── owners and the sink for taming decisions (tame_state.hpp).
    const TameWorld* tame_world{nullptr};
};

/// Recover the context, or null if the caller did not provide one.
///
/// A free function rather than a cast at every call site, so that the one
/// unchecked conversion in the design has exactly one home.
[[nodiscard]] inline const MobContext* mob_context(const entity::TickContext& context) noexcept {
    return static_cast<const MobContext*>(context.user);
}

/// The behaviour every entity has before it has any other: it falls.
///
/// Deliberately the whole of it. Vanilla's Mob is a tower of goals over a
/// LivingEntity that already knows how to fall, and this is that floor — a
/// zombie that chases a player does it by adding horizontal velocity on top of
/// this, not by replacing it.
class FallingMob final : public entity::IEntityLogic {
public:
    explicit FallingMob(EntityMotionConstants constants = {}) noexcept
        : constants_{constants} {}

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "falling"; }

private:
    EntityMotionConstants constants_;
};

/// How a species differs from every other species.
///
/// Everything here is either measured (the box, the speed) or a list of goals.
/// There is no per-species code: a zombie and a cow run the same `Mob::tick`
/// and differ only in what is in this struct, which is the point — a ninth
/// species is a table entry, not a class.
struct MobKind {
    std::string_view type_name;
    MobCategory      category{MobCategory::Monster};

    // ── mobs-2 ── The walk, as the game computes it (walk_speed.hpp): the
    // `movement_speed` attribute times the running goal's modifier, then the
    // law `2.15859 · s²`. Every attribute is the measured one
    // (normalized/entities.json) and every modifier was read off a real server
    // goal by goal — docs/provenance/mobs-2.md § 1. This replaces the old
    // `walk_speed`, which was the attribute halved and 16 % fast for a cow.
    f64 movement_speed{0.25};
    f64 stroll{1.0};
    f64 chase{1.0};
    f64 panic{1.25};
    f64 follow_parent{1.1};
    f64 avoid_sun{1.0};
    /// A ranged attacker stops approaching inside this many blocks. 0: melee.
    f64 hold_at{0.0};

    /// Blocks per tick under a goal with this modifier, on ordinary ground.
    [[nodiscard]] constexpr f64 speed(f64 modifier) const noexcept {
        return walk_blocks_per_tick(movement_speed * modifier);
    }

    bool opens_doors{false};
    bool avoids_sun{false};
    /// Attacks players on sight.
    bool hostile{false};
    /// Runs when hurt.
    bool panics{false};
    /// Can be bred, and follows a parent while young.
    bool breeds{false};
    // ── mobs-3 ──
    /// A swing in reach hurts. False for a creeper (it swells), a skeleton, a
    /// stray and a witch (they shoot or throw), a slime (it hurts by contact,
    /// not done — named in mobs-3.md).
    bool melee{true};
    /// Hunts villagers too, without line of sight: the zombie family.
    bool hunts_villagers{false};
    /// How far a target is sought and kept: the measured `follow_range`
    /// attribute (entities.json) — 35 for the zombie family, 64 for the
    /// enderman, 16 for every other hostile here.
    f64 follow_range{16.0};
};

/// The species this milestone ships. Eight, and the value is in the systems.
[[nodiscard]] std::span<const MobKind> mob_kinds() noexcept;

/// The kind for a registry name, or null. Named and refused rather than given a
/// plausible default: a mob whose behaviour was guessed is worse than one that
/// failed to spawn, because only one of them says so.
[[nodiscard]] const MobKind* mob_kind(std::string_view type_name) noexcept;

/// A mob with a brain.
///
/// Owns its goal selector, its brain and its own random source. One per entity:
/// two mobs sharing a generator would make the world depend on the order they
/// happen to be ticked in, which is exactly what determinism forbids.
class Mob final : public entity::IEntityLogic {
public:
    /// `seed` should differ per mob. The caller derives it from the entity's
    /// wire id, so a world replayed from the same sequence of spawns behaves
    /// identically.
    /// `quarry_type` is the entity type a hostile mob hunts — the caller's
    /// `minecraft:player` id in a server, `kNoQuarry` when there is nothing it
    /// should be attacking. Defaulting it to "anything" is what made a skeleton
    /// and a spider stare at each other instead of wandering.
    Mob(const MobKind& kind, f32 width, f32 height, i64 seed,
        i32 quarry_type = kNoQuarry);

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return kind_->type_name; }

    /// For tests and for the server: what the mob is doing right now.
    [[nodiscard]] const GoalSelector& goals() const noexcept { return goals_; }
    [[nodiscard]] const MobBrain&     brain() const noexcept { return brain_; }
    [[nodiscard]] const MobKind&      kind() const noexcept { return *kind_; }

    /// Tell the mob something hurt it. Panicking animals read this.
    void frighten(i32 ticks) noexcept;

    /// What the mob is currently trying to reach, or kNoEntity.
    ///
    /// Read-only on purpose: applying the damage is the caller's business, not
    /// this module's — ov_gameplay knows what a hit does but not who is allowed
    /// to be told about it.
    [[nodiscard]] entity::EntityHandle target() const noexcept { return brain_.target; }

    // ── husbandry ──
    /// The brain, for the server to feed, shear or saddle what is in it.
    [[nodiscard]] MobBrain& mutable_brain() noexcept { return brain_; }
    /// Make this mob a newborn: `Age` -24000 and half its box. `state` is its
    /// own state, still at the adult size the registry gave it.
    void make_baby(entity::EntityState& state) noexcept;
    /// Set an age read from anywhere (a test, an NBT one day). Resizes.
    void set_age(entity::EntityState& state, i32 age) noexcept;

    // ── noai ── `NoAI`: no brain and no physics. The position stays, no
    // gravity builds up, the stored velocity only decays by 0.98 a tick
    // (measured, docs/provenance/apprivoisement.md § 8.3). Age, love, eggs and
    // a wolf's anger still tick, as they do in vanilla.
    void               set_no_ai(bool no_ai) noexcept { no_ai_ = no_ai; }
    [[nodiscard]] bool no_ai() const noexcept { return no_ai_; }

private:
    /// Age, love and eggs, once per tick. breeding.cpp.
    void tick_husbandry(entity::EntityState& state, entity::EntityHandle self,
                        const MobContext& context);

    const MobKind*           kind_{nullptr};
    GoalSelector             goals_;
    MobBrain                 brain_;
    math::LegacyRandomSource random_;
    EntityMotionConstants    motion_{};
    PanicGoal*               panic_{nullptr};
    /// The size the registry gave this type, which a baby grows back to.
    f32 adult_width_{0.0F};
    f32 adult_height_{0.0F};
    f32 adult_eye_{-1.0F};
    /// The box is a baby's. Separate from `age < 0` because food and grass
    /// can move the age to 0 between two ticks, and the box must follow.
    bool baby_box_{false};
    bool no_ai_{false};  // ── noai ──
};

/// Build the goal list for a kind.
///
/// Public so a test can inspect the arrangement without spawning anything, and
/// so the priorities are in one readable place rather than scattered through a
/// constructor.
/// `look_type` is what the mob glances at (-1 for anything); `quarry_type` is
/// what a hostile one hunts (`kNoQuarry` for nothing). Two parameters and not
/// one, because a cow should watch a player it must not attack and a skeleton
/// should not hunt the spider it is happy to look at.
void install_goals(GoalSelector& selector, const MobKind& kind, i32 look_type,
                   i32 quarry_type);

}  // namespace ov::gameplay
