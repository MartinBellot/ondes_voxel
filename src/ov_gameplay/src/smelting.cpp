#include "ov/gameplay/smelting.hpp"

#include <algorithm>

namespace ov::gameplay {
namespace {

[[nodiscard]] registry::FuelKind fuel_kind_of(FurnaceKind kind) noexcept {
    switch (kind) {
        case FurnaceKind::BlastFurnace:
            return registry::FuelKind::BlastFurnace;
        case FurnaceKind::Smoker:
            return registry::FuelKind::Smoker;
        case FurnaceKind::Furnace:
            break;
    }
    return registry::FuelKind::Furnace;
}

/// Can this output slot take that many more of this item?
[[nodiscard]] bool fits(const RecipeBook& book, const RecipeStack& output,
                        const RecipeStack& produced) {
    if (output.empty()) {
        return true;
    }
    if (output.item != produced.item) {
        return false;
    }
    const i32 limit = book.registries().max_stack_size(output.item);
    return output.count + produced.count <= limit;
}

}  // namespace

registry::RecipeKind cooking_kind(FurnaceKind kind) noexcept {
    switch (kind) {
        case FurnaceKind::BlastFurnace:
            return registry::RecipeKind::Blasting;
        case FurnaceKind::Smoker:
            return registry::RecipeKind::Smoking;
        case FurnaceKind::Furnace:
            break;
    }
    return registry::RecipeKind::Smelting;
}

i32 burn_ticks(const RecipeBook& book, FurnaceKind kind, registry::ProtocolId item) noexcept {
    return book.registries().burn_ticks(item, fuel_kind_of(kind));
}

std::optional<RecipeIndex> match_cooking(const RecipeBook& book, FurnaceKind kind,
                                         registry::ProtocolId input) {
    for (const RecipeIndex index : book.of_kind(cooking_kind(kind))) {
        const registry::RecipeRecord& record = book.recipe(index);
        if (record.ingredient_count != 1) {
            continue;
        }
        if (book.accepts(record.ingredient_first, input)) {
            return index;
        }
    }
    return std::nullopt;
}

std::vector<RecipeIndex> stonecutting_options(const RecipeBook&    book,
                                              registry::ProtocolId input) {
    std::vector<RecipeIndex> out;
    for (const RecipeIndex index : book.of_kind(registry::RecipeKind::Stonecutting)) {
        const registry::RecipeRecord& record = book.recipe(index);
        if (record.ingredient_count == 1 && book.accepts(record.ingredient_first, input)) {
            out.push_back(index);
        }
    }
    return out;
}

std::optional<RecipeIndex> match_smithing(const RecipeBook& book, registry::ProtocolId tmpl,
                                          registry::ProtocolId base,
                                          registry::ProtocolId addition) {
    for (const RecipeIndex index : book.of_kind(registry::RecipeKind::SmithingTransform)) {
        const registry::RecipeRecord& record = book.recipe(index);
        if (record.ingredient_count != 3) {
            continue;
        }
        // Template, base, addition — the order the pack stores them in, and
        // the order the smithing table numbers its slots.
        if (book.accepts(record.ingredient_first + 0, tmpl) &&
            book.accepts(record.ingredient_first + 1, base) &&
            book.accepts(record.ingredient_first + 2, addition)) {
            return index;
        }
    }
    return std::nullopt;
}

FurnaceTick furnace_tick(const RecipeBook& book, FurnaceKind kind, FurnaceSlots& slots,
                         FurnaceState& state) {
    FurnaceTick report;
    const bool  was_lit = state.lit();

    // 1. Burn down. An already-lit furnace loses a tick before anything else,
    //    which is what makes a fuel last exactly its measured count: lighting
    //    sets the counter and this tick does not touch it.
    if (state.lit_time > 0) {
        --state.lit_time;
    }

    // What could be cooked right now, if anything.
    std::optional<RecipeIndex> recipe;
    RecipeStack                produced{};
    if (!slots.input.empty()) {
        recipe = match_cooking(book, kind, slots.input.item);
        if (recipe) {
            const auto result = book.result(*recipe);
            if (result) {
                produced = *result;
            } else {
                recipe.reset();
            }
        }
    }
    const bool can_cook = recipe.has_value() && fits(book, slots.output, produced);

    // 2. Light, if there is fuel and something to burn it for. A furnace with
    //    fuel and nothing to cook stays dark — which is why a fuel's value
    //    cannot be read out of a furnace that has an empty input slot.
    if (!state.lit() && can_cook && !slots.fuel.empty()) {
        const i32 duration = burn_ticks(book, kind, slots.fuel.item);
        if (duration > 0) {
            state.lit_time     = duration;
            state.lit_duration = duration;

            // The bucket comes back where the lava bucket was; anything else
            // is simply used up. Measured on the game, like the burn time.
            const auto left = book.remainder(slots.fuel.item);
            --slots.fuel.count;
            if (slots.fuel.count <= 0) {
                slots.fuel = left ? RecipeStack{*left, 1} : RecipeStack{};
            }
            report.slots_changed = true;
        }
    }

    // 3. Cook. Progress runs only while lit; a furnace that goes out loses it
    //    again at twice the speed, which is what stops a player from cooking
    //    an item on a series of single sticks.
    if (state.lit() && can_cook) {
        // `CookTimeTotal` is not read again here. Vanilla sets it when the
        // input slot changes and after each item; a furnace whose total is 0
        // — placed by hand without one — counts up for ever and never
        // finishes (measured: `CookTime` 1202 with nothing produced).
        ++state.cook_time;
        if (state.cook_time == state.cook_total) {
            state.cook_time = 0;
            if (slots.output.empty()) {
                slots.output = produced;
            } else {
                slots.output.count += produced.count;
            }
            --slots.input.count;
            if (slots.input.count <= 0) {
                slots.input = RecipeStack{};
            }
            state.stored_experience += book.recipe(*recipe).experience;
            report.produced      = true;
            report.slots_changed = true;
            // The next item's total, read again after each one. An input used
            // up keeps the total it had (measured: 200 on a furnace that has
            // just finished its eighth and last ingot).
            if (!slots.input.empty()) {
                if (const auto next = match_cooking(book, kind, slots.input.item)) {
                    state.cook_total = static_cast<i32>(book.recipe(*next).cook_time);
                }
            }
        }
    } else if (state.cook_time > 0) {
        // Lost at twice the speed, and never above the total: the furnace
        // placed without one reads `CookTime` 0 once its fire is out.
        state.cook_time = std::clamp(state.cook_time - 2, 0, std::max(state.cook_total, 0));
    }

    // A furnace that cannot cook keeps its `CookTimeTotal`: measured on the
    // real server (`scripts/measure_furnaces.py ticks`), a blocked output, a
    // smoker given ore and a furnace whose input ran out all still read 200
    // or 100 thousands of ticks later. Zeroing it here wrote a different NBT.

    report.lit_changed = was_lit != state.lit();
    return report;
}

void furnace_input_changed(const RecipeBook& book, FurnaceKind kind, const FurnaceSlots& slots,
                           FurnaceState& state) {
    state.cook_time = 0;
    if (!slots.input.empty()) {
        if (const auto recipe = match_cooking(book, kind, slots.input.item)) {
            state.cook_total = static_cast<i32>(book.recipe(*recipe).cook_time);
            return;
        }
    }
    // Nothing this furnace cooks, or nothing at all: the total stays. Measured
    // on the real server: dirt or an empty slot in place of iron ore leaves
    // 200. Whether vanilla keeps it or falls back to 200 is not told apart by
    // that measurement — a blast furnace (100) given dirt was not measured.
}

}  // namespace ov::gameplay
