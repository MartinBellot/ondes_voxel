// Matching a crafting grid, and what happens when the result is taken.
//
// Two rules do all the work, and both are easy to get subtly wrong:
//
//   * a **shaped** recipe matches at any position its pattern fits in the grid,
//     **and** in its horizontal mirror. Vanilla mirrors; a recipe book that
//     does not makes half the players' muscle memory wrong, silently.
//   * a **shapeless** recipe ignores order, which is a bipartite matching and
//     not a sorted comparison: an ingredient may be a tag accepting forty
//     items, and two ingredients may overlap. Greedily assigning the first
//     ingredient that fits fails on exactly those overlaps.
//
// Taking the result consumes one of each used cell, and puts back whatever the
// item leaves behind — the empty bucket, the glass bottle. Those remainders are
// Java code in vanilla, so they were measured; see docs/provenance/.
#pragma once

#include "ov/gameplay/recipe.hpp"

#include <array>
#include <optional>
#include <span>

namespace ov::gameplay {

/// The largest grid the game has: three by three.
inline constexpr usize kMaxGridWidth  = 3;
inline constexpr usize kMaxGridCells  = kMaxGridWidth * kMaxGridWidth;

/// A crafting grid: cells in row-major order, and its shape.
///
/// The 2x2 of the player's inventory and the 3x3 of the table are the same
/// type. They differ only in `width` and `height`, which is why a recipe that
/// fits in two by two can be made in either.
struct CraftingGrid {
    std::array<RecipeStack, kMaxGridCells> cells{};
    u8                                     width{3};
    u8                                     height{3};

    [[nodiscard]] const RecipeStack& at(usize x, usize y) const noexcept {
        return cells[y * width + x];
    }

    [[nodiscard]] RecipeStack& at(usize x, usize y) noexcept { return cells[y * width + x]; }

    [[nodiscard]] usize cell_count() const noexcept {
        return static_cast<usize>(width) * height;
    }
};

/// What a grid matched.
struct CraftMatch {
    RecipeIndex recipe{0};
    RecipeStack result{};
};

/// The recipe a grid makes, if any.
///
/// Order of preference is the pack's own order — recipe-identifier order —
/// which is what vanilla uses too once its own map has been iterated. No
/// vanilla grid matches two recipes at once, and the parity probe checks that
/// claim on every grid it lays out rather than assuming it.
[[nodiscard]] std::optional<CraftMatch> match_crafting(const RecipeBook&   book,
                                                       const CraftingGrid& grid);

/// What taking one result costs and leaves behind.
struct CraftConsumption {
    /// The grid after one craft: each used cell one smaller, and the
    /// remainder in its place where the cell emptied.
    CraftingGrid grid{};

    /// Remainders that had nowhere to go, because the cell still held more of
    /// the ingredient. Vanilla hands these to the player, or drops them.
    std::array<RecipeStack, kMaxGridCells> overflow{};
    usize                                 overflow_count{0};
};

/// Consume one craft's worth of the grid.
///
/// Takes the match rather than re-deriving it: a caller that has just matched
/// must not risk matching something else between the two calls.
[[nodiscard]] CraftConsumption consume_craft(const RecipeBook& book, const CraftingGrid& grid,
                                             const CraftMatch& match);

/// How many times this grid could be crafted without refilling.
///
/// The smallest count among the cells the recipe uses. This is what a
/// shift-click on the result slot repeats, and the reason that click is a
/// special case rather than an ordinary move: it crafts in a loop until the
/// grid runs out or the player does.
[[nodiscard]] i32 max_crafts(const RecipeBook& book, const CraftingGrid& grid,
                             const CraftMatch& match);

}  // namespace ov::gameplay
