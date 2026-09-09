// The compiled recipes, as flat arrays.
//
// Recipes really are data in 1.20.1 — they live in the datapack and are
// regenerated locally — so what is written here is the *shape* they are
// flattened into, not the rules. The rules are ov_gameplay's; this header only
// says what the bytes mean, so that the module holding the file and the module
// doing the matching do not have to be the same one.
//
// Flattened at build time for the same reason tags are: an ingredient that
// reads `#minecraft:planks` is expanded here into a sorted list of item ids, so
// that matching a grid cell is a binary search rather than a tag-graph walk.
#pragma once

#include "ov/base/types.hpp"

#include <span>
#include <string_view>

namespace ov::registry {

/// The nine data-driven recipe kinds of 1.20.1, plus the special ones.
///
/// `Special` is not a tenth kind of data: it is the fourteen recipes whose files
/// carry a type and a category and nothing else, because
/// in vanilla they are Java classes. They are kept so the client can be told
/// they exist — its recipe book wants them — and marked `declaration_only` so
/// that matching skips them. Treating them as empty recipes would make a bare
/// grid produce a firework.
enum class RecipeKind : u8 {
    CraftingShaped,
    CraftingShapeless,
    Smelting,
    Blasting,
    Smoking,
    CampfireCooking,
    Stonecutting,
    SmithingTransform,
    SmithingTrim,
    Special,
};

/// Which tab of the client's recipe book a recipe lands in.
///
/// A protocol value, not ours: it travels as a VarInt in Update Recipes. The
/// crafting and cooking enumerations are different and share this type only
/// because they are both small; a cooking recipe's 0 is Food, a crafting
/// recipe's 0 is Building.
enum class RecipeCategory : u8 { Zero, One, Two, Three };

inline constexpr u32 kFlagShowNotification = 1U << 0U;
/// The recipe is declared to clients but never matched here. True for the
/// fourteen special recipes and for `smithing_trim`, whose result is the input
/// armour piece decorated rather than an item of its own.
inline constexpr u32 kFlagDeclarationOnly = 1U << 1U;

/// One recipe. Offsets are into the pack's string blob; ids are item ids.
struct RecipeRecord {
    u32 name_offset;
    u32 group_offset;
    /// The recipe type as it appears on the wire, e.g.
    /// "minecraft:crafting_special_armordye". Kept verbatim because Update
    /// Recipes sends the string, not our enum.
    u32 type_offset;
    /// -1 when the recipe has no result item of its own: `smithing_trim`
    /// decorates the piece it was given, and the special recipes compute
    /// theirs. Zero would mean `minecraft:air`, which is a different claim.
    i32 result_item;
    i32 result_count;
    u32 ingredient_first;
    u32 ingredient_count;
    f32 experience;
    u32 cook_time;
    u8  kind;
    /// Shaped recipes only, 1..3. Zero elsewhere.
    u8  width;
    u8  height;
    u8  category;
    u32 flags;
};

/// One ingredient: the span of item ids it accepts, sorted ascending.
///
/// `choice_count == 0` is a shaped pattern's empty cell — not "anything", which
/// is the reading that would let a stick satisfy a blank.
struct RecipeIngredientRecord {
    u32 choice_first;
    u32 choice_count;
};

/// How many furnace kinds the fuel table covers, in this order:
/// furnace, blast furnace, smoker.
///
/// Three tables rather than one and a divisor: the blast furnace's times are
/// not exactly half of the furnace's for every item — integer division makes
/// 4001 into 2000 — and a divisor would round the wrong way for the items where
/// it matters.
inline constexpr usize kFuelKinds = 3;

enum class FuelKind : u8 { Furnace, BlastFurnace, Smoker };

/// Every array at once, so a caller holds one thing rather than five.
struct RecipeData {
    std::span<const RecipeRecord>           recipes;
    std::span<const RecipeIngredientRecord> ingredients;
    std::span<const i32>                    choices;
    /// `kFuelKinds * item_count` entries: burn ticks, zero when not a fuel.
    std::span<const u16> fuel;
    /// One per item: the item a recipe leaves behind, or -1.
    std::span<const i32> remainder;

    [[nodiscard]] bool empty() const noexcept { return recipes.empty(); }
};

}  // namespace ov::registry
