// Status effects: thirty-three of them, each a duration, an amplifier and a
// rule, and a way for a strong one to hide a weak one until it wears off.
//
// The registry gives the names and the (1-based) ids and nothing else. Every
// behaviour below was asked of a real 1.20.1 server by scripts/measure_effects.py
// and is written down in docs/provenance/effets.md with its campaign:
//
//   * **When the periodic effects act.** 522 cows were summoned with an effect
//     in their NBT at an exact duration D, in ticks, and left until it expired.
//     What they gained or lost over the whole duration does not depend on
//     console timing at all, and D -> count pins both the interval and its
//     boundary: regeneration heals `floor(D / k)` times with k = 50 >> amp,
//     poison k = 25 >> amp, wither k = 40 >> amp, and every tick once k is 0.
//     Poison and wither are then thinned by the invulnerability window, and
//     the gap they need is ten ticks, not the eleven a console hit needs — see
//     `kEffectDamage` in effects.cpp.
//   * **What the instant effects do.** 4 << amp healed and 6 << amp dealt, the
//     other way round on the undead.
//   * **The modifiers.** Nine effects carry one. Their UUIDs, names, amounts
//     and operations were read off `data get entity … Attributes`, not
//     recalled.
//   * **The replacement rules and the hidden effect.** Eleven pairs of `effect
//     give`, read back as NBT.
//   * **Who refuses what.** A zombie and a skeleton refuse regeneration and
//     poison; a spider refuses poison; a cow refuses nothing.
//
// Layer 9, like damage and hunger: nothing here takes a server, a level or an
// entity world. What an effect does to its bearer goes through `EffectTarget`,
// which a server player, a mob and a client prediction can each implement.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/attributes.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/math/random.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/types.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// The thirty-three effects, valued at their `minecraft:mob_effect` id.
///
/// **1-based**: speed is 1, darkness is 33. The value is the wire id and the
/// NBT `Id`, and a test holds it against the registry entry by entry.
enum class Effect : u8 {
    Speed = 1,
    Slowness,
    Haste,
    MiningFatigue,
    Strength,
    InstantHealth,
    InstantDamage,
    JumpBoost,
    Nausea,
    Regeneration,
    Resistance,
    FireResistance,
    WaterBreathing,
    Invisibility,
    Blindness,
    NightVision,
    Hunger,
    Weakness,
    Poison,
    Wither,
    HealthBoost,
    Absorption,
    Saturation,
    Glowing,
    Levitation,
    Luck,
    Unluck,
    SlowFalling,
    ConduitPower,
    DolphinsGrace,
    BadOmen,
    HeroOfTheVillage,
    Darkness,
};

inline constexpr usize kEffectCount = 33;

/// Ticks, for an effect that never runs out. 1.20.1's own encoding, in NBT and
/// on the wire alike (the Entity Effect capture carries the five-byte varint of
/// -1).
inline constexpr i32 kInfiniteDuration = -1;

[[nodiscard]] constexpr i32 effect_id(Effect effect) noexcept { return static_cast<i32>(effect); }

/// The attribute modifier an effect carries while it is active.
struct EffectModifier {
    Attribute          attribute;
    net::Uuid          uuid;
    /// The amount at amplifier 0. For the four percentage effects it is a
    /// **float** promoted to double — speed's 0.2 arrives as
    /// 0.20000000298023224 — and the amount at amplifier `a` is this times
    /// `a + 1`, multiplied in double. Measured at amplifiers 0, 1 and 4: speed
    /// V is 1.0000000149011612, which only that product gives.
    f64                per_level;
    AttributeOperation operation;
};

struct EffectInfo {
    /// "minecraft:speed".
    std::string_view name;
    /// Registry order, 1-based. Equal to `effect_id(effect)`.
    i32 id;
    /// The potion-swirl colour, 0xRRGGBB, as the metadata carries it.
    /// Measured one effect at a time on a cow; 0 where it could not be.
    u32 color;
    /// Applied once on arrival and never stored: instant health and damage.
    bool instantaneous;
    /// The modifier, for the nine effects that have one.
    std::optional<EffectModifier> modifier;
};

[[nodiscard]] const EffectInfo& effect_info(Effect effect) noexcept;

/// Nullopt for anything outside 1..33. From the wire or a save file, so the
/// answer to a bad id is a refusal, not speed.
[[nodiscard]] std::optional<Effect> effect_from_id(i32 id) noexcept;
[[nodiscard]] std::optional<Effect> effect_from_name(std::string_view name) noexcept;

/// The modifier's NBT name, e.g. "effect.minecraft.speed 2". Written into a
/// caller's buffer so nothing allocates. Returns the length, 0 when too small.
[[nodiscard]] usize modifier_name(Effect effect, u8 amplifier, char* out, usize capacity) noexcept;

/// The amount of an effect's modifier at an amplifier.
[[nodiscard]] f64 modifier_amount(const EffectModifier& modifier, u8 amplifier) noexcept;

/// One effect on one entity.
struct EffectInstance {
    Effect effect{Effect::Speed};
    /// Ticks left, or `kInfiniteDuration`.
    i32 duration{0};
    /// 0 is level I. A byte in NBT and on the wire, so 255 is the ceiling.
    u8 amplifier{0};
    /// A beacon's: fainter particles, and it does not count against "all
    /// ambient" in the metadata.
    bool ambient{false};
    bool visible{true};
    /// Shown in the inventory. `effect give … true` hides it together with the
    /// particles — measured, the flags byte came back 0x00.
    bool show_icon{true};

    [[nodiscard]] bool infinite() const noexcept { return duration == kInfiniteDuration; }

    friend bool operator==(const EffectInstance&, const EffectInstance&) noexcept = default;
};

/// What an effect can reach on the entity bearing it.
///
/// A server player, a mob and a client prediction each implement this; the
/// rules below never see which. Defaults are "does not have it": a cow has no
/// hunger to charge.
class EffectTarget {
public:
    virtual ~EffectTarget() = default;

    /// Undead are hurt by instant health and healed by instant damage, and
    /// refuse regeneration and poison. Arthropods refuse poison.
    enum class Body : u8 { Ordinary, Undead, Arthropod };

    [[nodiscard]] virtual Body body() const noexcept { return Body::Ordinary; }

    [[nodiscard]] virtual f32 health() const noexcept    = 0;
    [[nodiscard]] virtual f32 max_health() const noexcept = 0;

    virtual void heal(f32 amount) = 0;

    /// Hurt through the target's own damage rules — its invulnerability
    /// window, its resistance and its absorption. `constants` is the window to
    /// use; see `kEffectDamage`.
    virtual void hurt(DamageKind kind, f32 amount, const DamageConstants& constants) = 0;

    /// Hunger. Only a player has any.
    virtual void exhaust(f32 /*amount*/) {}
    virtual void feed(i32 /*nutrition*/, f32 /*saturation_modifier*/) {}

    [[nodiscard]] virtual f32 absorption() const noexcept { return 0.0F; }
    virtual void              set_absorption(f32 /*amount*/) {}

    /// The entity's attributes, or null for one that has none.
    [[nodiscard]] virtual AttributeMap* attributes() noexcept { return nullptr; }

    /// Clamp health after the maximum went down. Called when Health Boost
    /// ends; measured, a cow at 18 of 18 fell to 10 of 10.
    virtual void clamp_health() {}
};

/// The window effect damage is measured against: the twenty-tick window of
/// damage.hpp, entered at more than ten rather than at ten or more.
///
/// Why the two differ is phase, not rule: a console hit lands before the tick
/// in which the victim's counter is decremented, an effect hit after it. Both
/// campaigns are reproduced, each with its own constants — the survival one
/// untouched. See docs/provenance/effets.md § « la fenêtre ».
[[nodiscard]] DamageConstants effect_damage_constants(const DamageConstants& base) noexcept;

/// How an effect's arrival was received.
enum class AddResult : u8 {
    /// New.
    Added,
    /// Replaced or extended what was there, or pushed something into hiding.
    Updated,
    /// Nothing changed: a weaker, shorter one over a stronger.
    Unchanged,
    /// The body refuses it — undead and regeneration, spiders and poison.
    Immune,
    /// An instantaneous effect, applied once and not kept.
    Applied,
};

/// What changed during an `add`, `remove`, `clear` or `tick`, for the caller
/// who owes the client packets.
enum class EffectChange : u8 { Added, Updated, Removed };

struct EffectEvent {
    Effect       effect;
    EffectChange change;
};

/// Every effect on one entity.
///
/// Fixed storage: one slot per effect, each holding the active instance and up
/// to `kHiddenDepth - 1` hidden ones beneath it. The tick never allocates.
class ActiveEffects {
public:
    /// The measured chain was three deep (amplifiers 2 over 1 over 0). Four is
    /// room for that and one more; a fifth hidden effect is dropped, the
    /// shortest-lived first, and `dropped_hidden` counts it.
    static constexpr usize kHiddenDepth = 4;

    /// The amount of health each point of absorption effect grants, per level.
    static constexpr f32 kAbsorptionPerLevel = 4.0F;

    /// Apply an effect.
    ///
    /// The replacement rules, measured case by case:
    ///   * stronger over weaker — the stronger takes over, and the weaker is
    ///     kept hidden if it would have outlasted it;
    ///   * weaker over stronger — nothing changes, unless the weaker would
    ///     outlast the stronger, in which case it goes into hiding beneath it;
    ///   * equal — the longer duration wins; infinite outlasts everything.
    AddResult add(const EffectInstance& instance, EffectTarget& target);

    /// Remove one effect entirely, hidden ones included. False if absent.
    bool remove(Effect effect, EffectTarget& target);

    /// Remove every effect — milk, `/effect clear`. Returns how many.
    usize clear(EffectTarget& target);

    /// Forget everything without side effects on the target. Death: the
    /// player who respawns is a new entity in vanilla, and the client is told
    /// by Respawn rather than by thirty-three removals — measured, no Remove
    /// Entity Effect arrived at death.
    void forget() noexcept;

    /// One tick: every effect acts if its interval says so, counts down, and
    /// ends or gives way to its hidden successor. Runs in effect id order,
    /// which is a choice: vanilla iterates a hash map keyed by identity.
    void tick(EffectTarget& target);

    [[nodiscard]] const EffectInstance* get(Effect effect) const noexcept;
    [[nodiscard]] bool                  has(Effect effect) const noexcept;
    /// -1 when absent.
    [[nodiscard]] i32 amplifier(Effect effect) const noexcept;

    /// The hidden instance `depth` levels below the active one, or null.
    [[nodiscard]] const EffectInstance* hidden(Effect effect, usize depth) const noexcept;

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] bool  empty() const noexcept { return size() == 0; }

    /// Changes since `clear_events`, in the order they happened.
    [[nodiscard]] std::span<const EffectEvent> events() const noexcept {
        return {events_.data(), event_count_};
    }
    void clear_events() noexcept { event_count_ = 0; }

    /// The ticks since this holder was created. What an infinite effect counts
    /// its interval against — it has no duration to count down. Measured: an
    /// infinite regeneration healed 8 in 400 ticks, the finite rate.
    [[nodiscard]] i64 age() const noexcept { return age_; }

    [[nodiscard]] usize dropped_hidden() const noexcept { return dropped_hidden_; }

    // ── What the client is shown ────────────────────────────────────────────

    /// The swirl colour (living-entity metadata index 10), 0 with no visible
    /// effect.
    [[nodiscard]] u32 particle_color() const noexcept;

    /// Metadata index 11: true when every *visible* effect is ambient — which
    /// is also true when none is visible. Measured both ways: a hidden speed
    /// alone sent index 11 = true and no colour.
    [[nodiscard]] bool all_ambient() const noexcept;

    // ── What the other rules read ───────────────────────────────────────────

    /// Damage after Resistance. Types in #bypasses_resistance or
    /// #bypasses_effects pass unchanged.
    [[nodiscard]] f32 after_resistance(DamageKind kind, f32 amount) const noexcept;

private:
    struct Slot {
        std::array<EffectInstance, kHiddenDepth> chain{};
        u8                                       depth{0};
    };

    void record(Effect effect, EffectChange change) noexcept;
    void started(const EffectInstance& instance, EffectTarget& target);
    void ended(const EffectInstance& instance, EffectTarget& target);
    void hide(Slot& slot, usize at, const EffectInstance& instance);
    bool merge(Slot& slot, usize at, const EffectInstance& incoming);

    std::array<Slot, kEffectCount>            slots_{};
    /// The window an instant effect's hit is measured against — a console-
    /// phase hit, so the survival window unchanged. Periodic hits use
    /// `effect_damage_constants` of it.
    DamageConstants                           base_window_{};
    std::array<EffectEvent, 4 * kEffectCount> events_{};
    usize                                     event_count_{0};
    i64                                       age_{0};
    usize                                     dropped_hidden_{0};
};

/// Whether a body accepts an effect. Measured with `effect give` of all 33 to
/// a zombie, a skeleton, a spider and a cow.
[[nodiscard]] bool accepts(EffectTarget::Body body, Effect effect) noexcept;

/// The interval rule: does an effect of this amplifier act on a tick where the
/// counter reads `t`? (`t` is the duration left, or the holder's age for an
/// infinite effect.)
[[nodiscard]] bool acts_on(Effect effect, u8 amplifier, i64 t) noexcept;

// ── Food ────────────────────────────────────────────────────────────────────

/// An effect a food applies when eaten, with its chance.
struct FoodEffect {
    std::string_view item;
    Effect           effect;
    i32              duration;
    u8               amplifier;
    /// 1.0 for certain. Estimated from repeated eating, see effets.md.
    f32 probability;
};

/// The effects a food may apply, in the order it applies them. Empty for a
/// food that applies none.
[[nodiscard]] std::span<const FoodEffect> food_effects(std::string_view item) noexcept;

/// What finishing the use of an item does to the effects: the food's own,
/// honey removing poison, milk removing everything. Returns true when the item
/// was one of these, so the caller can tell "no effect" from "not handled".
bool consume_effects(ActiveEffects& effects, std::string_view item, EffectTarget& target,
                     math::XoroshiroRandomSource& random);

// ── Save format ─────────────────────────────────────────────────────────────

/// The `ActiveEffects` list, in 1.20.1's format — `Id` int, `Amplifier` byte,
/// `Duration` int, `Ambient`/`ShowParticles`/`ShowIcon` bytes, `HiddenEffect`
/// nested — verified against a playerdata file the real server wrote. Nullopt
/// when there is nothing: the real file omits the key rather than writing an
/// empty list.
[[nodiscard]] std::optional<nbt::Tag> save_effects(const ActiveEffects& effects);

/// Read an `ActiveEffects` list back. Unknown ids are skipped and counted, not
/// defaulted. Modifiers are re-applied to `target`; vanilla keeps them in the
/// `Attributes` list instead, which this server does not persist.
usize load_effects(ActiveEffects& effects, const nbt::Tag& list, EffectTarget& target);

}  // namespace ov::gameplay
