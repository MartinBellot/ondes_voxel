// What hurts, how much, and how often it is allowed to.
//
// Damage in Minecraft is not one number. It is a *source* — a named damage type
// carrying its own exhaustion cost, its own death message and its own set of
// rules about what it ignores — applied through a window during which the
// target is briefly untouchable. Both halves matter: an implementation with the
// right formulas and no invulnerability window kills a player standing in a
// cactus twenty times a second.
//
// The forty-four damage types below are the ones the 1.20.1 datapack declares,
// with the exhaustion, message id and tag membership the data generator reports
// for each. They are here by **name** rather than by number on purpose: the
// damage type registry is one of the six the server *sends* to the client
// during login, so its ids belong to whatever codec we ship rather than to
// Mojang. The server resolves a name to an id once, at start-up.
//
// Everything numeric here was measured against a real 1.20.1 server by
// scripts/measure_survival.py — the fall curve over thirty heights, the length
// of the invulnerability window, and the rule that lets a bigger hit through a
// window a smaller one opened. See docs/provenance/survie.md.
//
// This is layer 9 and it stays there: nothing below takes a level, a server or
// an entity world. A caller passes state in and gets a decision back, which is
// what makes the same code usable for client-side prediction.
#pragma once

#include "ov/base/types.hpp"

#include <optional>
#include <string_view>

namespace ov::gameplay {

/// Every damage type the 1.20.1 datapack declares, in the order the registry
/// lists them — which is alphabetical, because a datapack registry is sorted by
/// resource location. That order is also the id order in the codec we send, and
/// it was confirmed on the wire: `damage @p 3 minecraft:cactus` arrived as
/// Damage Event with type 2, and minecraft:lightning_bolt as 23, which are the
/// third and twenty-fourth names in this list.
enum class DamageKind : u8 {
    Arrow,
    BadRespawnPoint,
    Cactus,
    Cramming,
    DragonBreath,
    Drown,
    DryOut,
    Explosion,
    Fall,
    FallingAnvil,
    FallingBlock,
    FallingStalactite,
    Fireball,
    Fireworks,
    FlyIntoWall,
    Freeze,
    Generic,
    GenericKill,
    HotFloor,
    InFire,
    InWall,
    IndirectMagic,
    Lava,
    LightningBolt,
    Magic,
    MobAttack,
    MobAttackNoAggro,
    MobProjectile,
    OnFire,
    OutOfWorld,
    OutsideBorder,
    PlayerAttack,
    PlayerExplosion,
    SonicBoom,
    Stalagmite,
    Starve,
    Sting,
    SweetBerryBush,
    Thorns,
    Thrown,
    Trident,
    UnattributedFireball,
    Wither,
    WitherSkull,
};

inline constexpr usize kDamageKindCount = 44;

/// The tags a damage type belongs to, as one word.
///
/// Tag membership is the whole of the game's damage rules: armour is skipped
/// because the type is in #bypasses_armor, a fall is a fall because it is in
/// #is_fall. Encoding them as flags rather than as a set of lookups keeps the
/// hot path free of allocation, which the tick requires.
enum class DamageFlags : u16 {
    None                    = 0,
    BypassesArmor           = 1U << 0U,
    BypassesInvulnerability = 1U << 1U,
    BypassesEffects         = 1U << 2U,
    BypassesResistance      = 1U << 3U,
    BypassesEnchantments    = 1U << 4U,
    IsFall                  = 1U << 5U,
    IsFire                  = 1U << 6U,
    IsDrowning              = 1U << 7U,
    IsExplosion             = 1U << 8U,
    IsProjectile            = 1U << 9U,
    IsFreezing              = 1U << 10U,
    IsLightning             = 1U << 11U,
};

[[nodiscard]] constexpr DamageFlags operator|(DamageFlags a, DamageFlags b) noexcept {
    return static_cast<DamageFlags>(static_cast<u16>(a) | static_cast<u16>(b));
}

[[nodiscard]] constexpr bool has(DamageFlags set, DamageFlags flag) noexcept {
    return (static_cast<u16>(set) & static_cast<u16>(flag)) != 0U;
}

/// How the game scales a damage type with difficulty.
///
/// Straight from the datapack's own `scaling` field. Only `Always` and
/// `WhenCausedByLivingNonPlayer` occur in 1.20.1; the third value exists in the
/// format and is named here so that a datapack using it is refused rather than
/// silently treated as one of the other two.
enum class DamageScaling : u8 { Never, WhenCausedByLivingNonPlayer, Always };

/// Which family of death messages a type draws from — the datapack's own
/// `death_message_type` field.
///
/// Three values occur in 1.20.1 and they are genuinely different sentences, not
/// a formatting detail. `FallVariants` picks between "fell from a high place",
/// "was doomed to fall" and four more depending on who is to blame;
/// `IntentionalGameDesign` is the bed explosion, whose message is a link. A
/// type is given the family the data says it has, never a default.
enum class DeathMessageType : u8 { Default, FallVariants, IntentionalGameDesign };

struct DamageTypeInfo {
    /// The registry name, e.g. "minecraft:fall".
    std::string_view name;

    /// The suffix of the death message's translation key. The client builds
    /// "death.attack." + this, so it is not decoration: it is what decides
    /// which sentence a player reads when they die.
    std::string_view message_id;

    /// Charged to the victim's hunger when the hit lands. Zero for most, a
    /// tenth for anything that is somebody's fault.
    f32 exhaustion;

    DamageScaling scaling;
    DamageFlags   flags;

    /// Which family of death messages this type draws from.
    DeathMessageType death_message;
};

/// The table, by kind. Total, and refusing an out-of-range kind rather than
/// returning a plausible default.
[[nodiscard]] const DamageTypeInfo& damage_type(DamageKind kind) noexcept;

/// Look a damage type up by registry name. Nullopt for anything not in 1.20.1.
[[nodiscard]] std::optional<DamageKind> damage_kind_from_name(std::string_view name) noexcept;

/// The translation key the client is sent, e.g. "death.attack.fall".
///
/// Written into a caller-provided buffer so nothing allocates on the tick.
/// Returns the length written, or 0 if the buffer is too small.
[[nodiscard]] usize death_message_key(DamageKind kind, char* out, usize capacity) noexcept;

// ── The state a living thing carries ────────────────────────────────────────

/// Everything a hit reads and writes.
///
/// Deliberately not an entity: a player, a mob and a client-side prediction all
/// have one of these, and none of them is visible from this layer.
struct HealthState {
    f32 health{20.0F};
    f32 max_health{20.0F};

    /// Counts down. While it is at or above `kPartialDamageThreshold` a new hit
    /// is measured against `last_damage` instead of landing whole.
    i32 invulnerable_ticks{0};

    /// The size of the hit that opened the current window. A bigger one still
    /// gets through — the difference, not the whole of it.
    f32 last_damage{0.0F};

    /// The flinch: how long the client draws the red overlay. Cosmetic here,
    /// but it is what the Damage Event packet triggers, so it lives with the
    /// rest of the state rather than in the network layer.
    i32 hurt_ticks{0};

    /// Blocks fallen since the last time this thing was on the ground.
    f32 fall_distance{0.0F};

    /// Ticks of breath left. Counts down under water and back up in air.
    i32 air{kMaxAir};

    bool dead{false};

    static constexpr i32 kMaxAir = 300;
};

/// What one call to `apply_damage` did.
struct DamageResult {
    /// Whether any health came off at all.
    bool applied{false};

    /// How much did. Less than the amount asked for when a window was already
    /// open and this hit only exceeded it.
    f32 dealt{0.0F};

    /// The window swallowed it whole.
    bool absorbed{false};

    /// This hit is what took the last point.
    bool killed{false};
};

/// The numbers the damage rules are made of. Measured, and named so a wrong one
/// shows up in a diff rather than hiding inside an expression.
struct DamageConstants {
    /// Set on `invulnerable_ticks` when a hit lands.
    ///
    /// Twenty, and the second half of it behaves differently from the first —
    /// see `partial_damage_threshold`.
    i32 invulnerable_ticks{20};

    /// While `invulnerable_ticks` is at least this, a new hit is compared
    /// against the last one instead of landing in full.
    ///
    /// Ten. Measured, not assumed: two four-point hits were fired at gaps of
    /// zero to fourteen ticks with a datapack function holding the timing
    /// exactly, and the second one started landing at a gap of eleven. Ten and
    /// twenty together are the only pair that reproduces that boundary.
    i32 partial_damage_threshold{10};

    /// How long the client shows the hurt flash.
    i32 hurt_ticks{10};

    /// Blocks of falling that cost nothing.
    f32 fall_damage_free_distance{3.0F};

    /// Ticks of breath, and what running out costs, and how often.
    i32 max_air{HealthState::kMaxAir};
    f32 drown_damage{2.0F};
    i32 drown_interval_ticks{20};
};

/// Damage from a fall of `distance` blocks.
///
/// Measured over thirty successive heights against a real server. The result is
/// `ceil(distance - 3)` clamped at zero — a *ceiling*, which matters: a fall of
/// 3.5 blocks costs one point, and rounding it the other way costs none. Both
/// look right in a video and only one is the game.
[[nodiscard]] f32 fall_damage(f32 distance, const DamageConstants& constants) noexcept;

/// Apply one hit.
///
/// The invulnerability rule, in the order the game applies it:
///
///   1. A type in #bypasses_invulnerability ignores everything below and lands.
///   2. Inside an open window, a hit no larger than the one that opened it does
///      nothing at all.
///   3. Inside an open window, a *larger* hit lands the difference, and becomes
///      the new benchmark. Four then nine costs nine, not thirteen; nine then
///      four costs nine, not thirteen either. Both were measured.
///   4. Outside a window, the hit lands whole and opens one.
[[nodiscard]] DamageResult apply_damage(HealthState& state, DamageKind kind, f32 amount,
                                        const DamageConstants& constants) noexcept;

/// One tick of the counters: invulnerability, the hurt flash.
///
/// Must run **before** any damage for that tick. The gap between two hits is
/// counted in these calls, so applying damage first would make a hit on the
/// same tick look one tick older than it is, and the measured boundary would
/// move by one.
void tick_health(HealthState& state, const DamageConstants& constants) noexcept;

/// One tick of breath. `submerged` is whether this thing's eyes are in water.
///
/// Returns the drowning damage owed this tick, which is zero on all but one
/// tick in twenty once the air is gone.
[[nodiscard]] f32 tick_air(HealthState& state, bool submerged,
                           const DamageConstants& constants) noexcept;

/// Accumulate a fall. `on_ground` ends the fall and returns what it cost.
///
/// Fall distance accumulates from the *vertical movement actually made*, not
/// from the difference between two positions: a fall interrupted by a ledge and
/// resumed is two falls, and measuring it end to end would charge for one long
/// one.
[[nodiscard]] f32 accumulate_fall(HealthState& state, f64 delta_y, bool on_ground,
                                  const DamageConstants& constants) noexcept;

}  // namespace ov::gameplay
