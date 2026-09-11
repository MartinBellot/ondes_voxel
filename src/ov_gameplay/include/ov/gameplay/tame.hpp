// ── tame ── Taming, owners, riding and the stats a foal inherits: the rules.
//
// Rules only, on the state a tame animal carries (tame_state.hpp) and on the
// entity world. Everything that needs a player's inventory, a UUID on the wire,
// a packet or a file is the server's (ov_server/src/taming.{hpp,cpp}); this
// layer decides and the caller carries out, as for husbandry (breeding.hpp).
//
// ── Provenance ──────────────────────────────────────────────────────────────
//
// The mechanisms are the documented ones (minecraft.wiki: *Wolf*, *Cat*,
// *Ocelot*, *Parrot*, *Horse*, *Donkey*, *Llama*, *Tutorials/Horses*); every
// probability, range and timer below was then measured against a real 1.20.1
// server by scripts/measure_tame.py, and docs/provenance/apprivoisement.md
// carries the numbers. What was not measured is said where it is used.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/animal.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/gameplay/tame_state.hpp"
#include "ov/math/random.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

struct MobKind;

// ── Constants ───────────────────────────────────────────────────────────────

/// Entity Event statuses a taming try ends in: hearts, or smoke. Captured on
/// every try of the `tame` campaign.
inline constexpr i8 kTamedEventStatus  = 7;
inline constexpr i8 kRefusedEventStatus = 6;
/// An ocelot's own pair: it comes to trust (41) or stays wary (40).
inline constexpr i8 kTrustedEventStatus   = 41;
inline constexpr i8 kDistrustEventStatus  = 40;

/// Follow the owner beyond this, stop inside `kFollowStop`, and be put back
/// beside them from `kTeleportDistance` (the wiki's 10, 2 and 12 blocks).
inline constexpr f64 kFollowStart       = 10.0;
inline constexpr f64 kFollowStop        = 2.0;
inline constexpr f64 kTeleportDistance  = 12.0;

/// What a throw adds to a horse's temper (the wiki's 5).
inline constexpr i32 kTemperStep = 5;

/// A wolf's maximum health, wild and tame. Measured with `attribute`.
inline constexpr f32 kWildWolfHealth = 8.0F;
inline constexpr f32 kTameWolfHealth = 20.0F;

// ── Species ─────────────────────────────────────────────────────────────────

struct TameKind {
    std::string_view type_name;
    TameFamily       family{TameFamily::None};
    /// What tames it, and the odds of one try: tamed on `next_int(odds) == 0`.
    std::span<const std::string_view> taming_food{};
    i32                               taming_odds{0};
    /// What breeds it, heals it and grows its young. For a wolf, any meat.
    std::span<const std::string_view> food{};
    /// The temper at which a ridden animal always gives in: a draw under the
    /// temper tames it. 0: not tamed by riding.
    i32 max_temper{0};
    /// Sits when told, follows its owner, wears a collar.
    bool sits{false};
    bool follows{false};
    bool collar{false};
    /// Follows a player holding `taming_food` or `food`, at this modifier.
    f64 tempt_speed{0.0};
    /// Runs the goal list of `install_tame_goals`. False: the ordinary
    /// animal's list (a rabbit, a fox, a goat), and the state here is only a
    /// variant or a flag.
    bool own_goals{true};
};

/// The entry for a type, or null: not every mob can be tamed.
[[nodiscard]] const TameKind* tame_kind(std::string_view type_name) noexcept;

/// The MobKinds of the species this file brings to life that the species
/// table (mob_species.cpp) does not carry: donkey, mule, llama, trader llama,
/// ocelot, parrot, turtle, bee, goat, camel, sniffer. Consulted by `mob_kind`.
[[nodiscard]] const MobKind* tame_mob_kind(std::string_view type_name) noexcept;

[[nodiscard]] bool is_taming_food(const TameKind& kind, std::string_view item) noexcept;
[[nodiscard]] bool is_tame_food(const TameKind& kind, std::string_view item) noexcept;

/// The health one item gives a tame wolf: the food's hunger points (the
/// wiki's rule, measured on the `wolf` campaign). Nullopt: not a wolf's food.
[[nodiscard]] std::optional<i32> wolf_food_heal(std::string_view item) noexcept;

/// What one item does to a horse, donkey, mule or llama (the wiki's feeding
/// tables — not measured, named in apprivoisement.md).
struct HorseFood {
    f32  heal{0.0F};
    /// Ticks a foal grows by.
    i32  growth{0};
    i32  temper{0};
    /// Starts love in a tame adult.
    bool love{false};
};
[[nodiscard]] std::optional<HorseFood> horse_food(std::string_view type_name,
                                                  std::string_view item) noexcept;

/// Once a tick, before the goals: anger runs down, a rider's time counts up.
void tick_tame(TameState& tame, entity::EntityState& state) noexcept;

// ── Draws ───────────────────────────────────────────────────────────────────

/// One taming try: tamed on `next_int(odds) == 0`.
[[nodiscard]] bool taming_roll(math::LegacyRandomSource& random, i32 odds) noexcept;

/// A ridden animal's decision: tamed when `next_int(max_temper) < temper`.
[[nodiscard]] bool temper_tames(math::LegacyRandomSource& random, i32 temper,
                                i32 max_temper) noexcept;

/// The per-tick chance a ridden untamed animal decides (tame or throw):
/// `next_int(kTantrumOdds) == 0`. See `RideTantrumGoal`.
inline constexpr i32 kTantrumOdds = 50;

/// `AngerTime` a hit gives a neutral wolf: 20 to 39 seconds.
[[nodiscard]] i32 draw_anger_time(math::LegacyRandomSource& random) noexcept;

/// Where the horse family's stats live, per type: the documented natural
/// ranges and whether each stat is drawn or fixed.
struct HorseRanges {
    f64  health_min{15.0}, health_max{30.0};
    f64  speed_min{0.1125}, speed_max{0.3375};
    f64  jump_min{0.4}, jump_max{1.0};
    bool speed_drawn{true};
    bool jump_drawn{true};
    f64  fixed_speed{0.175};
    f64  fixed_jump{0.5};
};
[[nodiscard]] HorseRanges horse_ranges(std::string_view type_name) noexcept;

/// A freshly spawned horse's stats (the wiki's *Horse*): health
/// `15 + next_int(8) + next_int(9)`, speed `(0.45 + 0.3·(r+r+r)) × 0.25`, jump
/// `0.4 + 0.2·(r+r+r)`. Donkeys, mules and llamas draw only their health.
[[nodiscard]] HorseStats draw_horse_stats(std::string_view type_name,
                                          math::LegacyRandomSource& random) noexcept;

/// One stat of a foal, from its parents' (1.20's rule, the wiki's *Horse §
/// Breeding*): the parents' mean plus a spread of `|a − b| + 0.3·(max − min)`
/// times `(r₁ + r₂ + r₃)/3 − ½`, folded back into the natural range at its
/// ends.
[[nodiscard]] f64 offspring_stat(f64 a, f64 b, f64 min, f64 max,
                                 math::LegacyRandomSource& random) noexcept;
[[nodiscard]] HorseStats offspring_stats(std::string_view child_type, const HorseStats& mother,
                                         const HorseStats& father,
                                         math::LegacyRandomSource& random) noexcept;

/// A llama's `Strength`: 1..3, or 1..5 with this chance — fitted on 160
/// measured llamas (11 above 3); the wiki's 1 in 25 gives 2.6.
inline constexpr f32 kLlamaWideStrength = 0.17F;
[[nodiscard]] i32 draw_llama_strength(math::LegacyRandomSource& random) noexcept;

/// A fresh animal's state for its family: the stats, the variant, a goat's
/// scream. Called by `Mob`'s constructor.
void init_tame(TameState& tame, const TameKind& kind, math::LegacyRandomSource& random) noexcept;

/// Put the drawn stats (and a tame wolf's health) on the body: maximum health
/// and a health no higher than it. Idempotent.
void apply_tame_body(TameState& tame, entity::EntityState& state) noexcept;

/// A horse's own speed against its species' attribute, as a factor on the walk
/// law's output (quadratic in the attribute). 1 when not drawn.
[[nodiscard]] f64 tame_speed_factor(const TameState& tame, f64 species_attribute) noexcept;

/// Is this animal under a rider's control: tame, saddled, ridden, and of a
/// family a rider can steer. Its brain then stands aside.
[[nodiscard]] bool rider_controls(const TameState& tame) noexcept;

// ── Goals ───────────────────────────────────────────────────────────────────

/// The goal list of a tame-able species.
void install_tame_goals(GoalSelector& selector, const MobKind& kind, const TameKind& tame,
                        i32 look_type, i32 quarry_type);

/// A creeper keeps away from cats and ocelots (the wiki's *Creeper*): 6
/// blocks, at 1.0 walking and 1.2 when close.
void install_creeper_fear(GoalSelector& selector, const MobKind& kind);

/// Sit, and stay sat, while ordered to.
class SitWhenOrderedGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move | GoalFlag::Jump; }
    [[nodiscard]] std::string_view name() const noexcept override { return "sit"; }
};

/// Walk to the owner when they are more than 10 blocks away, stop at 2, and
/// be put back beside them from 12.
class FollowOwnerGoal final : public Goal {
public:
    explicit FollowOwnerGoal(f64 speed) noexcept : speed_{speed} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move | GoalFlag::Look; }
    [[nodiscard]] std::string_view name() const noexcept override { return "follow_owner"; }

private:
    f64   speed_{1.0};
    Vec3d owner_{};
};

/// Defend the owner (`mode` HurtBy: what hurt them) or join their fight
/// (`mode` Attacked: what they hit). Holds only Target.
class OwnerTargetGoal final : public Goal {
public:
    enum class Mode : u8 { HurtBy, Attacked };
    explicit OwnerTargetGoal(Mode mode) noexcept : mode_{mode} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Target; }
    [[nodiscard]] std::string_view name() const noexcept override {
        return mode_ == Mode::HurtBy ? "owner_hurt_by" : "owner_hurt";
    }

private:
    Mode                 mode_;
    i64                  seen_{-1};
    entity::EntityHandle found_{entity::kNoEntity};
};

/// While angry (`AngerTime` > 0), hunt the player it is angry at.
class AngerTargetGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Target; }
    [[nodiscard]] std::string_view name() const noexcept override { return "anger"; }

private:
    i32 player_{0};
};

/// An untamed horse or llama with a rider: it runs about, then decides — tamed
/// if a draw falls under its temper, otherwise the rider is thrown and the
/// temper rises by 5.
class RideTantrumGoal final : public Goal {
public:
    RideTantrumGoal(f64 speed, i32 max_temper) noexcept : speed_{speed}, max_temper_{max_temper} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "tantrum"; }

private:
    f64      speed_{1.2};
    i32      max_temper_{100};
    BlockPos wanted_{};
};

/// Keep away from the nearest entity of two types.
class AvoidEntityGoal final : public Goal {
public:
    AvoidEntityGoal(f64 radius, f64 walk, f64 sprint) noexcept
        : radius_{radius}, walk_{walk}, sprint_{sprint} {}

    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    void                   tick(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] std::string_view name() const noexcept override { return "avoid"; }

private:
    f64                  radius_{6.0};
    f64                  walk_{1.0};
    f64                  sprint_{1.2};
    entity::EntityHandle feared_{entity::kNoEntity};
    BlockPos             away_{};
};

}  // namespace ov::gameplay
