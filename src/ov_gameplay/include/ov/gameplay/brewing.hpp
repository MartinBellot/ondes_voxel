// Brewing and potions: the forty-three potions, the mixes that turn one into
// another, the brewing stand's clock and fuel, and what a potion does when it
// is drunk, thrown, left lingering or put on an arrow.
//
// There is no `brewing` recipe type in 1.20.1 and nothing about potions is in
// the data generator's reports beyond the names of `minecraft:potion`. The
// recipe table below is written from the wiki's « Brewing » article — the
// documentary source, traced in docs/provenance/alchimie.md — and then held
// against the real server by scripts/measure_brewing.py, which puts **every**
// (container, potion, ingredient) triple in a brewing stand: 3 × 43 bottles ×
// 21 ingredients, 2709 bottles. The durations, the brew time, the fuel, the
// splash law and the cloud's constants are held against it the same way; each
// number below names its source and the campaign that checks it.
//
// Layer 9: nothing here takes a server or a world. A drink, a splash and a
// cloud reach their bearer through `EffectTarget` and `ActiveEffects`, which a
// server player, a mob and a client prediction can each implement.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/effects.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// The three bottles a potion can be in. Brewing gunpowder into a potion makes
/// it a splash potion; dragon's breath into a splash potion makes it lingering.
enum class PotionForm : u8 { Drink, Splash, Lingering };

/// The form of an item name ("minecraft:splash_potion"), or nothing.
[[nodiscard]] std::optional<PotionForm> potion_form(std::string_view item) noexcept;

/// The item name of a form.
[[nodiscard]] std::string_view potion_form_item(PotionForm form) noexcept;

/// The forty-three entries of `minecraft:potion`, valued at their registry id.
/// A test holds the order against the registry entry by entry.
enum class Potion : u8 {
    Empty,
    Water,
    Mundane,
    Thick,
    Awkward,
    NightVision,
    LongNightVision,
    Invisibility,
    LongInvisibility,
    Leaping,
    LongLeaping,
    StrongLeaping,
    FireResistance,
    LongFireResistance,
    Swiftness,
    LongSwiftness,
    StrongSwiftness,
    Slowness,
    LongSlowness,
    StrongSlowness,
    TurtleMaster,
    LongTurtleMaster,
    StrongTurtleMaster,
    WaterBreathing,
    LongWaterBreathing,
    Healing,
    StrongHealing,
    Harming,
    StrongHarming,
    Poison,
    LongPoison,
    StrongPoison,
    Regeneration,
    LongRegeneration,
    StrongRegeneration,
    Strength,
    LongStrength,
    StrongStrength,
    Weakness,
    LongWeakness,
    Luck,
    SlowFalling,
    LongSlowFalling,
};

inline constexpr usize kPotionCount = 43;

/// One effect a potion carries, as drunk. Instant effects carry a duration
/// of 1 that nothing reads.
struct PotionEffect {
    Effect effect;
    i32    duration;
    u8     amplifier;
};

struct PotionInfo {
    /// "minecraft:strong_swiftness".
    std::string_view name;
    std::span<const PotionEffect> effects;
};

[[nodiscard]] const PotionInfo& potion_info(Potion potion) noexcept;

/// The potion an item's `Potion` tag names. Nullopt for a name that is not one
/// of the forty-three: the caller decides what an unknown name means (vanilla
/// reads it as `minecraft:empty`, and says so nowhere).
[[nodiscard]] std::optional<Potion> potion_from_name(std::string_view name) noexcept;

/// The colour of a potion's liquid, 0xRRGGBB: the effects' colours weighted by
/// `amplifier + 1`, in the arithmetic effets.md § 10 measured for the swirl,
/// and water's blue for a potion with no effect.
[[nodiscard]] u32 potion_color(std::span<const PotionEffect> effects) noexcept;
[[nodiscard]] u32 potion_color(Potion potion) noexcept;

/// A potion with no effect is drawn this colour (water, awkward, mundane…).
inline constexpr u32 kWaterColor = 0x385DC6;

// ── The mixes ───────────────────────────────────────────────────────────────

/// A bottle in a brewing stand: which bottle, holding which potion.
struct Bottle {
    PotionForm form{PotionForm::Drink};
    Potion     potion{Potion::Water};

    friend bool operator==(const Bottle&, const Bottle&) noexcept = default;
};

/// Is this item accepted by the ingredient slot at all? The seventeen that
/// appear in some mix, and nothing else.
[[nodiscard]] bool is_brewing_ingredient(std::string_view item) noexcept;

/// What brewing `ingredient` into `bottle` makes, or nothing when there is no
/// mix. A container mix (gunpowder, dragon's breath) changes the bottle and
/// keeps the potion; a potion mix keeps the bottle and changes the potion.
[[nodiscard]] std::optional<Bottle> brew(const Bottle& bottle, std::string_view ingredient) noexcept;

/// What the ingredient leaves behind after a brew. Dragon's breath leaves its
/// glass bottle (wiki; checked by the `timing` campaign).
[[nodiscard]] std::string_view brewing_remainder(std::string_view ingredient) noexcept;

// ── The brewing stand ───────────────────────────────────────────────────────

/// Ticks from the start of a brew to the bottles changing. Wiki; checked by
/// the `timing` campaign.
inline constexpr i32 kBrewTicks = 400;

/// Brews one blaze powder pays for. Wiki; the `timing` campaign reads the
/// refuel and the fuel at the start of each brew.
inline constexpr i32 kFuelPerPowder = 20;

/// The stand's five slots as the rules read them. Built by the caller from the
/// block entity without copying anything the rules do not need.
struct StandView {
    /// Slots 0..2. Nullopt for an empty slot **and** for a bottle slot holding
    /// something no mix applies to (a glass bottle).
    std::array<std::optional<Bottle>, 3> bottles{};
    /// Whether slots 0..2 hold anything at all — the block's `has_bottle_N`.
    std::array<bool, 3> occupied{};
    /// Slot 3's item name, empty when the slot is.
    std::string_view ingredient;
    /// Slot 4 holds blaze powder.
    bool powder{false};
};

/// What the stand keeps between ticks. `brew_time` and `fuel` are its NBT
/// (`BrewTime`, `Fuel`); `brewing` is not saved.
struct StandState {
    i32 brew_time{0};
    i32 fuel{0};
    /// The ingredient the brew in progress started with. Taking it out, or
    /// swapping it for another, stops the brew.
    std::string_view brewing{};
};

/// What one tick did, for the caller to write back.
struct StandTick {
    /// A blaze powder was taken from slot 4 to refuel.
    bool refuelled{false};
    /// A brew started this tick (and the fuel went down by one).
    bool started{false};
    /// A brew in progress was abandoned.
    bool stopped{false};
    /// The bottles changed this tick; `results` holds what to write, and one
    /// ingredient is used.
    bool brewed{false};
    std::array<std::optional<Bottle>, 3> results{};
};

/// One tick of a brewing stand. The rules, in the order the `timing` campaign
/// checks:
///
///   * out of fuel with blaze powder in slot 4: refuel to 20, one powder used;
///   * brewing: count down; at zero, brew if the mix still holds; stop as soon
///     as it does not, or the ingredient changed;
///   * not brewing, something to brew and fuel left: spend one fuel, start at
///     400.
[[nodiscard]] StandTick brewing_stand_tick(const StandView& view, StandState& state) noexcept;

/// Would anything brew? The ingredient is one, and at least one bottle has a
/// mix with it.
[[nodiscard]] bool stand_can_brew(const StandView& view) noexcept;

// ── Using a potion ──────────────────────────────────────────────────────────

/// Apply an instant effect scaled by `factor` (a splash's distance, a cloud's
/// half). `4 << amp` healed or `6 << amp` dealt at factor 1, inverted on the
/// undead, exactly as effects.cpp does it at full strength.
void apply_instant(Effect effect, u8 amplifier, f64 factor, EffectTarget& target);

/// Drink: every effect at full strength, instant ones applied at once.
void drink_effects(std::span<const PotionEffect> effects, ActiveEffects& active,
                   EffectTarget& target);

/// A splash's strength at a distance: 1 for the entity hit directly, else
/// `1 − d/4`, and nothing at 4 or beyond. `distance_sq` is squared.
[[nodiscard]] f64 splash_factor(f64 distance_sq, bool direct) noexcept;

/// Radius within which a splash reaches anything. The box it searches is this
/// wide and half as tall (±2 vertically).
inline constexpr f64 kSplashRadius     = 4.0;
inline constexpr f64 kSplashHalfHeight = 2.0;

/// A non-instant effect's duration after a splash: `(int)(factor × d + 0.5)`.
[[nodiscard]] i32 splash_duration(i32 duration, f64 factor) noexcept;

/// A splashed effect shorter than this many ticks is not applied at all.
inline constexpr i32 kSplashMinimumTicks = 20;

/// Apply a splash's effects to one target at one factor.
void splash_effects(std::span<const PotionEffect> effects, f64 factor, ActiveEffects& active,
                    EffectTarget& target);

/// An arrow's effect lasts an eighth of the potion's, at least one tick.
[[nodiscard]] i32 arrow_duration(i32 duration) noexcept;

/// A spectral arrow makes its target glow this long.
inline constexpr i32 kSpectralGlowTicks = 200;

/// Apply a tipped arrow's effects to what it hit.
void arrow_effects(std::span<const PotionEffect> effects, ActiveEffects& active,
                   EffectTarget& target);

// ── The lingering cloud ─────────────────────────────────────────────────────

/// An `area_effect_cloud` as a lingering potion leaves it. The defaults are the
/// NBT the real server wrote for a cloud a lingering potion made (lingering).
struct Cloud {
    f32 radius{3.0F};
    f32 radius_on_use{-0.5F};
    /// `-radius / duration` at birth.
    f32 radius_per_tick{-0.005F};
    i32 duration{600};
    i32 duration_on_use{0};
    i32 wait_time{10};
    i32 reapplication_delay{20};
    i32 age{0};
};

/// A cloud as a lingering potion leaves it.
[[nodiscard]] Cloud lingering_cloud() noexcept;

/// A cloud's effect lasts a quarter of the potion's.
[[nodiscard]] i32 cloud_duration(i32 duration) noexcept;

/// An instant effect in a cloud acts at half strength.
inline constexpr f64 kCloudInstantFactor = 0.5;

/// A cloud looks for victims once every this many ticks.
inline constexpr i32 kCloudScanInterval = 5;

/// One tick of the cloud's own clock. False when it is gone.
[[nodiscard]] bool cloud_tick(Cloud& cloud) noexcept;

/// Still waiting to start (the first `wait_time` ticks)?
[[nodiscard]] bool cloud_waiting(const Cloud& cloud) noexcept;

/// Does it scan for victims this tick?
[[nodiscard]] bool cloud_scans(const Cloud& cloud) noexcept;

/// Does the cloud reach an entity whose feet are `(dx, dy, dz)` from the
/// cloud's own position and which is `height` tall? Its box must meet the
/// cloud's, which is half a block tall, and its feet be within the radius
/// horizontally.
[[nodiscard]] bool cloud_reaches(const Cloud& cloud, f64 dx, f64 dy, f64 dz, f64 height) noexcept;

/// The cloud was used on someone: it shrinks by `radius_on_use`. False when it
/// is gone as a result.
[[nodiscard]] bool cloud_used(Cloud& cloud) noexcept;

/// Apply a cloud's effects to one target.
void cloud_effects(std::span<const PotionEffect> effects, ActiveEffects& active,
                   EffectTarget& target);

// ── Suspicious stew ─────────────────────────────────────────────────────────

/// An `Effects` entry of a suspicious stew with no `EffectDuration` lasts this
/// long. Wiki; checked by the `stew` campaign.
inline constexpr i32 kStewDefaultTicks = 160;

}  // namespace ov::gameplay
