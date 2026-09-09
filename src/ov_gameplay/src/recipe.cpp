#include "ov/gameplay/recipe.hpp"

#include <algorithm>

namespace ov::gameplay {

RecipeBook::RecipeBook(const registry::Registries& registries)
    : registries_(&registries), data_(registries.recipes()) {
    filled_.resize(data_.recipes.size(), 0);

    for (RecipeIndex index = 0; index < data_.recipes.size(); ++index) {
        const registry::RecipeRecord& record = data_.recipes[index];
        by_kind_[record.kind].push_back(index);

        // A shaped recipe's blanks are ingredients with no choices, so the
        // filled count has to be counted rather than taken from the span
        // length. A shapeless recipe has no blanks and the two agree.
        u8 filled = 0;
        for (const auto& ingredient : ingredients(index)) {
            if (ingredient.choice_count != 0) {
                ++filled;
            }
        }
        filled_[index] = filled;

        const bool craftable =
            (record.kind == static_cast<u8>(registry::RecipeKind::CraftingShaped) ||
             record.kind == static_cast<u8>(registry::RecipeKind::CraftingShapeless)) &&
            (record.flags & registry::kFlagDeclarationOnly) == 0;
        if (craftable && filled >= 1 && filled <= 9) {
            by_filled_[filled].push_back(index);
        }
    }
}

std::optional<RecipeIndex> RecipeBook::find(std::string_view identifier) const noexcept {
    // Linear, and deliberately: this is a test and tooling path. Matching
    // never looks a recipe up by name.
    for (RecipeIndex index = 0; index < data_.recipes.size(); ++index) {
        if (registries_->recipe_name(index) == identifier) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<RecipeStack> RecipeBook::result(RecipeIndex index) const noexcept {
    if (index >= data_.recipes.size()) {
        return std::nullopt;
    }
    const registry::RecipeRecord& record = data_.recipes[index];
    if (record.result_item < 0) {
        return std::nullopt;
    }
    return RecipeStack{record.result_item, record.result_count};
}

bool RecipeBook::accepts(u32 ingredient, registry::ProtocolId item) const noexcept {
    if (ingredient >= data_.ingredients.size()) {
        return false;
    }
    const registry::RecipeIngredientRecord& record = data_.ingredients[ingredient];
    if (record.choice_count == 0) {
        return false;
    }
    const std::span<const i32> ids =
        data_.choices.subspan(record.choice_first, record.choice_count);
    return std::ranges::binary_search(ids, item);
}

std::span<const registry::RecipeIngredientRecord> RecipeBook::ingredients(
    RecipeIndex index) const noexcept {
    if (index >= data_.recipes.size()) {
        return {};
    }
    const registry::RecipeRecord& record = data_.recipes[index];
    return data_.ingredients.subspan(record.ingredient_first, record.ingredient_count);
}

std::span<const i32> RecipeBook::choices(u32 ingredient) const noexcept {
    if (ingredient >= data_.ingredients.size()) {
        return {};
    }
    const registry::RecipeIngredientRecord& record = data_.ingredients[ingredient];
    return data_.choices.subspan(record.choice_first, record.choice_count);
}

std::span<const RecipeIndex> RecipeBook::of_kind(registry::RecipeKind kind) const noexcept {
    return by_kind_[static_cast<usize>(kind)];
}

std::span<const RecipeIndex> RecipeBook::crafting_by_filled(usize filled) const noexcept {
    if (filled >= by_filled_.size()) {
        return {};
    }
    return by_filled_[filled];
}

}  // namespace ov::gameplay
