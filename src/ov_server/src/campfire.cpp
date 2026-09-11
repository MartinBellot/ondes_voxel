#define OV_LOG_CATEGORY "fire"

#include "campfire.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace ov::server {

Campfires::Campfires(const registry::BlockRegistry& blocks,
                     const registry::Registries& registries, const gameplay::RecipeBook& recipes)
    : blocks_{&blocks}, registries_{&registries}, recipes_{&recipes} {
    item_registry_ = registries.find("minecraft:item");
    if (const auto types = registries.find("minecraft:block_entity_type")) {
        // One block entity type serves both campfires, as in vanilla.
        if (const auto id = registries.protocol_id(*types, "minecraft:campfire")) {
            entity_type_ = *id;
        }
    }
    if (const auto id = blocks.find_block("minecraft:campfire")) {
        campfire_ = *id;
    }
    if (const auto id = blocks.find_block("minecraft:soul_campfire")) {
        soul_campfire_ = *id;
    }
    if (entity_type_ < 0) {
        OV_LOG_WARN("no campfire block entity type in the registry: campfires will not cook");
    }
    scratch_.reserve(16);
}

bool Campfires::is_campfire(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    return block == campfire_ || block == soul_campfire_;
}

bool Campfires::lit(registry::BlockStateId state) const {
    const registry::BlockId block    = blocks_->block_of(state);
    const auto              property = blocks_->find_property(block, "lit");
    return property && blocks_->property_value(state, *property) == "true";
}

std::optional<i32> Campfires::cook_time(registry::ProtocolId item) const {
    for (const gameplay::RecipeIndex index :
         recipes_->of_kind(registry::RecipeKind::CampfireCooking)) {
        const registry::RecipeRecord& record = recipes_->recipe(index);
        if (record.ingredient_count == 1 && recipes_->accepts(record.ingredient_first, item)) {
            return static_cast<i32>(record.cook_time);
        }
    }
    return std::nullopt;
}

Campfires::Slots Campfires::read(const world::BlockEntity& entity) const {
    Slots out;
    if (const nbt::Tag* items = entity.data.find("Items"); items != nullptr && items->list()) {
        for (const nbt::Tag& entry : *items->list()) {
            const i64 slot = entry.find("Slot") != nullptr ? entry.find("Slot")->as_i64(-1) : -1;
            if (slot < 0 || slot >= 4 || !item_registry_) {
                continue;
            }
            const std::string_view name =
                entry.find("id") != nullptr ? entry.find("id")->as_string() : std::string_view{};
            if (const auto id = registries_->protocol_id(*item_registry_, name)) {
                out.slot[static_cast<usize>(slot)].item = *id;
            }
        }
    }
    const auto ints = [&](std::string_view key, auto member) {
        if (const nbt::Tag* tag = entity.data.find(key)) {
            if (const auto* values = tag->get_if<nbt::Tag::IntArray>()) {
                for (usize i = 0; i < std::min<usize>(4, values->size()); ++i) {
                    out.slot[i].*member = (*values)[i];
                }
            }
        }
    };
    ints("CookingTimes", &gameplay::CampfireSlot::progress);
    ints("CookingTotalTimes", &gameplay::CampfireSlot::total);
    return out;
}

void Campfires::write(world::BlockEntity& entity, const Slots& slots) const {
    if (entity.data.type() != nbt::TagType::Compound) {
        entity.data = nbt::Tag::make_compound();
    }
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    nbt::Tag::IntArray times(4, 0);
    nbt::Tag::IntArray totals(4, 0);
    for (usize i = 0; i < 4; ++i) {
        const gameplay::CampfireSlot& slot = slots.slot[i];
        times[i]  = slot.progress;
        totals[i] = slot.total;
        if (slot.item < 0 || !item_registry_) {
            continue;
        }
        nbt::Tag entry = nbt::Tag::make_compound();
        entry.put("Slot", nbt::Tag{static_cast<i8>(i)});
        entry.put("id", nbt::Tag{std::string{registries_->entry_of(*item_registry_, slot.item)}});
        entry.put("Count", nbt::Tag{static_cast<i8>(1)});
        (void)items.push(std::move(entry));
    }
    entity.data.put("Items", std::move(items));
    entity.data.put("CookingTimes", nbt::Tag{std::move(times)});
    entity.data.put("CookingTotalTimes", nbt::Tag{std::move(totals)});
}

bool Campfires::place_food(world::Chunk& chunk, BlockPos pos, registry::ProtocolId item,
                           const CampfireHost& host) {
    if (entity_type_ < 0) {
        return false;
    }
    const auto x = static_cast<usize>(pos.x & 15);
    const auto z = static_cast<usize>(pos.z & 15);
    if (!is_campfire(chunk.get_block(x, pos.y, z))) {
        return false;
    }
    const auto time = cook_time(item);
    if (!time) {
        return false;
    }
    world::BlockEntity* entity = chunk.block_entity_at(x, pos.y, z);
    if (entity == nullptr) {
        // A campfire placed by a player carries no block entity until it is
        // given something to cook.
        world::BlockEntity fresh;
        fresh.x       = static_cast<u8>(x);
        fresh.y       = pos.y;
        fresh.z       = static_cast<u8>(z);
        fresh.type    = "minecraft:campfire";
        fresh.type_id = entity_type_;
        fresh.data    = nbt::Tag::make_compound();
        chunk.set_block_entity(std::move(fresh));
        entity = chunk.block_entity_at(x, pos.y, z);
        if (entity == nullptr) {
            return false;
        }
    }
    Slots slots = read(*entity);
    for (gameplay::CampfireSlot& slot : slots.slot) {
        if (slot.item >= 0) {
            continue;
        }
        slot = gameplay::CampfireSlot{.item = item, .progress = 0, .total = *time};
        write(*entity, slots);
        if (host.send_entity) {
            host.send_entity(pos, *entity);
        }
        if (host.mark_dirty) {
            host.mark_dirty(pos.x >> 4, pos.z >> 4);
        }
        return true;
    }
    return false;
}

CampfireStats Campfires::tick(const CampfireHost& host) {
    CampfireStats stats;
    if (entity_type_ < 0 || !host.each_chunk) {
        return stats;
    }
    host.each_chunk([&](world::Chunk& chunk) {
        const ChunkPos where = chunk.position();
        scratch_.clear();
        for (const world::BlockEntity& entity : chunk.block_entities()) {
            if (entity.type == "minecraft:campfire") {
                scratch_.push_back(BlockPos{where.x * 16 + entity.x, entity.y, where.z * 16 + entity.z});
            }
        }
        for (const BlockPos pos : scratch_) {
            const auto x = static_cast<usize>(pos.x & 15);
            const auto z = static_cast<usize>(pos.z & 15);
            const registry::BlockStateId state = chunk.get_block(x, pos.y, z);
            world::BlockEntity*          entity = chunk.block_entity_at(x, pos.y, z);
            if (entity == nullptr || !is_campfire(state)) {
                continue;
            }
            ++stats.campfires;
            Slots      slots    = read(*entity);
            const bool burning  = lit(state);
            bool       occupied = false;
            for (const gameplay::CampfireSlot& slot : slots.slot) {
                occupied = occupied || slot.item >= 0;
            }
            if (!occupied) {
                continue;
            }
            const u8 finished = gameplay::tick_campfire(slots.slot, burning);
            bool     changed  = false;
            for (usize i = 0; i < 4; ++i) {
                if ((finished & (1U << i)) == 0U) {
                    continue;
                }
                gameplay::CampfireSlot& slot = slots.slot[i];
                // The recipe's result, looked up again from the item: the slot
                // stores what went in, as vanilla's does.
                for (const gameplay::RecipeIndex index :
                     recipes_->of_kind(registry::RecipeKind::CampfireCooking)) {
                    const registry::RecipeRecord& record = recipes_->recipe(index);
                    if (record.ingredient_count != 1 ||
                        !recipes_->accepts(record.ingredient_first, slot.item)) {
                        continue;
                    }
                    if (const auto result = recipes_->result(index); result && host.drop_item) {
                        host.drop_item(Vec3d{static_cast<f64>(pos.x) + 0.5,
                                             static_cast<f64>(pos.y) + 0.5,
                                             static_cast<f64>(pos.z) + 0.5},
                                       net::ItemStack{result->item,
                                                      static_cast<i8>(std::max(1, result->count)),
                                                      {}});
                    }
                    break;
                }
                slot    = gameplay::CampfireSlot{};
                changed = true;
                ++stats.cooked;
            }
            write(*entity, slots);
            if (host.mark_dirty) {
                host.mark_dirty(where.x, where.z);
            }
            if (changed && host.send_entity) {
                host.send_entity(pos, *entity);
            }
        }
    });
    return stats;
}

}  // namespace ov::server
