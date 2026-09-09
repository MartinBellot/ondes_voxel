#include "ov/gameplay/durability.hpp"

#include <algorithm>

namespace ov::gameplay {
namespace {

/// The measured maxima.
///
/// Bisected on a real 1.20.1 server: a tool handed over at `Damage: N` and made
/// to break one block either survives or does not, so thirteen round trips find
/// the boundary of a tool with two thousand uses. The number here is `N + 2`,
/// where `N` is the largest surviving damage — an item at N takes one more
/// point and is destroyed when it reaches the maximum, so the largest survivor
/// is two below it, not one. Reading it as `N + 1` costs every tool in the game
/// exactly one use, which no play session would ever notice.
///
/// The reading was checked the long way round on a golden pickaxe: it was worn
/// out from new, one stone block at a time, and the number of breaks it lasted
/// is what the bisection says.
///
/// Gold is the odd one out again, and in the other direction from mining: it is
/// the *fastest* tier and the shortest-lived, at 32 uses against wood's 59.
constexpr ItemDurability kDurability[] = {
    {"minecraft:wooden_pickaxe", 59},     {"minecraft:wooden_shovel", 59},
    {"minecraft:wooden_axe", 59},         {"minecraft:wooden_hoe", 59},
    {"minecraft:wooden_sword", 59},

    {"minecraft:stone_pickaxe", 131},     {"minecraft:stone_shovel", 131},
    {"minecraft:stone_axe", 131},         {"minecraft:stone_hoe", 131},
    {"minecraft:stone_sword", 131},

    {"minecraft:iron_pickaxe", 250},      {"minecraft:iron_shovel", 250},
    {"minecraft:iron_axe", 250},          {"minecraft:iron_hoe", 250},
    {"minecraft:iron_sword", 250},

    {"minecraft:golden_pickaxe", 32},     {"minecraft:golden_shovel", 32},
    {"minecraft:golden_axe", 32},         {"minecraft:golden_hoe", 32},
    {"minecraft:golden_sword", 32},

    {"minecraft:diamond_pickaxe", 1561},  {"minecraft:diamond_shovel", 1561},
    {"minecraft:diamond_axe", 1561},      {"minecraft:diamond_hoe", 1561},
    {"minecraft:diamond_sword", 1561},

    {"minecraft:netherite_pickaxe", 2031}, {"minecraft:netherite_shovel", 2031},
    {"minecraft:netherite_axe", 2031},     {"minecraft:netherite_hoe", 2031},
    {"minecraft:netherite_sword", 2031},

    {"minecraft:shears", 238},
};

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace

std::span<const ItemDurability> durability_table() noexcept { return kDurability; }

std::optional<i32> max_damage(std::string_view item) noexcept {
    for (const ItemDurability& row : kDurability) {
        if (row.item == item) {
            return row.max_damage;
        }
    }
    return std::nullopt;
}

i32 action_damage(std::string_view item, ToolAction action) noexcept {
    if (!max_damage(item)) {
        return 0;
    }
    switch (action) {
        case ToolAction::BreakInstantBlock:
            // Measured: a diamond pickaxe that broke a torch came back at
            // Damage 0. Vanilla charges only for a block that took time.
            return 0;

        case ToolAction::BreakBlock:
            // One for every tool, and two for a sword — a sword is not a
            // digging tool and vanilla charges it double for behaving like one.
            // Measured: a diamond sword that broke dirt came back at Damage 2,
            // a diamond pickaxe that broke stone at Damage 1.
            return ends_with(item, "_sword") ? 2 : 1;

        case ToolAction::Attack:
            // The other way round. A sword costs one, every other tool costs
            // two, and shears cost nothing at all — all three measured.
            if (item == "minecraft:shears") {
                return 0;
            }
            return ends_with(item, "_sword") ? 1 : 2;

        case ToolAction::UseOnBlock:
            // Tilling, stripping, path-making, lighting: one, measured on a
            // diamond hoe and on flint and steel.
            return 1;

        default: return 0;
    }
}

i32 unbreaking_survives(i32 amount, u8 unbreaking, bool is_armour,
                        math::XoroshiroRandomSource& random) noexcept {
    if (unbreaking == 0 || amount <= 0) {
        return amount;
    }
    i32 landed = amount;
    for (i32 i = 0; i < amount; ++i) {
        // Armour's own gate comes first, and it is drawn *whether or not* the
        // level check would have mattered. Folding the two into one probability
        // would give the same average and a different stream, and the stream is
        // what a client prediction has to match.
        if (is_armour && random.next_float() >= 0.6F) {
            continue;
        }
        if (random.next_int(static_cast<i32>(unbreaking) + 1) > 0) {
            --landed;
        }
    }
    return landed;
}

WearResult wear(std::string_view item, i32 current, ToolAction action, u8 unbreaking,
                math::XoroshiroRandomSource& random) noexcept {
    WearResult out;
    out.damage = current;

    const auto maximum = max_damage(item);
    if (!maximum) {
        return out;
    }
    const i32 asked = action_damage(item, action);
    if (asked <= 0) {
        return out;
    }
    out.applied = unbreaking_survives(asked, unbreaking, false, random);
    out.damage  = current + out.applied;
    if (out.damage >= *maximum) {
        out.broke = true;
    }
    return out;
}

}  // namespace ov::gameplay
