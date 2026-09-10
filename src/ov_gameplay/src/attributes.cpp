#include "ov/gameplay/attributes.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

// The clamp of every attribute, measured on a real 1.20.1 server: the base was
// set to +1e9 and to -1e9 on an entity that owns the attribute (a zombie for
// most, the bot for attack speed and luck, a horse and a parrot for the two
// that only they carry) and `attribute … get` read back.
//
// Registry order. `attribute_info` indexes this by the enum's value.
constexpr std::array<AttributeInfo, kAttributeCount> kAttributes{{
    {"minecraft:generic.max_health", 1.0, 1024.0},
    {"minecraft:generic.follow_range", 0.0, 2048.0},
    {"minecraft:generic.knockback_resistance", 0.0, 1.0},
    {"minecraft:generic.movement_speed", 0.0, 1024.0},
    {"minecraft:generic.flying_speed", 0.0, 1024.0},
    {"minecraft:generic.attack_damage", 0.0, 2048.0},
    {"minecraft:generic.attack_knockback", 0.0, 5.0},
    {"minecraft:generic.attack_speed", 0.0, 1024.0},
    {"minecraft:generic.armor", 0.0, 30.0},
    {"minecraft:generic.armor_toughness", 0.0, 20.0},
    {"minecraft:generic.luck", -1024.0, 1024.0},
    {"minecraft:zombie.spawn_reinforcements", 0.0, 1.0},
    {"minecraft:horse.jump_strength", 0.0, 2.0},
}};

/// Java's `UUID.hashCode`: the two halves XORed, then folded to 32 bits.
[[nodiscard]] u32 java_uuid_hash(const net::Uuid& uuid) noexcept {
    const u64 hilo = uuid.most_significant ^ uuid.least_significant;
    return static_cast<u32>(hilo >> 32U) ^ static_cast<u32>(hilo);
}

/// The table a Java HashSet holding `count` entries has grown to: sixteen,
/// doubling each time the count passes three quarters of it.
///
/// An approximation in one direction, named: a Java set never shrinks, so one
/// that once held thirteen modifiers keeps a table of thirty-two after they are
/// removed. This one recomputes from the current count. It only matters past
/// twelve modifiers of one operation on one attribute.
[[nodiscard]] u32 table_for(usize count) noexcept {
    u32 table = 16;
    while (static_cast<f64>(count) > 0.75 * static_cast<f64>(table)) {
        table *= 2;
    }
    return table;
}

}  // namespace

const AttributeInfo& attribute_info(Attribute attribute) noexcept {
    const auto index = static_cast<usize>(attribute);
    return kAttributes[index < kAttributes.size() ? index : 0];
}

std::optional<Attribute> attribute_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kAttributes.size(); ++i) {
        if (kAttributes[i].name == name) {
            return static_cast<Attribute>(i);
        }
    }
    return std::nullopt;
}

std::string_view to_string(ModifierError error) noexcept {
    switch (error) {
        case ModifierError::AlreadyPresent: return "modifier already present";
        case ModifierError::Full: return "too many modifiers on one attribute";
    }
    return "unknown modifier error";
}

u32 java_hash_bucket(const net::Uuid& uuid, u32 table_size) noexcept {
    const u32 hash   = java_uuid_hash(uuid);
    const u32 spread = hash ^ (hash >> 16U);
    return spread & (table_size - 1U);
}

AttributeInstance::AttributeInstance(Attribute attribute, f64 base) noexcept
    : attribute_{attribute}, base_{base}, dirty_{true} {}

void AttributeInstance::set_base(f64 base) noexcept {
    if (base != base_) {
        base_  = base;
        dirty_ = true;
    }
}

std::expected<void, ModifierError> AttributeInstance::add_modifier(
    const AttributeModifier& modifier) noexcept {
    if (find(modifier.uuid) != nullptr) {
        return std::unexpected(ModifierError::AlreadyPresent);
    }
    if (count_ >= kCapacity) {
        return std::unexpected(ModifierError::Full);
    }
    modifiers_[count_] = modifier;
    ++count_;
    dirty_ = true;
    return {};
}

bool AttributeInstance::remove_modifier(const net::Uuid& uuid) noexcept {
    for (usize i = 0; i < count_; ++i) {
        if (modifiers_[i].uuid == uuid) {
            // Shift rather than swap: insertion order is the tie-break inside
            // a hash bucket, and a swap would reorder the survivors.
            for (usize j = i + 1; j < count_; ++j) {
                modifiers_[j - 1] = modifiers_[j];
            }
            --count_;
            modifiers_[count_] = AttributeModifier{};
            dirty_             = true;
            return true;
        }
    }
    return false;
}

const AttributeModifier* AttributeInstance::find(const net::Uuid& uuid) const noexcept {
    for (usize i = 0; i < count_; ++i) {
        if (modifiers_[i].uuid == uuid) {
            return &modifiers_[i];
        }
    }
    return nullptr;
}

f64 AttributeInstance::value() const noexcept {
    // The modifiers of one operation, in the order the game applies them.
    //
    // Addition and multiplication of doubles are not associative, so this
    // order is visible in the last bit. The game keeps each operation's
    // modifiers in a hash set keyed by UUID; its iteration order is bucket
    // order, and insertion order inside a bucket. Measured, not assumed — see
    // the `order` campaign in scripts/measure_effects.py, where two modifiers
    // inserted against their bucket order sum to the bucket-order bits.
    std::array<u8, kCapacity> order{};
    const auto                apply = [&](AttributeOperation operation, auto&& each) {
        usize n = 0;
        for (usize i = 0; i < count_; ++i) {
            if (modifiers_[i].operation == operation) {
                order[n++] = static_cast<u8>(i);
            }
        }
        const u32 table = table_for(n);
        // Stable: equal buckets keep insertion order, which is the linked
        // list inside one Java bucket.
        std::stable_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(n),
                         [&](u8 a, u8 b) {
                             return java_hash_bucket(modifiers_[a].uuid, table) <
                                    java_hash_bucket(modifiers_[b].uuid, table);
                         });
        for (usize k = 0; k < n; ++k) {
            each(modifiers_[order[k]].amount);
        }
    };

    f64 base = base_;
    apply(AttributeOperation::Addition, [&](f64 amount) { base += amount; });
    f64 total = base;
    apply(AttributeOperation::MultiplyBase, [&](f64 amount) { total += base * amount; });
    apply(AttributeOperation::MultiplyTotal, [&](f64 amount) { total *= 1.0 + amount; });

    const AttributeInfo& info = attribute_info(attribute_);
    if (std::isnan(total)) {
        return info.min;
    }
    return std::clamp(total, info.min, info.max);
}

void AttributeMap::own(Attribute attribute, f64 base) noexcept {
    slots_[static_cast<usize>(attribute)] = AttributeInstance{attribute, base};
}

bool AttributeMap::owns(Attribute attribute) const noexcept {
    return slots_[static_cast<usize>(attribute)].has_value();
}

AttributeInstance* AttributeMap::get(Attribute attribute) noexcept {
    auto& slot = slots_[static_cast<usize>(attribute)];
    return slot ? &*slot : nullptr;
}

const AttributeInstance* AttributeMap::get(Attribute attribute) const noexcept {
    const auto& slot = slots_[static_cast<usize>(attribute)];
    return slot ? &*slot : nullptr;
}

std::optional<f64> AttributeMap::value(Attribute attribute) const noexcept {
    const AttributeInstance* instance = get(attribute);
    if (instance == nullptr) {
        return std::nullopt;
    }
    return instance->value();
}

AttributeMap AttributeMap::player() noexcept {
    // Measured on the bot: `attribute <bot> <name> base get` for each.
    AttributeMap map;
    map.own(Attribute::MaxHealth, 20.0);
    map.own(Attribute::KnockbackResistance, 0.0);
    map.own(Attribute::MovementSpeed, 0.10000000149011612);
    map.own(Attribute::AttackDamage, 1.0);
    map.own(Attribute::AttackSpeed, 4.0);
    map.own(Attribute::Armor, 0.0);
    map.own(Attribute::ArmorToughness, 0.0);
    map.own(Attribute::Luck, 0.0);
    return map;
}

}  // namespace ov::gameplay
