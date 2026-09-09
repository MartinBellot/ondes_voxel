// Hunger: three counters, one of which the player can never see.
//
// The visible haunches are only the top of it. Underneath sits **saturation**,
// a hidden pool that is spent before the visible bar ever moves, and under that
// **exhaustion**, an accumulator that every action charges a fraction to. When
// exhaustion reaches 4.0 it wraps: it drops by four and one point of saturation
// is spent, or, if there is none left, one haunch.
//
// That three-stage arrangement is why a well-fed player can sprint for a long
// time before the bar twitches and then loses it steadily afterwards, and an
// implementation with only the visible counter gets the feel of the whole game
// wrong while looking correct in a screenshot.
//
// Everything here is measured. Nutrition and saturation modifiers live in Java
// code and appear in no report Mojang publishes, so every food in the game was
// fed to a real player on a real 1.20.1 server and the two counters read back;
// the exhaustion costs were measured the same way, by sprinting a known
// distance and breaking a known number of blocks. See docs/provenance/survie.md
// and scripts/measure_survival.py.
#pragma once

#include "ov/base/types.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// The world's difficulty. Hunger is the one system where it changes the rules
/// rather than a multiplier: starvation stops at a different health on each.
enum class Difficulty : u8 { Peaceful, Easy, Normal, Hard };

/// A player's hunger, in full.
struct FoodState {
    /// The visible bar, 0..20. Twenty is two haunch rows of ten halves.
    i32 food{20};

    /// The hidden pool. Never above `food`, which is why eating on a full bar
    /// wastes most of a golden carrot.
    f32 saturation{5.0F};

    /// Charged by actions. Wraps at 4.0 into one point of saturation or food.
    f32 exhaustion{0.0F};

    /// Counts the ticks between regeneration or starvation events.
    i32 tick_timer{0};
};

/// What one food restores.
struct FoodValue {
    /// The registry name, e.g. "minecraft:bread".
    std::string_view name;

    /// Points added to the visible bar.
    i32 nutrition;

    /// The modifier, not the saturation itself. Eating adds
    /// `nutrition * modifier * 2`, capped at the new food level — which is why
    /// a steak eaten on an empty stomach gives less saturation than the same
    /// steak eaten half full.
    f32 saturation_modifier;

    /// Edible at a full bar. Golden apples and honey, and nothing else that a
    /// sweep of every non-block item in the game turned up.
    bool always_edible;
};

/// The measured table, in registry order.
[[nodiscard]] std::span<const FoodValue> food_table() noexcept;

/// Look up one item. Nullopt for anything that is not food, which is the honest
/// answer — a zero-nutrition entry would make a stone block edible.
[[nodiscard]] std::optional<FoodValue> food_for(std::string_view item_name) noexcept;

/// The costs, in exhaustion, of doing things.
///
/// Named individually so a wrong one appears in a diff. Each was measured by
/// doing the thing many times and reading the counters on both sides — the
/// totals are reconstructed through the wrap at 4.0, because the counter itself
/// never shows more than that.
struct FoodConstants {
    /// The wrap point. Four exactly.
    f32 exhaustion_per_point{4.0F};

    /// Per block, while sprinting. Walking costs nothing at all, which is not
    /// an omission: it was measured at zero over twenty-six blocks.
    f32 sprint_per_block{0.1F};
    f32 walk_per_block{0.0F};

    /// Per block, swimming or crawling.
    f32 swim_per_block{0.01F};

    /// One jump, and the more expensive sprinting one.
    f32 jump{0.05F};
    f32 sprint_jump{0.2F};

    /// Breaking one block.
    f32 break_block{0.005F};

    /// One swing of an attack that connects.
    f32 attack{0.1F};

    /// Charged when regeneration heals half a heart.
    f32 regeneration{6.0F};

    /// At or above this, health comes back.
    i32 regeneration_food{18};

    /// Ticks between the two kinds of regeneration.
    ///
    /// The fast one needs a full bar *and* saturation left; it heals every ten
    /// ticks. The slow one needs only eighteen and heals every eighty. Both
    /// charge the same exhaustion per heal, which is why the fast one empties
    /// the bar so quickly.
    i32 saturated_regeneration_ticks{10};
    i32 slow_regeneration_ticks{80};

    /// Ticks between starvation hits, and how hard each one is.
    i32 starve_ticks{80};
    f32 starve_damage{1.0F};

    /// How long eating takes. Thirty-two ticks for nearly everything; dried
    /// kelp is faster and is the reason this is per-item rather than a
    /// constant.
    i32 default_use_duration{32};

    /// The peaceful-difficulty rule: food comes back on its own.
    i32 peaceful_refill_ticks{10};
};

/// The health below which starvation stops, on a given difficulty.
///
/// Easy stops at ten, normal at one, hard at zero — so only on hard does hunger
/// alone kill. Measured by starving a player on each difficulty and watching
/// where the health settled.
[[nodiscard]] f32 starvation_floor(Difficulty difficulty) noexcept;

/// Charge exhaustion, wrapping into saturation and then into food.
///
/// Returns how many points were wrapped, which the caller needs in order to
/// know whether to tell the client anything.
i32 add_exhaustion(FoodState& state, f32 amount, const FoodConstants& constants) noexcept;

/// Eat something.
///
/// The saturation ceiling is the *new* food level, applied after the nutrition:
/// that ordering is what makes eating on an empty bar restore less hidden
/// saturation than the same item on a half-full one.
void eat(FoodState& state, const FoodValue& food) noexcept;

/// What one tick of hunger did.
struct FoodTick {
    /// Health to give back this tick. Half a point at a time.
    f32 heal{0.0F};

    /// Health to take away this tick.
    f32 damage{0.0F};

    /// Exhaustion charged by the regeneration itself.
    f32 exhaustion_charged{0.0F};
};

/// One tick of hunger, regeneration and starvation.
///
/// `health` and `max_health` are passed rather than owned: this module knows
/// nothing about what is being fed, which is what lets the same code run on the
/// server and inside a client prediction.
[[nodiscard]] FoodTick tick_food(FoodState& state, f32 health, f32 max_health,
                                 Difficulty difficulty, bool natural_regeneration,
                                 const FoodConstants& constants) noexcept;

/// Whether a player may start eating this.
[[nodiscard]] bool can_eat(const FoodState& state, const FoodValue& food) noexcept;

}  // namespace ov::gameplay
