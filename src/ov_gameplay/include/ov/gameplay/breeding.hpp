// Husbandry: how farm animals grow up, fall in love, breed, and are worked.
//
// Rules only, on the state an animal carries (animal.hpp) and on the entity
// world. Everything that needs a player's inventory, a spawn or a packet is the
// server's (ov_server/src/husbandry.{hpp,cpp}); this layer decides and the
// caller carries out, as for every other system in ov_gameplay.
//
// ── Provenance ──────────────────────────────────────────────────────────────
//
// Every constant below was measured against a real 1.20.1 server by
// scripts/measure_husbandry.py, and docs/provenance/elevage.md carries the
// numbers. The few that were not are said to be not measured, where they are.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/animal.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/math/random.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

// ── Measured constants ──────────────────────────────────────────────────────

/// A newborn's `Age`. Measured on a bred calf and on a hatched chick.
inline constexpr i32 kBabyAge = -24000;
/// `InLove` right after feeding. Measured.
inline constexpr i32 kLoveTicks = 600;
/// Both parents' `Age` right after a birth. Measured.
inline constexpr i32 kParentCooldown = 6000;
/// From feeding an adjacent pair to the calf: 59 to 60 ticks over five trials.
/// Taken as 60 ticks of the breed goal running.
inline constexpr i32 kMateTicks = 60;
/// A baby's box against its adult's. Measured exactly 0.5 on cow, sheep, pig
/// and chicken; the movement_speed attribute is unchanged. The eye height is
/// *not* always half — see AnimalKind::baby_eye_height.
inline constexpr f32 kBabyScale = 0.5F;
/// Entity Event status when an animal falls in love (the hearts). Captured.
inline constexpr i8 kLoveEventStatus = 18;
/// How far a mate is looked for: the searcher's box grown by this on every
/// axis. See BreedGoal and docs/provenance/elevage.md for what was measured.
inline constexpr f64 kMateReach = 8.0;

/// Blocks per tick of a mob walking on level ground, from its speed — the
/// `movement_speed` attribute times the goal's modifier.
///
/// Measured as a law, not a ratio: seven walking speeds on a real server (a
/// chasing zombie, a strolling cow, five tempted animals from 0.20 to 0.30)
/// all fit `v = 2.1586 · s²` within 0.5 %. The halving mobs.md uses for the
/// walk (`v = attribute / 2`) is this law's tangent at the zombie's 0.23, and
/// it is 16 % fast for a strolling cow — which is why the husbandry goals take
/// their speed from here.
inline constexpr f64 kWalkLaw = 2.1586;

[[nodiscard]] constexpr f64 walk_blocks_per_tick(f64 attribute_times_modifier) noexcept {
    return kWalkLaw * attribute_times_modifier * attribute_times_modifier;
}

// ── Species ─────────────────────────────────────────────────────────────────

/// What makes one farm animal different from another, as far as husbandry is
/// concerned. A table entry, as MobKind is.
struct AnimalKind {
    std::string_view type_name;
    /// The items that breed it and grow its young. 1.20.1 has no `*_food`
    /// item tags for these species, so each list is measured, item by item
    /// (18 candidates per species).
    std::span<const std::string_view> food;
    /// TemptGoal's speed multiplier over the walk speed.
    f64  tempt_speed{1.0};
    bool lays_eggs{false};
    bool shearable{false};
    bool milkable{false};
    bool saddleable{false};
    /// A baby's eye height. **Not** half the adult's: measured 0.665 for a
    /// calf (adult 1.3), 0.2975 for a chick (adult 0.644), while the lamb and
    /// the piglet are exactly half. The boxes, by contrast, are all half.
    f32 baby_eye_height{0.0F};
};

/// The husbandry entry for a type, or null: not every mob is livestock.
[[nodiscard]] const AnimalKind* animal_kind(std::string_view type_name) noexcept;

[[nodiscard]] bool is_food(const AnimalKind& kind, std::string_view item) noexcept;

// ── Feeding ─────────────────────────────────────────────────────────────────

enum class FeedResult : u8 {
    /// Not this species' food: the interaction is someone else's.
    NotFood,
    /// Food, refused: already in love, or a parent still cooling down. The
    /// item is not consumed. Measured both ways.
    Refused,
    /// An adult fell in love.
    Love,
    /// A baby grew.
    Grew,
};

/// Ticks a fed baby gains: a tenth of what is left, counted in whole seconds.
///
/// Measured at twenty ages from -24000 to -1: -23994 gained 2380, -1995 gained
/// 180, -395 gained 20, -194 gained 0. That is `floor(-age / 200) * 20`, which
/// is also the game's float `(int)(-age / 20 * 0.1F) * 20` for every age there
/// is — 0.1F is slightly above a tenth, so the product never floors low.
[[nodiscard]] constexpr i32 feeding_growth(i32 age) noexcept {
    return age >= 0 ? 0 : (-age / 200) * 20;
}

/// Feed one item. Changes `animal` and says what happened; consuming the item
/// and telling clients is the caller's.
FeedResult feed(AnimalState& animal, const AnimalKind& kind, std::string_view item,
                i32 player) noexcept;

/// Age up by `ticks`, never past adulthood.
void age_up(AnimalState& animal, i32 ticks) noexcept;

// ── Random draws ────────────────────────────────────────────────────────────
//
// Each is one call so the order of draws is written once.

/// Wool from one shearing: 1 to 3. Measured on 150 sheep: 55 / 44 / 51.
[[nodiscard]] i32 wool_count(math::LegacyRandomSource& random) noexcept;
/// The orb a birth drops: 1 to 7. Measured on 60 births, every value seen.
[[nodiscard]] i32 breeding_xp(math::LegacyRandomSource& random) noexcept;
/// Ticks to the next egg: 6000 to 11999. Measured on 200 fresh chickens
/// (6004..11992, mean 8972) and on 40 that had just laid.
[[nodiscard]] i32 egg_interval(math::LegacyRandomSource& random) noexcept;

// ── Colours ─────────────────────────────────────────────────────────────────

/// The sixteen colours in the game's dye order, `white` … `black`.
[[nodiscard]] std::span<const std::string_view, 16> colour_names() noexcept;

/// The colour a dye item sets, or nullopt for anything that is not a dye.
[[nodiscard]] std::optional<i8> dye_colour(std::string_view item) noexcept;

/// The dye two dyes craft into, or nullopt. The nine two-dye shapeless recipes
/// of the 1.20.1 datapack; `test_breeding.cpp` checks this table against them.
[[nodiscard]] std::optional<i8> mixed_colour(i8 a, i8 b) noexcept;

/// A lamb's colour: the mix when the two dyes craft into one, otherwise one of
/// the parents' at random. Measured over all 256 ordered pairs.
[[nodiscard]] i8 offspring_colour(i8 a, i8 b, math::LegacyRandomSource& random) noexcept;

// ── The body ────────────────────────────────────────────────────────────────

/// Shrink an entity to a baby's box. `adult` is its size as the registry
/// measured it.
void baby_box(entity::EntityState& state, f32 adult_width, f32 adult_height,
              f32 adult_eye) noexcept;

// ── Goals ───────────────────────────────────────────────────────────────────

/// Follow a player holding this species' food.
///
/// Holds Move and Look. Starts when a tempter within `range` holds food in
/// either hand, walks to it, stops `stop_distance` short of it.
class TemptGoal final : public Goal {
public:
    TemptGoal(const AnimalKind& kind, f64 speed) noexcept : kind_{&kind}, speed_{speed} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override {
        return GoalFlag::Move | GoalFlag::Look;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "tempt"; }

    /// Measured: tempted at 10 blocks, not at 11 (see elevage.md § tempt).
    static constexpr f64 kRange = 10.0;
    /// Measured: the animal stops about this far from the player.
    static constexpr f64 kStopDistance = 2.5;
    /// Ticks before a goal that stopped may start again. Not measured.
    static constexpr i32 kCalmDown = 100;

private:
    [[nodiscard]] const Tempter* find(GoalContext& context) const;

    const AnimalKind* kind_;
    f64               speed_{1.0};
    Vec3d             player_{};
    i32               calm_down_{0};
};

/// A sheep eats the grass under it, and its wool grows back.
///
/// One in 1000 per tick for an adult, one in 50 for a lamb, then 40 ticks of
/// head down; the grass goes on the fourth tick from the end. The adult rate is
/// measured (a hazard over 50 penned sheep); the lamb rate and the timing
/// inside the 40 ticks are the wiki's, not measured.
class EatGrassGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override {
        return GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "eat_grass"; }

    static constexpr i32 kAdultOdds = 1000;
    static constexpr i32 kLambOdds  = 50;
    static constexpr i32 kDuration  = 40;

    /// Ticks left of eating; 0 when not eating. For the server's animation.
    [[nodiscard]] i32 remaining() const noexcept { return remaining_; }

private:
    i32 remaining_{0};
};

/// The brain of another mob, for a goal that must know whether its mate is in
/// love. Null for an entity with no `Mob` behaviour.
[[nodiscard]] MobBrain* mob_brain_of(entity::EntityWorld& world,
                                     entity::EntityHandle handle) noexcept;

}  // namespace ov::gameplay
