#include "ov/gameplay/dispenser.hpp"

namespace ov::gameplay {
namespace {

/// The measured table.
///
/// Every row here came off a real 1.20.1 server: one dispenser and one dropper
/// per item, triggered by a redstone block placed **adjacent** and **last**,
/// looked at six ticks later, and read three ways — the block on the target
/// square, the entity that appeared and whether it was a plain
/// `minecraft:item`, and what the machine still held. See
/// docs/provenance/redstone.md §13.
struct Row {
    std::string_view item;
    DispenseKind     kind;
    std::string_view entity;
    std::string_view block;
    std::string_view replacement;
    bool             damages;
};

constexpr std::array<Row, 12> kMeasured{{
    // A bucket is replaced, not consumed. The measurement cell came back
    // holding `minecraft:bucket`, which is the whole rule.
    {"minecraft:water_bucket", DispenseKind::PlaceFluid, {}, "minecraft:water",
     "minecraft:bucket", false},
    {"minecraft:lava_bucket", DispenseKind::PlaceFluid, {}, "minecraft:lava",
     "minecraft:bucket", false},
    // Neither consumed nor replaced: it came back with `Damage: 1`.
    {"minecraft:flint_and_steel", DispenseKind::Ignite, {}, "minecraft:fire", {}, true},
    {"minecraft:fire_charge", DispenseKind::Projectile, "minecraft:small_fireball", {}, {}, false},
    {"minecraft:egg", DispenseKind::Projectile, "minecraft:egg", {}, {}, false},
    {"minecraft:splash_potion", DispenseKind::Projectile, "minecraft:potion", {}, {}, false},
    {"minecraft:lingering_potion", DispenseKind::Projectile, "minecraft:potion", {}, {}, false},
    // The measurement's own dispenser was destroyed by what it dispensed, which
    // is the strongest evidence available that this is a priming and not a drop.
    {"minecraft:tnt", DispenseKind::PrimeTnt, "minecraft:tnt", {}, {}, false},
    {"minecraft:arrow", DispenseKind::Projectile, "minecraft:arrow", {}, {}, false},
    {"minecraft:spectral_arrow", DispenseKind::Projectile, "minecraft:spectral_arrow", {}, {},
     false},
    // Both of these read as an ordinary ejected item until the measurement was
    // asked the one question that settles it — is the entity a
    // `minecraft:item`? A thrown snowball and a dropped snowball are both
    // called "Snowball", and so is a firework. They are not the same thing.
    {"minecraft:snowball", DispenseKind::Projectile, "minecraft:snowball", {}, {}, false},
    {"minecraft:firework_rocket", DispenseKind::Projectile, "minecraft:firework_rocket", {}, {},
     false},
}};

/// Items vanilla gives a behaviour to that this table does not carry.
///
/// Named rather than defaulted, and the list splits in two.
///
/// **Measured and still refused.** Bone meal and shears were in the campaign
/// and came back having produced **no entity at all** while leaving the machine
/// empty — so the item did something, and this table does not know what. Boats
/// and glass bottles came back as plain ejected items, but only because the
/// target square was air: both have a behaviour that depends on what is in
/// front of them, and only the air case was measured. Recording "eject" from
/// the air case would be recording half a rule as the whole of it.
///
/// **Not measured at all.** The rest, listed because vanilla has a behaviour
/// for each and the default is plausible for every one of them — a shulker box
/// on the floor instead of placed looks like a dispenser that works.
constexpr std::array<std::string_view, 12> kUnhandled{
    "minecraft:bone_meal",          "minecraft:shears",
    "minecraft:glass_bottle",       "minecraft:oak_boat",
    "minecraft:powder_snow_bucket", "minecraft:cod_bucket",
    "minecraft:tropical_fish_bucket", "minecraft:salmon_bucket",
    "minecraft:pufferfish_bucket",  "minecraft:axolotl_bucket",
    "minecraft:tadpole_bucket",     "minecraft:carved_pumpkin",
};

/// The spawn eggs are a family and not a list: every `X_spawn_egg` spawns an
/// `X`, and tabulating eighty of them would be eighty chances to mistype one.
[[nodiscard]] bool spawn_egg_target(std::string_view item, std::string_view& mob) noexcept {
    constexpr std::string_view prefix = "minecraft:";
    constexpr std::string_view suffix = "_spawn_egg";
    if (!item.starts_with(prefix) || !item.ends_with(suffix)) {
        return false;
    }
    mob = item.substr(0, item.size() - suffix.size());
    return !mob.empty();
}

}  // namespace

Dispenser::Dispenser(const registry::Registries& registries) {
    const auto items = registries.find("minecraft:item");
    if (!items) {
        return;
    }
    const std::span<const std::string_view> names = registries.entries(*items);
    actions_.assign(names.size(), DispenseAction{DispenseKind::Eject});

    const auto index_of = [&](std::string_view name) -> i64 {
        const auto id = registries.protocol_id(*items, name);
        if (!id) {
            return -1;
        }
        const i64 offset = static_cast<i64>(*id) - static_cast<i64>(registries.first_id(*items));
        return offset >= 0 && static_cast<usize>(offset) < actions_.size() ? offset : -1;
    };

    for (const Row& row : kMeasured) {
        const i64 at = index_of(row.item);
        if (at < 0) {
            continue;
        }
        actions_[static_cast<usize>(at)] =
            DispenseAction{row.kind, row.entity, row.block, row.replacement, row.damages, {}};
    }

    for (usize i = 0; i < names.size(); ++i) {
        std::string_view mob;
        if (spawn_egg_target(names[i], mob)) {
            actions_[i] = DispenseAction{DispenseKind::SpawnMob, mob, {}, {}, false, {}};
        }
    }

    for (const std::string_view name : kUnhandled) {
        const i64 at = index_of(name);
        if (at < 0) {
            continue;
        }
        actions_[static_cast<usize>(at)] =
            DispenseAction{DispenseKind::Refused, {}, {}, {}, false,
                           "vanilla gives this item a dispense behaviour that has not "
                           "been measured; it must not be ejected as a plain item"};
        unhandled_.push_back(name);
    }
}

DispenseAction Dispenser::action_for(registry::ProtocolId item) const noexcept {
    // A protocol id outside the table is not "eject it": it is an item this
    // build does not have, and the caller must not act on it at all.
    if (item < 0 || static_cast<usize>(item) >= actions_.size()) {
        return DispenseAction{DispenseKind::Refused, {}, {}, {}, false,
                              "item id is outside this build's item registry"};
    }
    return actions_[static_cast<usize>(item)];
}

i32 Dispenser::slot_to_fire(const ItemContainer& machine) {
    const i32 count = machine.slot_count();
    for (i32 index = 0; index < count; ++index) {
        if (!machine.slot(index).empty()) {
            return index;
        }
    }
    return -1;
}

}  // namespace ov::gameplay
