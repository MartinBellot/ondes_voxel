#include "ov/gameplay/crafting.hpp"

#include <algorithm>

namespace ov::gameplay {
namespace {

/// The smallest rectangle that holds everything in the grid.
struct Bounds {
    usize min_x{0};
    usize min_y{0};
    usize width{0};
    usize height{0};
    usize filled{0};
};

[[nodiscard]] Bounds bounds_of(const CraftingGrid& grid) {
    Bounds box;
    usize  max_x = 0;
    usize  max_y = 0;
    bool   any   = false;
    for (usize y = 0; y < grid.height; ++y) {
        for (usize x = 0; x < grid.width; ++x) {
            if (grid.at(x, y).empty()) {
                continue;
            }
            ++box.filled;
            if (!any) {
                box.min_x = max_x = x;
                box.min_y = max_y = y;
                any               = true;
                continue;
            }
            box.min_x = std::min(box.min_x, x);
            box.min_y = std::min(box.min_y, y);
            max_x     = std::max(max_x, x);
            max_y     = std::max(max_y, y);
        }
    }
    if (any) {
        box.width  = max_x - box.min_x + 1;
        box.height = max_y - box.min_y + 1;
    }
    return box;
}

/// Does the recipe's pattern sit exactly on the grid's bounding box?
///
/// `mirrored` reads the pattern's columns right to left. Vanilla accepts both,
/// and the grid is compared against the whole bounding box rather than only the
/// filled cells: a pattern's blank must face an empty cell, or a stick dropped
/// in the hole of a bucket recipe would still craft.
[[nodiscard]] bool pattern_fits(const RecipeBook& book, RecipeIndex index,
                               const CraftingGrid& grid, const Bounds& box, bool mirrored) {
    const registry::RecipeRecord& record      = book.recipe(index);
    const auto                    ingredients = book.ingredients(index);
    const usize                   width       = record.width;

    for (usize y = 0; y < record.height; ++y) {
        for (usize x = 0; x < width; ++x) {
            const usize source = y * width + (mirrored ? width - 1 - x : x);
            const RecipeStack& cell = grid.at(box.min_x + x, box.min_y + y);
            const u32          ingredient = record.ingredient_first + static_cast<u32>(source);
            if (ingredients[source].choice_count == 0) {
                if (!cell.empty()) {
                    return false;
                }
                continue;
            }
            if (cell.empty() || !book.accepts(ingredient, cell.item)) {
                return false;
            }
        }
    }
    return true;
}

/// Can every grid item be paired with a distinct ingredient?
///
/// A backtracking assignment, not a greedy one. Two ingredients of the same
/// recipe often overlap — `#planks` and a specific plank — and greedily giving
/// an item to the first ingredient that accepts it can strand a later item that
/// only one ingredient would have taken. With at most nine of each, the search
/// is bounded and finishes immediately in practice.
[[nodiscard]] bool assign(const RecipeBook& book, RecipeIndex index,
                         std::span<const registry::ProtocolId> items, usize position,
                         u32 used_mask) {
    if (position == items.size()) {
        return true;
    }
    const registry::RecipeRecord& record = book.recipe(index);
    for (u32 slot = 0; slot < record.ingredient_count; ++slot) {
        const u32 bit = 1U << slot;
        if ((used_mask & bit) != 0) {
            continue;
        }
        if (!book.accepts(record.ingredient_first + slot, items[position])) {
            continue;
        }
        if (assign(book, index, items, position + 1, used_mask | bit)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<CraftMatch> match_crafting(const RecipeBook& book, const CraftingGrid& grid) {
    const Bounds box = bounds_of(grid);
    if (box.filled == 0) {
        return std::nullopt;
    }

    // Only recipes that fill exactly as many cells can match. This is the whole
    // index: the buckets average a tenth of the book, and everything else is
    // decided per candidate.
    for (const RecipeIndex index : book.crafting_by_filled(box.filled)) {
        const registry::RecipeRecord& record = book.recipe(index);
        if (record.kind == static_cast<u8>(registry::RecipeKind::CraftingShaped)) {
            if (record.width != box.width || record.height != box.height) {
                continue;
            }
            if (!pattern_fits(book, index, grid, box, false) &&
                !pattern_fits(book, index, grid, box, true)) {
                continue;
            }
        } else {
            std::array<registry::ProtocolId, kMaxGridCells> items{};
            usize                                          count = 0;
            for (usize cell = 0; cell < grid.cell_count(); ++cell) {
                if (!grid.cells[cell].empty()) {
                    items[count++] = grid.cells[cell].item;
                }
            }
            if (count != record.ingredient_count) {
                continue;
            }
            if (!assign(book, index, std::span{items.data(), count}, 0, 0)) {
                continue;
            }
        }

        const auto result = book.result(index);
        if (!result) {
            // A recipe in this bucket with no result of its own would be a
            // pack that disagrees with itself; refuse rather than craft air.
            continue;
        }
        return CraftMatch{index, *result};
    }
    return std::nullopt;
}

CraftConsumption consume_craft(const RecipeBook& book, const CraftingGrid& grid,
                               const CraftMatch& match) {
    CraftConsumption out;
    out.grid = grid;
    (void)match;

    // Every non-empty cell is used exactly once by any matching recipe: a
    // shaped pattern's blanks face empty cells, and a shapeless recipe pairs
    // one ingredient per item. So consumption does not need to know which
    // cell went to which ingredient.
    for (usize cell = 0; cell < out.grid.cell_count(); ++cell) {
        RecipeStack& stack = out.grid.cells[cell];
        if (stack.empty()) {
            continue;
        }
        const auto left = book.remainder(stack.item);
        --stack.count;
        if (!left) {
            if (stack.count <= 0) {
                stack = RecipeStack{};
            }
            continue;
        }
        if (stack.count <= 0) {
            stack = RecipeStack{*left, 1};
        } else {
            // The cell still holds more of the ingredient, so the bucket that
            // came back has nowhere to sit. Vanilla hands it to the player
            // rather than deleting it, and so does the caller.
            out.overflow[out.overflow_count++] = RecipeStack{*left, 1};
        }
    }
    return out;
}

i32 max_crafts(const RecipeBook& book, const CraftingGrid& grid, const CraftMatch& match) {
    (void)book;
    (void)match;
    i32 smallest = 0;
    for (usize cell = 0; cell < grid.cell_count(); ++cell) {
        const RecipeStack& stack = grid.cells[cell];
        if (stack.empty()) {
            continue;
        }
        smallest = smallest == 0 ? stack.count : std::min(smallest, stack.count);
    }
    return smallest;
}

}  // namespace ov::gameplay
