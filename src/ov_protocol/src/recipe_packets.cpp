#include "ov/protocol/recipe_packets.hpp"

#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {
namespace {

/// The kinds Update Recipes serialises differently.
///
/// Everything else — the thirteen `crafting_special_*` recipes and
/// `crafting_decorated_pot` — carries a category and nothing more. That is not
/// a gap: those recipes are Java classes in vanilla, with no data to send.
enum class WireKind : u8 {
    Shaped,
    Shapeless,
    Cooking,
    Stonecutting,
    SmithingTransform,
    SmithingTrim,
    SpecialWithCategory,
};

[[nodiscard]] WireKind kind_of(std::string_view type) {
    if (type == "minecraft:crafting_shaped") {
        return WireKind::Shaped;
    }
    if (type == "minecraft:crafting_shapeless") {
        return WireKind::Shapeless;
    }
    if (type == "minecraft:smelting" || type == "minecraft:blasting" ||
        type == "minecraft:smoking" || type == "minecraft:campfire_cooking") {
        return WireKind::Cooking;
    }
    if (type == "minecraft:stonecutting") {
        return WireKind::Stonecutting;
    }
    if (type == "minecraft:smithing_transform") {
        return WireKind::SmithingTransform;
    }
    if (type == "minecraft:smithing_trim") {
        return WireKind::SmithingTrim;
    }
    return WireKind::SpecialWithCategory;
}

void write_ingredient(io::ByteWriter& writer, const WireIngredient& ingredient) {
    write_varint(writer, static_cast<i32>(ingredient.items.size()));
    for (const i32 item : ingredient.items) {
        // Every choice as a stack of one. The client compares items, but the
        // field is a Slot and a bare id would leave it one byte short and
        // desynchronise the rest of a packet holding a thousand more recipes.
        write_slot(writer, ItemStack{item, 1, {}});
    }
}

}  // namespace

std::vector<u8> encode_container_property(u8 window_id, i16 property, i16 value) {
    io::ByteWriter writer;
    writer.write_u8(window_id);
    writer.write_u16(static_cast<u16>(property));
    writer.write_u16(static_cast<u16>(value));
    return std::move(writer).take();
}

std::vector<u8> encode_update_recipes(std::span<const WireRecipe> recipes) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(recipes.size()));

    for (const WireRecipe& recipe : recipes) {
        // Type first, then the identifier. The order looks backwards and is
        // not: the client reads the type to know which reader to use, and a
        // swapped pair makes every recipe after the first unreadable.
        write_string(writer, recipe.type);
        write_string(writer, recipe.identifier);

        switch (kind_of(recipe.type)) {
            case WireKind::Shaped:
                write_varint(writer, recipe.width);
                write_varint(writer, recipe.height);
                write_string(writer, recipe.group);
                write_varint(writer, recipe.category);
                for (const WireIngredient& ingredient : recipe.ingredients) {
                    write_ingredient(writer, ingredient);
                }
                write_slot(writer, recipe.result);
                writer.write_u8(recipe.show_notification ? 1 : 0);
                break;

            case WireKind::Shapeless:
                write_string(writer, recipe.group);
                write_varint(writer, recipe.category);
                write_varint(writer, static_cast<i32>(recipe.ingredients.size()));
                for (const WireIngredient& ingredient : recipe.ingredients) {
                    write_ingredient(writer, ingredient);
                }
                write_slot(writer, recipe.result);
                break;

            case WireKind::Cooking:
                write_string(writer, recipe.group);
                write_varint(writer, recipe.category);
                for (const WireIngredient& ingredient : recipe.ingredients) {
                    write_ingredient(writer, ingredient);
                }
                write_slot(writer, recipe.result);
                writer.write_f32(recipe.experience);
                write_varint(writer, recipe.cooking_time);
                break;

            case WireKind::Stonecutting:
                write_string(writer, recipe.group);
                for (const WireIngredient& ingredient : recipe.ingredients) {
                    write_ingredient(writer, ingredient);
                }
                write_slot(writer, recipe.result);
                break;

            case WireKind::SmithingTransform:
                for (const WireIngredient& ingredient : recipe.ingredients) {
                    write_ingredient(writer, ingredient);
                }
                write_slot(writer, recipe.result);
                break;

            case WireKind::SmithingTrim:
                // Three ingredients and no result: the result is the armour
                // piece the player put in, decorated.
                for (const WireIngredient& ingredient : recipe.ingredients) {
                    write_ingredient(writer, ingredient);
                }
                break;

            case WireKind::SpecialWithCategory:
                write_varint(writer, recipe.category);
                break;
        }
    }
    return std::move(writer).take();
}

std::vector<u8> encode_update_recipe_book(const RecipeBookState& state) {
    io::ByteWriter writer;
    write_varint(writer, state.action);
    for (const bool flag : {state.crafting_open, state.crafting_filter, state.furnace_open,
                            state.furnace_filter, state.blast_open, state.blast_filter,
                            state.smoker_open, state.smoker_filter}) {
        writer.write_u8(flag ? 1 : 0);
    }
    write_varint(writer, static_cast<i32>(state.recipes.size()));
    for (const std::string_view name : state.recipes) {
        write_string(writer, name);
    }
    if (state.action == 0) {
        write_varint(writer, static_cast<i32>(state.highlighted.size()));
        for (const std::string_view name : state.highlighted) {
            write_string(writer, name);
        }
    }
    return std::move(writer).take();
}

}  // namespace ov::net
