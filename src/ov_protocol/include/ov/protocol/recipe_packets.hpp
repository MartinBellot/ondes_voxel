// The packets a crafting screen and a recipe book need.
//
// Kept out of play.cpp on purpose: Update Recipes alone is a few hundred
// kilobytes of nested, per-kind encoding, and mixing it into the file that
// carries the join sequence would make both harder to read.
//
// The ids here were **not** written from a table. They were captured off a real
// 1.20.1 server by scripts/capture_recipe_packets.py, which connects, opens a
// furnace and records what arrives — the same method the rest of this module
// uses, and for the same reason: the archived wiki disagreed on every id this
// project has checked.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/play.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::net {

namespace clientbound {
/// Set Container Property. One 16-bit number in one window, and the only way a
/// furnace's two bars ever move: the client draws them from these and computes
/// nothing.
inline constexpr i32 kContainerProperty = 0x13;

/// Update Recipes — every recipe the server knows, sent once after login. The
/// client needs it for its recipe book and for the "place from book" button.
inline constexpr i32 kUpdateRecipes = 0x6D;

/// Update Recipe Book — which recipes this player has unlocked, and which of
/// the book's tabs are open.
inline constexpr i32 kUpdateRecipeBook = 0x3B;
}  // namespace clientbound

/// One property of a window: 0..3 on a furnace are fuel left, fuel at ignition,
/// cooking progress and how long the recipe takes.
[[nodiscard]] std::vector<u8> encode_container_property(u8 window_id, i16 property, i16 value);

/// One ingredient, as the wire wants it: the list of stacks that satisfy it.
///
/// A tag has already been flattened into items by the time it gets here, which
/// is what the client expects — it has no idea our tags exist.
struct WireIngredient {
    std::span<const i32> items;
};

/// One recipe, in the shape Update Recipes carries.
///
/// The union of every kind's fields, because the encoder switches on `type` and
/// a separate struct per kind would be nine structs to say what nine `if`s say.
struct WireRecipe {
    std::string_view identifier;
    std::string_view type;
    std::string_view group;
    /// The client's own category enumeration for this recipe's kind.
    i32 category{0};
    /// Shaped recipes only.
    i32 width{0};
    i32 height{0};
    bool show_notification{false};
    /// Cooking recipes only.
    f32 experience{0.0F};
    i32 cooking_time{0};
    std::span<const WireIngredient> ingredients;
    /// The result, or an empty stack for the kinds that have none.
    ItemStack result;
};

/// Every recipe the server knows.
///
/// One packet, several hundred kilobytes for vanilla's 1174. Vanilla sends it
/// once, right after the join sequence, and so should we: a client that never
/// gets it works, but its recipe book stays empty and the button that fills a
/// grid from the book does nothing.
[[nodiscard]] std::vector<u8> encode_update_recipes(std::span<const WireRecipe> recipes);

/// The state of the player's recipe book.
///
/// `action` 0 initialises it, 1 adds recipes, 2 removes them. The four booleans
/// are the open/filter state of the crafting, furnace, blast furnace and smoker
/// tabs, in that order.
struct RecipeBookState {
    i32                                   action{0};
    bool                                  crafting_open{false};
    bool                                  crafting_filter{false};
    bool                                  furnace_open{false};
    bool                                  furnace_filter{false};
    bool                                  blast_open{false};
    bool                                  blast_filter{false};
    bool                                  smoker_open{false};
    bool                                  smoker_filter{false};
    std::span<const std::string_view>     recipes;
    /// Only sent for action 0: the recipes shown as newly unlocked.
    std::span<const std::string_view> highlighted;
};

[[nodiscard]] std::vector<u8> encode_update_recipe_book(const RecipeBookState& state);

}  // namespace ov::net
