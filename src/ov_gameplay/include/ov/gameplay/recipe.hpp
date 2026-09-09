// The recipe book: what the game knows how to make.
//
// The recipes themselves are data — vanilla's own, regenerated locally and
// flattened into the pack — so what lives here is the *matching*, which is
// code. That split is the same one the loot tables use, and for the same
// reason: a datapack changes what a recipe needs, not what "shaped" means.
//
// This header holds the parts every recipe kind shares: an index over the
// pack's arrays, the ingredient test, and the stack type the matching speaks.
// Grids live in crafting.hpp, furnaces in smelting.hpp.
//
// Nothing here knows about a server, a window or a packet. A recipe match is a
// pure function of a grid and the pack, which is what lets it be tested without
// starting anything and lets a client predict its own crafting one day.
#pragma once

#include "ov/registry/recipe_data.hpp"
#include "ov/registry/registries.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// One stack, as the recipe system sees it.
///
/// Deliberately not the protocol's ItemStack: ov_gameplay sits below
/// ov_protocol has no business knowing what a slot looks like on the wire, and
/// a recipe never needs an item's NBT in 1.20.1.
struct RecipeStack {
    registry::ProtocolId item{0};
    i32                  count{0};

    [[nodiscard]] bool empty() const noexcept { return count <= 0; }

    friend constexpr bool operator==(const RecipeStack&, const RecipeStack&) noexcept = default;
};

/// A recipe's index in the pack. Not a protocol value: the wire names recipes
/// by their identifier string.
using RecipeIndex = u32;

/// The compiled recipes, plus the indexes that keep matching off the hot path.
///
/// Holds no mutable state, so one instance serves every player and every
/// window. Built once from the pack; building it is a few thousand pushes.
class RecipeBook {
public:
    explicit RecipeBook(const registry::Registries& registries);

    [[nodiscard]] usize size() const noexcept { return data_.recipes.size(); }

    [[nodiscard]] const registry::RecipeRecord& recipe(RecipeIndex index) const noexcept {
        return data_.recipes[index];
    }

    [[nodiscard]] registry::RecipeKind kind(RecipeIndex index) const noexcept {
        return static_cast<registry::RecipeKind>(data_.recipes[index].kind);
    }

    [[nodiscard]] std::string_view name(RecipeIndex index) const noexcept {
        return registries_->recipe_name(index);
    }

    [[nodiscard]] std::string_view group(RecipeIndex index) const noexcept {
        return registries_->recipe_group(index);
    }

    [[nodiscard]] std::string_view type_name(RecipeIndex index) const noexcept {
        return registries_->recipe_type(index);
    }

    /// Find a recipe by its identifier, e.g. "minecraft:wooden_pickaxe".
    [[nodiscard]] std::optional<RecipeIndex> find(std::string_view identifier) const noexcept;

    /// The result of a recipe that has one of its own.
    ///
    /// Nullopt for `smithing_trim`, whose result is the armour it was given,
    /// and for the special recipes, which compute theirs. That is not the same
    /// as a result of air.
    [[nodiscard]] std::optional<RecipeStack> result(RecipeIndex index) const noexcept;

    /// Whether this recipe is only ever declared to clients, never matched.
    [[nodiscard]] bool declaration_only(RecipeIndex index) const noexcept {
        return (data_.recipes[index].flags & registry::kFlagDeclarationOnly) != 0;
    }

    /// Does this ingredient accept this item?
    ///
    /// A binary search over ids that were sorted at build time. An ingredient
    /// with no choices is a shaped pattern's blank, and accepts nothing — not
    /// anything, which is the reading that would let a stick fill a gap.
    [[nodiscard]] bool accepts(u32 ingredient, registry::ProtocolId item) const noexcept;

    /// The ingredients of a recipe, in the order the pack stores them: row by
    /// row for a shaped recipe, as listed for a shapeless one, and
    /// template-base-addition for smithing.
    [[nodiscard]] std::span<const registry::RecipeIngredientRecord> ingredients(
        RecipeIndex index) const noexcept;

    /// The items one ingredient accepts, sorted. Used to tell a client what a
    /// recipe needs; matching uses `accepts` instead.
    [[nodiscard]] std::span<const i32> choices(u32 ingredient) const noexcept;

    /// What is left in the slot after a recipe consumes one of this item.
    [[nodiscard]] std::optional<registry::ProtocolId> remainder(
        registry::ProtocolId item) const noexcept {
        return registries_->crafting_remainder(item);
    }

    /// Every recipe of one kind, as pack indexes. In pack order, which is
    /// recipe-identifier order, so iteration is reproducible.
    [[nodiscard]] std::span<const RecipeIndex> of_kind(registry::RecipeKind kind) const noexcept;

    /// Crafting recipes that use exactly this many filled cells.
    ///
    /// The pruning that makes matching cheap: a grid with four things in it
    /// cannot match a recipe that needs three. Everything else — the bounding
    /// box, the mirror — is decided per candidate.
    [[nodiscard]] std::span<const RecipeIndex> crafting_by_filled(usize filled) const noexcept;

    /// How many cells a shaped recipe actually fills, blanks excluded.
    [[nodiscard]] u8 filled_cells(RecipeIndex index) const noexcept {
        return filled_[index];
    }

    [[nodiscard]] const registry::Registries& registries() const noexcept { return *registries_; }

private:
    const registry::Registries* registries_{nullptr};
    registry::RecipeData        data_;

    /// One bucket per kind, then one per filled-cell count 0..9.
    std::array<std::vector<RecipeIndex>, 10> by_kind_{};
    std::array<std::vector<RecipeIndex>, 10> by_filled_{};
    std::vector<u8>                          filled_;
};

}  // namespace ov::gameplay
