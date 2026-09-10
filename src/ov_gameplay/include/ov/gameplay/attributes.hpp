// Attributes: a base value, a handful of modifiers, and a clamp.
//
// An attribute in Minecraft is not a number. It is a *base* — what the entity
// type is born with — plus a set of modifiers, each identified by a UUID, each
// applied by one of three operations, and a range the result is clamped into.
// Speed from a potion, the attack damage of the sword in hand and the extra
// hearts of Health Boost are all modifiers on someone's attribute, and they
// come off again by UUID when the thing that put them there goes away.
//
// Everything here was asked of a real 1.20.1 server by scripts/measure_effects.py:
//
//   * the clamp of every one of the thirteen attributes, by setting the base to
//     plus and minus a billion and reading `attribute … get` back. The base
//     itself is stored unclamped — `base get` answers the billion — and only
//     the value is clamped;
//   * the three operations and their order, on a zombie's movement speed with
//     modifiers added from the console and the total read back after each one,
//     compared bit for bit (the console prints Java's Double.toString, which
//     round-trips);
//   * the order of modifiers *within* one operation, which is visible in the
//     last bit of a sum of doubles. See `AttributeInstance::value`.
//
// See docs/provenance/effets.md.
//
// Layer 9. The rules live here rather than on EntityState (layer 8) because a
// client predicting its own speed under a potion needs exactly this arithmetic,
// and a server-only copy would be the second implementation that drifts.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// The thirteen attributes of 1.20.1, in `minecraft:attribute` registry order.
/// The numeric value **is** the registry id; a test holds the two together.
enum class Attribute : u8 {
    MaxHealth                 = 0,
    FollowRange               = 1,
    KnockbackResistance       = 2,
    MovementSpeed             = 3,
    FlyingSpeed               = 4,
    AttackDamage              = 5,
    AttackKnockback           = 6,
    AttackSpeed               = 7,
    Armor                     = 8,
    ArmorToughness            = 9,
    Luck                      = 10,
    ZombieSpawnReinforcements = 11,
    HorseJumpStrength         = 12,
};

inline constexpr usize kAttributeCount = 13;

/// What the game says about one attribute, independent of any entity.
struct AttributeInfo {
    /// The registry name — also the string the Update Attributes packet
    /// carries, which is one of the few places the protocol spells a name out.
    std::string_view name;
    /// The clamp. Measured, one attribute at a time.
    f64 min;
    f64 max;
};

[[nodiscard]] const AttributeInfo& attribute_info(Attribute attribute) noexcept;

/// Nullopt for anything that is not a 1.20.1 attribute. Refused, never
/// defaulted: a modifier on an unknown attribute would otherwise land on max
/// health.
[[nodiscard]] std::optional<Attribute> attribute_from_name(std::string_view name) noexcept;

/// The three operations, numbered as the wire and the save file number them.
///
/// The console names them `add`, `multiply_base` and `multiply`; the NBT and the
/// packet carry 0, 1 and 2. Measured: a modifier added as `multiply` from the
/// console reads back with `Operation: 2`.
enum class AttributeOperation : u8 {
    /// Added to the base, before anything multiplies.
    Addition = 0,
    /// `total += base' * amount`, where base' is the base after every addition.
    MultiplyBase = 1,
    /// `total *= 1 + amount`, after everything else.
    MultiplyTotal = 2,
};

/// One modifier.
struct AttributeModifier {
    /// Identity. Two modifiers with the same UUID are the same modifier, and
    /// the game refuses the second: "Modifier … is already present".
    net::Uuid uuid{};
    /// The name the save file carries. Static storage: every modifier this
    /// project creates is one of a fixed set, and a string per modifier would
    /// allocate on the tick.
    std::string_view name{};
    f64              amount{0.0};
    AttributeOperation operation{AttributeOperation::Addition};
};

enum class ModifierError : u8 {
    /// The UUID is already there. The game's own refusal, measured.
    AlreadyPresent,
    /// More than `AttributeInstance::kCapacity` modifiers on one attribute.
    /// Not a game rule: a limit of this implementation, which keeps the tick
    /// free of allocation. Refused and named rather than silently dropped.
    Full,
};

[[nodiscard]] std::string_view to_string(ModifierError error) noexcept;

/// One attribute on one entity.
class AttributeInstance {
public:
    /// Sixteen: the most a vanilla entity carries on one attribute is a
    /// handful (armour pieces, an effect, a held item), and the no-allocation
    /// rule wants a fixed bound.
    static constexpr usize kCapacity = 16;

    AttributeInstance() = default;
    AttributeInstance(Attribute attribute, f64 base) noexcept;

    [[nodiscard]] Attribute attribute() const noexcept { return attribute_; }

    /// Stored as given, **unclamped**. Measured: `base set 1000000000` on a
    /// zombie's armour reads back a billion from `base get` and thirty from
    /// `get`.
    [[nodiscard]] f64 base() const noexcept { return base_; }
    void              set_base(f64 base) noexcept;

    [[nodiscard]] std::expected<void, ModifierError> add_modifier(
        const AttributeModifier& modifier) noexcept;

    /// True when something was removed.
    bool remove_modifier(const net::Uuid& uuid) noexcept;

    [[nodiscard]] const AttributeModifier* find(const net::Uuid& uuid) const noexcept;

    /// Insertion order. Not the order `value` applies them in.
    [[nodiscard]] std::span<const AttributeModifier> modifiers() const noexcept {
        return {modifiers_.data(), count_};
    }

    /// The value the game uses: base, additions, multiply-base, multiply-total,
    /// clamped. Recomputed on every call — at most sixteen terms.
    [[nodiscard]] f64 value() const noexcept;

    /// Changed since the last `clear_dirty` — base or modifiers. What the
    /// server reads to decide whether Update Attributes owes this attribute.
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void               clear_dirty() noexcept { dirty_ = false; }

private:
    Attribute                                attribute_{Attribute::MaxHealth};
    f64                                      base_{0.0};
    std::array<AttributeModifier, kCapacity> modifiers_{};
    u8                                       count_{0};
    bool                                     dirty_{false};
};

/// The attributes one entity owns.
///
/// Absent is not zero — a cow has no attack damage at all — so each slot is
/// optional and a query for an attribute the entity does not own answers
/// nullopt.
class AttributeMap {
public:
    /// Give the entity an attribute at a base value. Replaces any existing one.
    void own(Attribute attribute, f64 base) noexcept;

    [[nodiscard]] bool owns(Attribute attribute) const noexcept;

    [[nodiscard]] AttributeInstance*       get(Attribute attribute) noexcept;
    [[nodiscard]] const AttributeInstance* get(Attribute attribute) const noexcept;

    /// The clamped value, or nullopt when not owned.
    [[nodiscard]] std::optional<f64> value(Attribute attribute) const noexcept;

    /// The attributes of a player, at the bases measured on a real server:
    /// max health 20, knockback resistance 0, movement speed 0.1, attack
    /// damage 1, attack speed 4, armour 0, armour toughness 0, luck 0.
    [[nodiscard]] static AttributeMap player() noexcept;

private:
    std::array<std::optional<AttributeInstance>, kAttributeCount> slots_{};
};

/// The bucket Java's HashMap would put a UUID in, for a table of `table_size`.
///
/// Exposed because it is the whole of the ordering rule inside `value`, and a
/// test that checks the rule against the measured totals wants to see it.
[[nodiscard]] u32 java_hash_bucket(const net::Uuid& uuid, u32 table_size) noexcept;

}  // namespace ov::gameplay
