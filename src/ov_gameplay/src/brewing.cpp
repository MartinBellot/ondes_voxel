#include "ov/gameplay/brewing.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

// ── The forty-three potions ─────────────────────────────────────────────────
//
// Effects and durations from the wiki's « Potion » article, held against a bot
// drinking each one on a real server (scripts/measure_brewing.py drink), which
// reads every effect off its Entity Effect packet — amplifier and duration to
// the tick. The result is docs/provenance/alchimie.md § 5, replayed by the
// `[parity]` test. Instant effects carry a duration of 1 nothing reads.

using E = Effect;

constexpr std::array<PotionEffect, 1> kNightVision{{{E::NightVision, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongNightVision{{{E::NightVision, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kInvisibility{{{E::Invisibility, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongInvisibility{{{E::Invisibility, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kLeaping{{{E::JumpBoost, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongLeaping{{{E::JumpBoost, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kStrongLeaping{{{E::JumpBoost, 1800, 1}}};
constexpr std::array<PotionEffect, 1> kFireResistance{{{E::FireResistance, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongFireResistance{{{E::FireResistance, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kSwiftness{{{E::Speed, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongSwiftness{{{E::Speed, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kStrongSwiftness{{{E::Speed, 1800, 1}}};
constexpr std::array<PotionEffect, 1> kSlowness{{{E::Slowness, 1800, 0}}};
constexpr std::array<PotionEffect, 1> kLongSlowness{{{E::Slowness, 4800, 0}}};
constexpr std::array<PotionEffect, 1> kStrongSlowness{{{E::Slowness, 400, 3}}};
constexpr std::array<PotionEffect, 2> kTurtleMaster{{{E::Slowness, 400, 3},
                                                     {E::Resistance, 400, 2}}};
constexpr std::array<PotionEffect, 2> kLongTurtleMaster{{{E::Slowness, 800, 3},
                                                         {E::Resistance, 800, 2}}};
constexpr std::array<PotionEffect, 2> kStrongTurtleMaster{{{E::Slowness, 400, 5},
                                                           {E::Resistance, 400, 3}}};
constexpr std::array<PotionEffect, 1> kWaterBreathing{{{E::WaterBreathing, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongWaterBreathing{{{E::WaterBreathing, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kHealing{{{E::InstantHealth, 1, 0}}};
constexpr std::array<PotionEffect, 1> kStrongHealing{{{E::InstantHealth, 1, 1}}};
constexpr std::array<PotionEffect, 1> kHarming{{{E::InstantDamage, 1, 0}}};
constexpr std::array<PotionEffect, 1> kStrongHarming{{{E::InstantDamage, 1, 1}}};
constexpr std::array<PotionEffect, 1> kPoison{{{E::Poison, 900, 0}}};
constexpr std::array<PotionEffect, 1> kLongPoison{{{E::Poison, 1800, 0}}};
constexpr std::array<PotionEffect, 1> kStrongPoison{{{E::Poison, 432, 1}}};
constexpr std::array<PotionEffect, 1> kRegeneration{{{E::Regeneration, 900, 0}}};
constexpr std::array<PotionEffect, 1> kLongRegeneration{{{E::Regeneration, 1800, 0}}};
constexpr std::array<PotionEffect, 1> kStrongRegeneration{{{E::Regeneration, 450, 1}}};
constexpr std::array<PotionEffect, 1> kStrength{{{E::Strength, 3600, 0}}};
constexpr std::array<PotionEffect, 1> kLongStrength{{{E::Strength, 9600, 0}}};
constexpr std::array<PotionEffect, 1> kStrongStrength{{{E::Strength, 1800, 1}}};
constexpr std::array<PotionEffect, 1> kWeakness{{{E::Weakness, 1800, 0}}};
constexpr std::array<PotionEffect, 1> kLongWeakness{{{E::Weakness, 4800, 0}}};
constexpr std::array<PotionEffect, 1> kLuck{{{E::Luck, 6000, 0}}};
constexpr std::array<PotionEffect, 1> kSlowFalling{{{E::SlowFalling, 1800, 0}}};
constexpr std::array<PotionEffect, 1> kLongSlowFalling{{{E::SlowFalling, 4800, 0}}};

constexpr std::array<PotionInfo, kPotionCount> kPotions{{
    {"minecraft:empty", {}},
    {"minecraft:water", {}},
    {"minecraft:mundane", {}},
    {"minecraft:thick", {}},
    {"minecraft:awkward", {}},
    {"minecraft:night_vision", kNightVision},
    {"minecraft:long_night_vision", kLongNightVision},
    {"minecraft:invisibility", kInvisibility},
    {"minecraft:long_invisibility", kLongInvisibility},
    {"minecraft:leaping", kLeaping},
    {"minecraft:long_leaping", kLongLeaping},
    {"minecraft:strong_leaping", kStrongLeaping},
    {"minecraft:fire_resistance", kFireResistance},
    {"minecraft:long_fire_resistance", kLongFireResistance},
    {"minecraft:swiftness", kSwiftness},
    {"minecraft:long_swiftness", kLongSwiftness},
    {"minecraft:strong_swiftness", kStrongSwiftness},
    {"minecraft:slowness", kSlowness},
    {"minecraft:long_slowness", kLongSlowness},
    {"minecraft:strong_slowness", kStrongSlowness},
    {"minecraft:turtle_master", kTurtleMaster},
    {"minecraft:long_turtle_master", kLongTurtleMaster},
    {"minecraft:strong_turtle_master", kStrongTurtleMaster},
    {"minecraft:water_breathing", kWaterBreathing},
    {"minecraft:long_water_breathing", kLongWaterBreathing},
    {"minecraft:healing", kHealing},
    {"minecraft:strong_healing", kStrongHealing},
    {"minecraft:harming", kHarming},
    {"minecraft:strong_harming", kStrongHarming},
    {"minecraft:poison", kPoison},
    {"minecraft:long_poison", kLongPoison},
    {"minecraft:strong_poison", kStrongPoison},
    {"minecraft:regeneration", kRegeneration},
    {"minecraft:long_regeneration", kLongRegeneration},
    {"minecraft:strong_regeneration", kStrongRegeneration},
    {"minecraft:strength", kStrength},
    {"minecraft:long_strength", kLongStrength},
    {"minecraft:strong_strength", kStrongStrength},
    {"minecraft:weakness", kWeakness},
    {"minecraft:long_weakness", kLongWeakness},
    {"minecraft:luck", kLuck},
    {"minecraft:slow_falling", kSlowFalling},
    {"minecraft:long_slow_falling", kLongSlowFalling},
}};

// ── The mixes ───────────────────────────────────────────────────────────────
//
// The wiki's « Brewing » table, one line per arrow. The `recipes` campaign of
// scripts/measure_brewing.py holds it against a real server exhaustively:
// every one of the 3 × 43 bottles against the 17 ingredients and 4 controls.

struct PotionMix {
    Potion           from;
    std::string_view ingredient;
    Potion           to;
};

using P = Potion;

constexpr std::string_view kWart      = "minecraft:nether_wart";
constexpr std::string_view kRedstone  = "minecraft:redstone";
constexpr std::string_view kGlowstone = "minecraft:glowstone_dust";
constexpr std::string_view kFermented = "minecraft:fermented_spider_eye";
constexpr std::string_view kGunpowder = "minecraft:gunpowder";
constexpr std::string_view kBreath    = "minecraft:dragon_breath";
constexpr std::string_view kSugar     = "minecraft:sugar";
constexpr std::string_view kRabbit    = "minecraft:rabbit_foot";
constexpr std::string_view kMelon     = "minecraft:glistering_melon_slice";
constexpr std::string_view kSpider    = "minecraft:spider_eye";
constexpr std::string_view kPuffer    = "minecraft:pufferfish";
constexpr std::string_view kMagma     = "minecraft:magma_cream";
constexpr std::string_view kCarrot    = "minecraft:golden_carrot";
constexpr std::string_view kBlaze     = "minecraft:blaze_powder";
constexpr std::string_view kTear      = "minecraft:ghast_tear";
constexpr std::string_view kTurtle    = "minecraft:turtle_helmet";
constexpr std::string_view kMembrane  = "minecraft:phantom_membrane";

constexpr std::array<std::string_view, 17> kIngredients{
    kWart,   kRedstone, kGlowstone, kFermented, kGunpowder, kBreath, kSugar,   kRabbit,  kMelon,
    kSpider, kPuffer,   kMagma,     kCarrot,    kBlaze,     kTear,   kTurtle, kMembrane,
};

constexpr auto kMixes = std::to_array<PotionMix>({
    // Water into the three base potions. Eight ingredients make mundane.
    {P::Water, kMelon, P::Mundane},
    {P::Water, kTear, P::Mundane},
    {P::Water, kRabbit, P::Mundane},
    {P::Water, kBlaze, P::Mundane},
    {P::Water, kSpider, P::Mundane},
    {P::Water, kSugar, P::Mundane},
    {P::Water, kMagma, P::Mundane},
    {P::Water, kGlowstone, P::Thick},
    {P::Water, kRedstone, P::Mundane},
    {P::Water, kWart, P::Awkward},
    // Night vision and invisibility.
    {P::Awkward, kCarrot, P::NightVision},
    {P::NightVision, kRedstone, P::LongNightVision},
    {P::NightVision, kFermented, P::Invisibility},
    {P::LongNightVision, kFermented, P::LongInvisibility},
    {P::Invisibility, kRedstone, P::LongInvisibility},
    // Fire resistance.
    {P::Awkward, kMagma, P::FireResistance},
    {P::FireResistance, kRedstone, P::LongFireResistance},
    // Leaping, and its corruption.
    {P::Awkward, kRabbit, P::Leaping},
    {P::Leaping, kRedstone, P::LongLeaping},
    {P::Leaping, kGlowstone, P::StrongLeaping},
    {P::Leaping, kFermented, P::Slowness},
    {P::LongLeaping, kFermented, P::LongSlowness},
    // Slowness.
    {P::Slowness, kRedstone, P::LongSlowness},
    {P::Slowness, kGlowstone, P::StrongSlowness},
    // The turtle master.
    {P::Awkward, kTurtle, P::TurtleMaster},
    {P::TurtleMaster, kRedstone, P::LongTurtleMaster},
    {P::TurtleMaster, kGlowstone, P::StrongTurtleMaster},
    // Swiftness, and its corruption.
    {P::Swiftness, kFermented, P::Slowness},
    {P::LongSwiftness, kFermented, P::LongSlowness},
    {P::Awkward, kSugar, P::Swiftness},
    {P::Swiftness, kRedstone, P::LongSwiftness},
    {P::Swiftness, kGlowstone, P::StrongSwiftness},
    // Water breathing.
    {P::Awkward, kPuffer, P::WaterBreathing},
    {P::WaterBreathing, kRedstone, P::LongWaterBreathing},
    // Healing and harming.
    {P::Awkward, kMelon, P::Healing},
    {P::Healing, kGlowstone, P::StrongHealing},
    {P::Healing, kFermented, P::Harming},
    {P::StrongHealing, kFermented, P::StrongHarming},
    {P::Harming, kGlowstone, P::StrongHarming},
    {P::Poison, kFermented, P::Harming},
    {P::LongPoison, kFermented, P::Harming},
    {P::StrongPoison, kFermented, P::StrongHarming},
    // Poison.
    {P::Awkward, kSpider, P::Poison},
    {P::Poison, kRedstone, P::LongPoison},
    {P::Poison, kGlowstone, P::StrongPoison},
    // Regeneration.
    {P::Awkward, kTear, P::Regeneration},
    {P::Regeneration, kRedstone, P::LongRegeneration},
    {P::Regeneration, kGlowstone, P::StrongRegeneration},
    // Strength.
    {P::Awkward, kBlaze, P::Strength},
    {P::Strength, kRedstone, P::LongStrength},
    {P::Strength, kGlowstone, P::StrongStrength},
    // Weakness, straight from water.
    {P::Water, kFermented, P::Weakness},
    {P::Weakness, kRedstone, P::LongWeakness},
    // Slow falling.
    {P::Awkward, kMembrane, P::SlowFalling},
    {P::SlowFalling, kRedstone, P::LongSlowFalling},
});

/// Java's `int << n`, as effects.cpp has it: amplifier 32 is amplifier 0.
[[nodiscard]] i32 java_shl(i32 value, u32 count) noexcept {
    return static_cast<i32>(static_cast<u32>(value) << (count & 31U));
}

[[nodiscard]] EffectInstance instance_of(const PotionEffect& effect, i32 duration) noexcept {
    EffectInstance instance;
    instance.effect    = effect.effect;
    instance.duration  = duration;
    instance.amplifier = effect.amplifier;
    return instance;
}

}  // namespace

std::optional<PotionForm> potion_form(std::string_view item) noexcept {
    if (item == "minecraft:potion") {
        return PotionForm::Drink;
    }
    if (item == "minecraft:splash_potion") {
        return PotionForm::Splash;
    }
    if (item == "minecraft:lingering_potion") {
        return PotionForm::Lingering;
    }
    return std::nullopt;
}

std::string_view potion_form_item(PotionForm form) noexcept {
    switch (form) {
        case PotionForm::Drink: return "minecraft:potion";
        case PotionForm::Splash: return "minecraft:splash_potion";
        case PotionForm::Lingering: return "minecraft:lingering_potion";
    }
    return "minecraft:potion";
}

const PotionInfo& potion_info(Potion potion) noexcept {
    const auto index = static_cast<usize>(potion);
    return kPotions[index < kPotions.size() ? index : 0];
}

std::optional<Potion> potion_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kPotions.size(); ++i) {
        if (kPotions[i].name == name) {
            return static_cast<Potion>(i);
        }
    }
    return std::nullopt;
}

u32 potion_color(std::span<const PotionEffect> effects) noexcept {
    // The swirl's own mixing, measured on 64 cases in effets.md § 10: each
    // channel `(float)(weight × c) / 255` summed, then `/ total × 255`, cut.
    if (effects.empty()) {
        return kWaterColor;
    }
    f32 red   = 0.0F;
    f32 green = 0.0F;
    f32 blue  = 0.0F;
    i32 total = 0;
    for (const PotionEffect& effect : effects) {
        const u32 color  = effect_info(effect.effect).color;
        const i32 weight = static_cast<i32>(effect.amplifier) + 1;
        red += static_cast<f32>(weight * static_cast<i32>((color >> 16U) & 0xFFU)) / 255.0F;
        green += static_cast<f32>(weight * static_cast<i32>((color >> 8U) & 0xFFU)) / 255.0F;
        blue += static_cast<f32>(weight * static_cast<i32>(color & 0xFFU)) / 255.0F;
        total += weight;
    }
    const auto channel = [total](f32 sum) {
        return static_cast<u32>(static_cast<i32>(sum / static_cast<f32>(total) * 255.0F));
    };
    return (channel(red) << 16U) | (channel(green) << 8U) | channel(blue);
}

u32 potion_color(Potion potion) noexcept { return potion_color(potion_info(potion).effects); }

bool is_brewing_ingredient(std::string_view item) noexcept {
    return !item.empty() && std::ranges::find(kIngredients, item) != kIngredients.end();
}

std::optional<Bottle> brew(const Bottle& bottle, std::string_view ingredient) noexcept {
    if (ingredient.empty()) {
        return std::nullopt;
    }
    // The container mixes first: they are item-to-item and keep the potion,
    // whatever it is — an uncraftable potion becomes an uncraftable splash.
    if (ingredient == kGunpowder && bottle.form == PotionForm::Drink) {
        return Bottle{PotionForm::Splash, bottle.potion};
    }
    if (ingredient == kBreath && bottle.form == PotionForm::Splash) {
        return Bottle{PotionForm::Lingering, bottle.potion};
    }
    for (const PotionMix& mix : kMixes) {
        if (mix.from == bottle.potion && mix.ingredient == ingredient) {
            return Bottle{bottle.form, mix.to};
        }
    }
    return std::nullopt;
}

std::string_view brewing_remainder(std::string_view ingredient) noexcept {
    return ingredient == kBreath ? std::string_view{"minecraft:glass_bottle"} : std::string_view{};
}

bool stand_can_brew(const StandView& view) noexcept {
    if (!is_brewing_ingredient(view.ingredient)) {
        return false;
    }
    return std::ranges::any_of(view.bottles, [&](const std::optional<Bottle>& bottle) {
        return bottle && brew(*bottle, view.ingredient).has_value();
    });
}

StandTick brewing_stand_tick(const StandView& view, StandState& state) noexcept {
    StandTick out;
    if (state.fuel <= 0 && view.powder) {
        state.fuel    = kFuelPerPowder;
        out.refuelled = true;
    }
    const bool brewable = stand_can_brew(view);
    if (state.brew_time > 0) {
        --state.brew_time;
        if (state.brew_time == 0 && brewable) {
            out.brewed = true;
            for (usize i = 0; i < view.bottles.size(); ++i) {
                if (view.bottles[i]) {
                    out.results[i] = brew(*view.bottles[i], view.ingredient);
                }
            }
            state.brewing = {};
        } else if (!brewable || view.ingredient != state.brewing) {
            state.brew_time = 0;
            state.brewing   = {};
            out.stopped     = true;
        }
    } else if (brewable && state.fuel > 0) {
        --state.fuel;
        state.brew_time = kBrewTicks;
        // The table's own copy of the name, not the caller's: the caller's
        // points into a block entity that the brew itself is about to rewrite.
        state.brewing = *std::ranges::find(kIngredients, view.ingredient);
        out.started     = true;
    }
    return out;
}

// ── Using a potion ──────────────────────────────────────────────────────────

void apply_instant(Effect effect, u8 amplifier, f64 factor, EffectTarget& target) {
    const bool undead = target.body() == EffectTarget::Body::Undead;
    const bool heals  = (effect == Effect::InstantHealth) != undead;
    if (heals) {
        const auto amount = static_cast<i32>(factor * static_cast<f64>(java_shl(4, amplifier)) + 0.5);
        target.heal(static_cast<f32>(std::max(amount, 0)));
    } else {
        const auto amount = static_cast<i32>(factor * static_cast<f64>(java_shl(6, amplifier)) + 0.5);
        target.hurt(DamageKind::Magic, static_cast<f32>(amount), DamageConstants{});
    }
}

void drink_effects(std::span<const PotionEffect> effects, ActiveEffects& active,
                   EffectTarget& target) {
    for (const PotionEffect& effect : effects) {
        if (effect_info(effect.effect).instantaneous) {
            apply_instant(effect.effect, effect.amplifier, 1.0, target);
            continue;
        }
        (void)active.add(instance_of(effect, effect.duration), target);
    }
}

f64 splash_factor(f64 distance_sq, bool direct) noexcept {
    if (direct) {
        return 1.0;
    }
    if (distance_sq >= kSplashRadius * kSplashRadius) {
        return 0.0;
    }
    return 1.0 - std::sqrt(distance_sq) / kSplashRadius;
}

i32 splash_duration(i32 duration, f64 factor) noexcept {
    return static_cast<i32>(factor * static_cast<f64>(duration) + 0.5);
}

void splash_effects(std::span<const PotionEffect> effects, f64 factor, ActiveEffects& active,
                    EffectTarget& target) {
    if (factor <= 0.0) {
        return;
    }
    for (const PotionEffect& effect : effects) {
        if (effect_info(effect.effect).instantaneous) {
            apply_instant(effect.effect, effect.amplifier, factor, target);
            continue;
        }
        const i32 duration = splash_duration(effect.duration, factor);
        if (duration > kSplashMinimumTicks) {
            (void)active.add(instance_of(effect, duration), target);
        }
    }
}

i32 arrow_duration(i32 duration) noexcept { return std::max(duration / 8, 1); }

void arrow_effects(std::span<const PotionEffect> effects, ActiveEffects& active,
                   EffectTarget& target) {
    for (const PotionEffect& effect : effects) {
        if (effect_info(effect.effect).instantaneous) {
            apply_instant(effect.effect, effect.amplifier, 1.0, target);
            continue;
        }
        (void)active.add(instance_of(effect, arrow_duration(effect.duration)), target);
    }
}

// ── The lingering cloud ─────────────────────────────────────────────────────

Cloud lingering_cloud() noexcept { return Cloud{}; }

i32 cloud_duration(i32 duration) noexcept { return duration / 4; }

bool cloud_tick(Cloud& cloud) noexcept {
    ++cloud.age;
    if (cloud.age >= cloud.wait_time + cloud.duration) {
        return false;
    }
    if (cloud_waiting(cloud)) {
        return true;
    }
    cloud.radius += cloud.radius_per_tick;
    return cloud.radius >= 0.5F;
}

bool cloud_waiting(const Cloud& cloud) noexcept { return cloud.age < cloud.wait_time; }

bool cloud_scans(const Cloud& cloud) noexcept {
    return !cloud_waiting(cloud) && cloud.age % kCloudScanInterval == 0;
}

bool cloud_reaches(const Cloud& cloud, f64 dx, f64 dy, f64 dz, f64 height) noexcept {
    const f64 r = static_cast<f64>(cloud.radius);
    return dy < 0.5 && dy + height > 0.0 && dx * dx + dz * dz <= r * r;
}

bool cloud_used(Cloud& cloud) noexcept {
    if (cloud.radius_on_use != 0.0F) {
        cloud.radius += cloud.radius_on_use;
        if (cloud.radius <= 0.5F) {
            return false;
        }
    }
    if (cloud.duration_on_use != 0) {
        cloud.duration += cloud.duration_on_use;
        if (cloud.duration <= 0) {
            return false;
        }
    }
    return true;
}

void cloud_effects(std::span<const PotionEffect> effects, ActiveEffects& active,
                   EffectTarget& target) {
    for (const PotionEffect& effect : effects) {
        if (effect_info(effect.effect).instantaneous) {
            apply_instant(effect.effect, effect.amplifier, kCloudInstantFactor, target);
            continue;
        }
        (void)active.add(instance_of(effect, cloud_duration(effect.duration)), target);
    }
}

}  // namespace ov::gameplay
