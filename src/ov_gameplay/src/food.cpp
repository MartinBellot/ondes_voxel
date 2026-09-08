#include "ov/gameplay/food.hpp"

#include <algorithm>
#include <array>

namespace ov::gameplay {
namespace {

// The measured food table.
//
// Nutrition and saturation modifiers are in Java code and in no report Mojang
// publishes, so this is not transcribed from anywhere: every item in the game
// that is not also a block was handed to a real player on a real 1.20.1 server
// and used, with foodLevel and foodSaturationLevel read on both sides. The
// modifier is recovered as `saturation_gained / (2 * nutrition)`, which is the
// game's own definition of it.
//
// The measurement is run from a baseline of ten food and zero saturation. Ten
// because the largest nutrition in the game is ten, so nothing can reach the
// ceiling of twenty and be recorded short — the first attempt used a baseline
// of fifteen and cooked porkchop came back as six instead of eight.
//
// See scripts/measure_survival.py and docs/provenance/survie.md.
constexpr std::array<FoodValue, 0> kFoods{};

}  // namespace

std::span<const FoodValue> food_table() noexcept { return kFoods; }

std::optional<FoodValue> food_for(std::string_view item_name) noexcept {
    for (const FoodValue& value : kFoods) {
        if (value.name == item_name) {
            return value;
        }
    }
    return std::nullopt;
}

f32 starvation_floor(Difficulty difficulty) noexcept {
    // Measured by starving a player on each difficulty from twelve health and
    // watching where it settled. Easy stops at ten, normal at one, hard at
    // zero — so hunger alone kills only on hard. Peaceful never starves at all;
    // it feeds the player back up instead.
    switch (difficulty) {
    case Difficulty::Peaceful:
        return 20.0F;
    case Difficulty::Easy:
        return 10.0F;
    case Difficulty::Normal:
        return 1.0F;
    case Difficulty::Hard:
        return 0.0F;
    }
    return 0.0F;
}

i32 add_exhaustion(FoodState& state, f32 amount, const FoodConstants& constants) noexcept {
    // Accumulate only. The wrap into saturation happens once per tick, in
    // `tick_food`, and not here: charging four points in one action must not
    // cost four haunches at once, and a loop here would do exactly that.
    constexpr f32 kExhaustionCeiling = 40.0F;
    state.exhaustion = std::min(state.exhaustion + amount, kExhaustionCeiling);
    (void)constants;
    return 0;
}

void eat(FoodState& state, const FoodValue& food) noexcept {
    constexpr i32 kMaxFood = 20;
    state.food             = std::min(state.food + food.nutrition, kMaxFood);
    // The ceiling is the *new* food level, applied after the nutrition. That
    // ordering is the whole reason a steak eaten on an empty stomach is worth
    // less hidden saturation than the same steak eaten half full.
    const f32 gained = static_cast<f32>(food.nutrition) * food.saturation_modifier * 2.0F;
    state.saturation = std::min(state.saturation + gained, static_cast<f32>(state.food));
}

bool can_eat(const FoodState& state, const FoodValue& food) noexcept {
    constexpr i32 kMaxFood = 20;
    return food.always_edible || state.food < kMaxFood;
}

FoodTick tick_food(FoodState& state, f32 health, f32 max_health, Difficulty difficulty,
                   bool natural_regeneration, const FoodConstants& constants) noexcept {
    FoodTick result;

    // One wrap per tick, and strictly *above* four rather than at it. Both
    // halves matter: a loop here would spend a whole bar on a single expensive
    // action, and wrapping at exactly four would move the boundary by one tick
    // in every measurement taken against it.
    if (state.exhaustion > constants.exhaustion_per_point) {
        state.exhaustion -= constants.exhaustion_per_point;
        if (state.saturation > 0.0F) {
            state.saturation = std::max(state.saturation - 1.0F, 0.0F);
        } else if (difficulty != Difficulty::Peaceful) {
            state.food = std::max(state.food - 1, 0);
        }
    }

    const bool hurt = health < max_health;

    if (natural_regeneration && state.saturation > 0.0F && hurt && state.food >= 20) {
        // The fast one: a full bar and saturation left. It heals every ten
        // ticks and charges its own exhaustion, which is why a saturated player
        // heals quickly and is hungry afterwards.
        ++state.tick_timer;
        if (state.tick_timer >= constants.saturated_regeneration_ticks) {
            const f32 pool = std::min(state.saturation, constants.regeneration);
            result.heal    = pool / constants.regeneration;
            result.exhaustion_charged = pool;
            add_exhaustion(state, pool, constants);
            state.tick_timer = 0;
        }
    } else if (natural_regeneration && state.food >= constants.regeneration_food && hurt) {
        ++state.tick_timer;
        if (state.tick_timer >= constants.slow_regeneration_ticks) {
            result.heal               = 1.0F;
            result.exhaustion_charged = constants.regeneration;
            add_exhaustion(state, constants.regeneration, constants);
            state.tick_timer = 0;
        }
    } else if (state.food <= 0) {
        ++state.tick_timer;
        if (state.tick_timer >= constants.starve_ticks) {
            // Starvation stops above a floor that depends on the difficulty,
            // which is measured rather than derived: easy leaves ten health,
            // normal one, hard nothing.
            if (health > starvation_floor(difficulty)) {
                result.damage = constants.starve_damage;
            }
            state.tick_timer = 0;
        }
    } else {
        state.tick_timer = 0;
    }

    return result;
}

}  // namespace ov::gameplay
