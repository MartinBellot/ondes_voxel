#include "furnace_entity.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::server {
namespace {

using gameplay::RecipeStack;

/// Set a short in place when the key exists, which it does after the first
/// write: `put` would move a fresh `Tag` in and, for a missing key, allocate
/// the entry.
void set_short(nbt::Tag& data, std::string_view key, i32 value) {
    const auto clamped = static_cast<i16>(std::clamp(value, -32768, 32767));
    if (nbt::Tag* existing = data.find(key); existing != nullptr) {
        if (auto* raw = existing->get_if<i16>(); raw != nullptr) {
            *raw = clamped;
            return;
        }
    }
    (void)data.put(std::string{key}, nbt::Tag{clamped});
}

[[nodiscard]] i32 read_number(const nbt::Tag& data, std::string_view key) noexcept {
    const nbt::Tag* found = data.find(key);
    return found != nullptr ? static_cast<i32>(found->as_i64()) : 0;
}

[[nodiscard]] RecipeStack read_item(const registry::Registries& registries,
                                    registry::RegistryId items, const nbt::Tag& entry) noexcept {
    const nbt::Tag* id    = entry.find("id");
    const nbt::Tag* count = entry.find("Count");
    if (id == nullptr || count == nullptr) {
        return {};
    }
    const auto item = registries.protocol_id(items, id->as_string());
    if (!item || count->as_i64() <= 0) {
        return {};
    }
    return RecipeStack{*item, static_cast<i32>(count->as_i64())};
}

[[nodiscard]] bool same(const RecipeStack& a, const RecipeStack& b) noexcept {
    return a.item == b.item && a.count == b.count;
}

}  // namespace

std::optional<gameplay::FurnaceKind> furnace_kind_of(std::string_view name) noexcept {
    if (name == "minecraft:furnace") {
        return gameplay::FurnaceKind::Furnace;
    }
    if (name == "minecraft:blast_furnace") {
        return gameplay::FurnaceKind::BlastFurnace;
    }
    if (name == "minecraft:smoker") {
        return gameplay::FurnaceKind::Smoker;
    }
    return std::nullopt;
}

void read_furnace_counters(const nbt::Tag& data, gameplay::FurnaceState& state) noexcept {
    state.lit_time   = read_number(data, "BurnTime");
    state.cook_time  = read_number(data, "CookTime");
    state.cook_total = read_number(data, "CookTimeTotal");
    // Vanilla does not store what the burn started at. A value that is still
    // consistent (the flame only shrinks between two fuels) is kept; anything
    // else means a new fuel was lit, or the furnace was just loaded.
    if (state.lit_time > state.lit_duration || state.lit_time <= 0) {
        state.lit_duration = state.lit_time;
    }
}

void write_furnace_counters(nbt::Tag& data, const gameplay::FurnaceState& state) {
    set_short(data, "BurnTime", state.lit_time);
    set_short(data, "CookTime", state.cook_time);
    set_short(data, "CookTimeTotal", state.cook_total);
    if (data.find("RecipesUsed") == nullptr) {
        (void)data.put("RecipesUsed", nbt::Tag::make_compound());
    }
}

void read_furnace_slots(const registry::Registries& registries, registry::RegistryId items,
                        const nbt::Tag& data, gameplay::FurnaceSlots& slots) noexcept {
    slots = {};
    // `Items` omits empty slots and is not indexed by position: the `Slot`
    // byte of each entry is the only thing that says where a stack goes.
    const nbt::Tag* list = data.find("Items");
    if (list == nullptr || list->list() == nullptr) {
        return;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* slot = entry.find("Slot");
        if (slot == nullptr) {
            continue;
        }
        switch (slot->as_i64()) {
            case 0:
                slots.input = read_item(registries, items, entry);
                break;
            case 1:
                slots.fuel = read_item(registries, items, entry);
                break;
            case 2:
                slots.output = read_item(registries, items, entry);
                break;
            default:
                break;
        }
    }
}

void write_furnace_slots(const registry::Registries& registries, registry::RegistryId items,
                         nbt::Tag& data, const gameplay::FurnaceSlots& slots) {
    // Anything an entry carried besides id and count (an enchanted book in
    // the fuel slot, say) is kept: the entry is edited, not rebuilt, when the
    // item in that slot is still the same one.
    nbt::Tag        rebuilt = nbt::Tag::make_list(nbt::TagType::Compound);
    const nbt::Tag* before  = data.find("Items");
    const std::array<const RecipeStack*, 3> order{&slots.input, &slots.fuel, &slots.output};
    for (usize index = 0; index < order.size(); ++index) {
        const RecipeStack& stack = *order[index];
        if (stack.empty()) {
            continue;  // empty slots are omitted, not stored as air
        }
        const std::string_view name = registries.entry_of(items, stack.item);
        if (name.empty()) {
            continue;
        }
        nbt::Tag entry = nbt::Tag::make_compound();
        if (before != nullptr && before->list() != nullptr) {
            for (const nbt::Tag& old : *before->list()) {
                const nbt::Tag* slot = old.find("Slot");
                const nbt::Tag* id   = old.find("id");
                if (slot != nullptr && id != nullptr && slot->as_i64() == static_cast<i64>(index) &&
                    id->as_string() == name) {
                    entry = old;
                }
            }
        }
        (void)entry.put("Slot", nbt::Tag{static_cast<i8>(index)});
        (void)entry.put("id", nbt::Tag{std::string{name}});
        (void)entry.put("Count", nbt::Tag{static_cast<i8>(std::min(stack.count, 127))});
        (void)rebuilt.push(std::move(entry));
    }
    (void)data.put("Items", std::move(rebuilt));
}

void count_recipe_used(nbt::Tag& data, std::string_view recipe) {
    nbt::Tag* used = data.find("RecipesUsed");
    if (used == nullptr || used->compound() == nullptr) {
        (void)data.put("RecipesUsed", nbt::Tag::make_compound());
        used = data.find("RecipesUsed");
    }
    if (nbt::Tag* count = used->find(recipe); count != nullptr) {
        if (auto* raw = count->get_if<i32>(); raw != nullptr) {
            ++*raw;
            return;
        }
        (void)used->put(std::string{recipe}, nbt::Tag{static_cast<i32>(count->as_i64() + 1)});
        return;
    }
    (void)used->put(std::string{recipe}, nbt::Tag{i32{1}});
}

i32 recipes_used(const nbt::Tag& data, std::string_view recipe) noexcept {
    const nbt::Tag* used = data.find("RecipesUsed");
    if (used == nullptr) {
        return 0;
    }
    const nbt::Tag* count = used->find(recipe);
    return count != nullptr ? static_cast<i32>(count->as_i64()) : 0;
}

std::vector<i32> take_recipes_used_experience(nbt::Tag& data, const gameplay::RecipeBook& book,
                                              math::LegacyRandomSource& random) {
    std::vector<i32> out;
    nbt::Tag*        used = data.find("RecipesUsed");
    if (used == nullptr || used->compound() == nullptr) {
        return out;
    }
    for (const nbt::CompoundEntry& entry : *used->compound()) {
        const auto recipe = book.find(entry.name);
        if (!recipe) {
            continue;
        }
        const auto  count  = static_cast<f32>(entry.value.as_i64());
        const f32   amount = count * book.recipe(*recipe).experience;
        auto        whole  = static_cast<i32>(std::floor(amount));
        const f32   frac   = amount - static_cast<f32>(whole);
        if (frac != 0.0F && random.next_float() < frac) {
            ++whole;
        }
        if (whole > 0) {
            out.push_back(whole);
        }
    }
    used->compound()->clear();
    return out;
}

FurnaceEntityTick tick_furnace_entity(const registry::Registries& registries,
                                      registry::RegistryId items, const gameplay::RecipeBook& book,
                                      gameplay::FurnaceKind kind, nbt::Tag& data) {
    FurnaceEntityTick     out;
    gameplay::FurnaceSlots slots;
    gameplay::FurnaceState state;
    read_furnace_slots(registries, items, data, slots);
    read_furnace_counters(data, state);

    // The common case by far: cold, with nothing cooking and no progress to
    // lose. A world full of decorative furnaces costs a read and a compare.
    if (!state.lit() && state.cook_time == 0 && slots.input.empty()) {
        return out;
    }

    const gameplay::FurnaceSlots before_slots = slots;
    const gameplay::FurnaceState before       = state;
    // Which recipe is cooking, before the tick may use the last input up.
    const std::optional<gameplay::RecipeIndex> recipe =
        slots.input.empty() ? std::nullopt : gameplay::match_cooking(book, kind, slots.input.item);

    out.step = gameplay::furnace_tick(book, kind, slots, state);
    out.counters_changed = state.lit_time != before.lit_time ||
                           state.cook_time != before.cook_time ||
                           state.cook_total != before.cook_total;
    if (out.counters_changed) {
        write_furnace_counters(data, state);
    }
    if (out.step.slots_changed || !same(slots.input, before_slots.input) ||
        !same(slots.fuel, before_slots.fuel) || !same(slots.output, before_slots.output)) {
        write_furnace_slots(registries, items, data, slots);
    }
    if (out.step.produced && recipe) {
        count_recipe_used(data, book.name(*recipe));
    }
    return out;
}

FurnaceEntities::FurnaceEntities(const registry::Registries& registries,
                                 const gameplay::RecipeBook& book)
    : registries_(&registries), book_(&book), items_(registries.find("minecraft:item")) {
    index_.reserve(256);
}

void FurnaceEntities::note(BlockPos pos) {
    if (std::ranges::find(index_, pos) == index_.end()) {
        index_.push_back(pos);
    }
}

std::optional<registry::BlockStateId> relight_furnace_block(world::Chunk&                  chunk,
                                                            const registry::BlockRegistry& blocks,
                                                            BlockPos at, bool lit) {
    const auto                   local_x = static_cast<usize>(at.x & 15);
    const auto                   local_z = static_cast<usize>(at.z & 15);
    const registry::BlockStateId state   = chunk.get_block(local_x, at.y, local_z);
    // `lit` is a property: the same block with one value changed, keeping the
    // facing it was placed with.
    const auto property = blocks.find_property(blocks.block_of(state), "lit");
    if (!property) {
        return std::nullopt;
    }
    const std::string_view wanted = lit ? "true" : "false";
    for (u16 index = 0; index < property->values.size(); ++index) {
        if (property->values[index] != wanted) {
            continue;
        }
        const registry::BlockStateId next = blocks.with_property(state, *property, index);
        if (next == state) {
            return std::nullopt;
        }
        // Copied, not referenced: `set_block` erases the entry it points at.
        std::optional<world::BlockEntity> kept;
        if (const world::BlockEntity* existing = chunk.block_entity_at(local_x, at.y, local_z);
            existing != nullptr) {
            kept = *existing;
        }
        chunk.set_block(local_x, at.y, local_z, next);
        if (kept) {
            chunk.set_block_entity(std::move(*kept));
        }
        return next;
    }
    return std::nullopt;
}

FurnaceStats FurnaceEntities::tick(const FurnaceHost& host, i64 now) {
    FurnaceStats stats;
    if (!items_) {
        return stats;
    }
    if (indexed_at_ < 0 || now < indexed_at_ || now - indexed_at_ >= 20) {
        indexed_at_ = now;
        index_.clear();
        if (host.for_each_chunk) {
            host.for_each_chunk([this](ChunkPos pos, const world::Chunk& chunk) {
                for (const world::BlockEntity& entity : chunk.block_entities()) {
                    if (furnace_kind_of(entity.type)) {
                        // Local to the chunk in storage, world coordinates here.
                        index_.push_back(BlockPos{pos.x * 16 + static_cast<i32>(entity.x),
                                                  entity.y,
                                                  pos.z * 16 + static_cast<i32>(entity.z)});
                    }
                }
            });
        }
    }

    for (const BlockPos& at : index_) {
        world::Chunk* chunk = host.chunk(at.x >> 4, at.z >> 4);
        if (chunk == nullptr) {
            continue;
        }
        world::BlockEntity* entity = chunk->block_entity_at(static_cast<usize>(at.x & 15), at.y,
                                                            static_cast<usize>(at.z & 15));
        if (entity == nullptr) {
            continue;  // gone under the index, which is up to a second old
        }
        const auto kind = furnace_kind_of(entity->type);
        if (!kind) {
            continue;
        }
        ++stats.furnaces;
        const FurnaceEntityTick one =
            tick_furnace_entity(*registries_, *items_, *book_, *kind, entity->data);
        if (read_number(entity->data, "BurnTime") > 0) {
            ++stats.burning;
        }
        if (one.step.produced) {
            ++stats.cooked;
        }
        if (one.counters_changed || one.step.slots_changed) {
            host.mark_dirty(at.x >> 4, at.z >> 4);
        }
        if (one.step.lit_changed && host.set_lit) {
            // Last: flipping `lit` rewrites the block, and `entity` is not to
            // be trusted after that.
            host.set_lit(*chunk, at, read_number(entity->data, "BurnTime") > 0);
        }
    }
    return stats;
}

}  // namespace ov::server
