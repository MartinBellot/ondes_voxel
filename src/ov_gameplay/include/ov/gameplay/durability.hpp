// How long a tool lasts, and what wears it out.
//
// Durability is two numbers and one enchantment. The first number is the tool's
// maximum damage; the second is what one action costs it, and it is not always
// one — a sword swung at a mob costs one, a *pickaxe* swung at the same mob
// costs two, and breaking a block that comes off instantly costs nothing at
// all. Getting the second wrong makes every tool in the game last twice as long
// or half as long while the maximum looks perfectly correct.
//
// Both were measured against a real 1.20.1 server. The costs by making a bot
// perform each action once on a pristine tool and reading `Damage` back; the
// maxima by handing the bot a tool already at `Damage: N`, making it act once,
// and asking whether the tool was still there — bisected, so a tool with 2031
// uses is found in eleven round trips rather than two thousand. See
// scripts/measure_combat.py and docs/provenance/combat.md.
//
// Layer 9, and nothing here takes a world: an item wearing out is arithmetic on
// a stack, which is exactly what a client needs in order to predict it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// One item's maximum damage.
///
/// The number is what `Damage` may reach before the item is destroyed: a tool
/// at `max_damage - 1` survives one more point, one at `max_damage` is gone. So
/// a pristine tool has exactly `max_damage` points of wear in it, which for a
/// diamond pickaxe is 1561 and for a golden one is 32.
struct ItemDurability {
    std::string_view item;
    i32              max_damage;
};

/// The measured table.
[[nodiscard]] std::span<const ItemDurability> durability_table() noexcept;

/// The maximum damage of an item, or nothing for an item that has none.
///
/// Nothing rather than zero: an item with no durability and an item with none
/// left are different answers, and a zero here would destroy a stack of cobble
/// the first time it was used.
[[nodiscard]] std::optional<i32> max_damage(std::string_view item) noexcept;

/// The things a tool can be made to do, as far as its durability cares.
///
/// Named individually rather than folded into "use", because the cost differs
/// between them and the differences are the measurement:
///
///   * `BreakBlock` costs one — to any tool, whether or not it was the right
///     one. A sword breaking dirt is the exception and costs two.
///   * `BreakInstantBlock` costs nothing. A torch, a flower, a redstone dust:
///     vanilla charges only for a block that took time.
///   * `Attack` costs one for a sword and two for anything else. Shears cost
///     nothing at all, which is measured and is not an omission.
///   * `UseOnBlock` costs one: tilling, stripping, shovelling a path, lighting
///     a fire, shearing a beehive.
enum class ToolAction : u8 { BreakBlock, BreakInstantBlock, Attack, UseOnBlock };

/// What one action costs this item, in damage points.
///
/// Zero for an item with no durability, which is the honest answer rather than
/// a refusal: a player hitting a mob with a stone block does no damage to the
/// block, and the caller does not need to special-case it.
[[nodiscard]] i32 action_damage(std::string_view item, ToolAction action) noexcept;

/// How much of `amount` actually lands, given Unbreaking.
///
/// Unbreaking does not make a tool last longer by making each point smaller: it
/// makes each point *miss*, one draw per point. The chance a point is ignored
/// is `level / (level + 1)` for a tool and steeper for armour, which has a
/// separate 60% gate in front of it — armour with Unbreaking III lasts about
/// eleven times as long, a tool about four.
///
/// The draws are taken from the caller's own generator, in order, so a
/// prediction and the server agree when they share a seed.
[[nodiscard]] i32 unbreaking_survives(i32 amount, u8 unbreaking, bool is_armour,
                                      math::XoroshiroRandomSource& random) noexcept;

/// What happened to a stack that took damage.
struct WearResult {
    /// The item's new `Damage` value.
    i32 damage{0};

    /// The item is gone. `damage` is then meaningless and the stack should be
    /// removed rather than kept at its maximum.
    bool broke{false};

    /// How many points actually landed, after Unbreaking. Callers that show a
    /// durability bar need this rather than the amount they asked for.
    i32 applied{0};
};

/// Wear a stack by one action.
///
/// `current` is the item's `Damage` tag. An item with no durability is returned
/// untouched and unbroken, which is what lets a caller run this on every use
/// without asking first.
[[nodiscard]] WearResult wear(std::string_view item, i32 current, ToolAction action, u8 unbreaking,
                              math::XoroshiroRandomSource& random) noexcept;

}  // namespace ov::gameplay
