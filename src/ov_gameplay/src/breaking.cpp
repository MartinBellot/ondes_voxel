#include "ov/gameplay/breaking.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace ov::gameplay {
namespace {

/// Speed each tier digs at, for the blocks its family covers.
///
/// Measured, one tier at a time, on stone: with a hardness of 1.5 the tick
/// count is `ceil(45 / speed)`, and for each tier only one integer produces the
/// number counted. Netherite needed obsidian to separate 9 from 10 and 11.
constexpr std::array<f32, 6> kTierSpeed{2.0F, 4.0F, 12.0F, 6.0F, 8.0F, 9.0F};

/// Harvest level, in the order the `needs_*_tool` tags demand.
constexpr std::array<u8, 6> kTierLevel{0, 1, 0, 2, 3, 4};

/// The six tiers, in the order the two tables above use.
constexpr std::array<std::string_view, 6> kTierPrefix{"wooden_", "stone_",   "golden_",
                                                      "iron_",   "diamond_", "netherite_"};

/// Index of a tier from an item name, or nothing when the item is not a tool.
[[nodiscard]] std::optional<usize> tier_index(std::string_view name) {
    const auto             colon = name.find(':');
    const std::string_view bare  = colon == std::string_view::npos ? name : name.substr(colon + 1);
    for (usize i = 0; i < kTierPrefix.size(); ++i) {
        if (bare.starts_with(kTierPrefix[i])) {
            return i;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::string_view, 5> kMineableTags{
    "minecraft:mineable/pickaxe", "minecraft:mineable/axe", "minecraft:mineable/shovel",
    "minecraft:mineable/hoe", "minecraft:mineable/hoe"};

constexpr std::array<std::string_view, 3> kNeedsTags{
    "minecraft:needs_stone_tool", "minecraft:needs_iron_tool", "minecraft:needs_diamond_tool"};

constexpr std::array<std::string_view, 7> kFamilyTags{"",
                                                      "minecraft:pickaxes",
                                                      "minecraft:axes",
                                                      "minecraft:shovels",
                                                      "minecraft:hoes",
                                                      "minecraft:swords",
                                                      ""};

}  // namespace

BreakRules::BreakRules(const registry::BlockRegistry& blocks,
                       const registry::Registries&    registries)
    : blocks_{&blocks}, registries_{&registries} {
    const auto block_registry = registries.find("minecraft:block");
    const auto item_registry  = registries.find("minecraft:item");
    if (block_registry) {
        block_ids_.reserve(blocks.block_count());
        for (usize i = 0; i < blocks.block_count(); ++i) {
            const auto id = registries.protocol_id(
                *block_registry, blocks.block_name(registry::BlockId{static_cast<u16>(i)}));
            block_ids_.push_back(id ? *id : -1);
        }
        for (usize i = 0; i < kMineableTags.size(); ++i) {
            mineable_[i] = registries.find_tag(*block_registry, kMineableTags[i]);
        }
        for (usize i = 0; i < kNeedsTags.size(); ++i) {
            needs_[i] = registries.find_tag(*block_registry, kNeedsTags[i]);
        }
    }
    if (item_registry) {
        for (usize i = 0; i < kFamilyTags.size(); ++i) {
            if (!kFamilyTags[i].empty()) {
                family_tag_[i] = registries.find_tag(*item_registry, kFamilyTags[i]);
            }
        }
    }
}

registry::ProtocolId BreakRules::wire_id(registry::BlockId block) const noexcept {
    return block.value() < block_ids_.size() ? block_ids_[block.value()] : -1;
}

Family BreakRules::family_of(registry::ProtocolId item) const noexcept {
    for (usize i = 1; i < kFamilyTags.size(); ++i) {
        if (family_tag_[i] && registries_->tag_contains(*family_tag_[i], item)) {
            return static_cast<Family>(i);
        }
    }
    const auto item_registry = registries_->find("minecraft:item");
    if (item_registry && registries_->entry_of(*item_registry, item) == "minecraft:shears") {
        return Family::Shears;
    }
    return Family::None;
}

Tier BreakRules::tier_of(registry::ProtocolId item) const noexcept {
    const auto item_registry = registries_->find("minecraft:item");
    if (!item_registry) {
        return Tier::Wood;
    }
    const auto index = tier_index(registries_->entry_of(*item_registry, item));
    return index ? static_cast<Tier>(kTierLevel[*index]) : Tier::Wood;
}

namespace {

/// Which mineable tag a family reads, or nothing for one that has none.
[[nodiscard]] std::optional<usize> mineable_slot(Family family) noexcept {
    switch (family) {
        case Family::Pickaxe: return 0;
        case Family::Axe: return 1;
        case Family::Shovel: return 2;
        case Family::Hoe: return 3;
        default: return std::nullopt;
    }
}

}  // namespace

bool BreakRules::can_harvest(registry::BlockStateId state, const Held& held) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    if (!blocks_->requires_correct_tool(block)) {
        return true;
    }
    if (!held.item) {
        return false;
    }
    const registry::ProtocolId block_id = wire_id(block);

    // Tier first: a wooden pickaxe on iron ore was measured at 150 ticks, the
    // bare-hand number, not the 23 the same pickaxe takes on stone.
    const u8 level = static_cast<u8>(tier_of(*held.item));
    for (usize i = 0; i < kNeedsTags.size(); ++i) {
        if (needs_[i] && registries_->tag_contains(*needs_[i], block_id) && level < i + 1) {
            return false;
        }
    }

    // Then the family. This is the part that is not guessable: a netherite
    // shovel clears every tier there is and still cannot harvest stone.
    const auto slot = mineable_slot(family_of(*held.item));
    if (!slot || !mineable_[*slot]) {
        return false;
    }
    return registries_->tag_contains(*mineable_[*slot], block_id);
}

f32 BreakRules::destroy_speed(registry::BlockStateId state, const Held& held,
                              const Stance& stance) const noexcept {
    f32 speed = 1.0F;

    if (held.item) {
        const auto                 slot     = mineable_slot(family_of(*held.item));
        const registry::ProtocolId block_id = wire_id(blocks_->block_of(state));
        if (slot && mineable_[*slot] && registries_->tag_contains(*mineable_[*slot], block_id)) {
            const auto item_registry = registries_->find("minecraft:item");
            const auto index = item_registry
                                   ? tier_index(registries_->entry_of(*item_registry, *held.item))
                                   : std::nullopt;
            if (index) {
                speed = kTierSpeed[*index];
            }
        }
    }

    // Efficiency only helps a tool that was already helping. On a bare hand, or
    // on a shovel held against stone, it does nothing at all.
    if (speed > 1.0F && held.efficiency > 0) {
        speed += static_cast<f32>(held.efficiency) * static_cast<f32>(held.efficiency) + 1.0F;
    }

    if (stance.haste >= 0) {
        speed *= 1.0F + static_cast<f32>(stance.haste + 1) * 0.2F;
    }
    if (stance.mining_fatigue >= 0) {
        switch (stance.mining_fatigue) {
            case 0: speed *= 0.3F; break;
            case 1: speed *= 0.09F; break;
            case 2: speed *= 0.0027F; break;
            default: speed *= 0.00081F; break;
        }
    }
    if (stance.head_in_water && !stance.aqua_affinity) {
        speed /= 5.0F;
    }
    if (!stance.on_ground) {
        speed /= 5.0F;
    }
    return std::max(speed, 0.0F);
}

f32 BreakRules::destroy_progress(registry::BlockStateId state, const Held& held,
                                 const Stance& stance) const noexcept {
    const f32 hardness = blocks_->hardness(blocks_->block_of(state));
    if (hardness < 0.0F) {
        return 0.0F;
    }
    const f32 speed = destroy_speed(state, held, stance);
    if (speed <= 0.0F) {
        return 0.0F;
    }
    // Thirty when the block will drop, a hundred when it will not. The second
    // number is why breaking stone with a shovel feels broken and is not.
    const f32 divisor = can_harvest(state, held) ? 30.0F : 100.0F;
    return speed / hardness / divisor;
}

i32 BreakRules::break_ticks(registry::BlockStateId state, const Held& held,
                            const Stance& stance) const noexcept {
    const f32 progress = destroy_progress(state, held, stance);
    if (progress <= 0.0F) {
        return -1;
    }
    return static_cast<i32>(std::ceil(1.0F / progress));
}

}  // namespace ov::gameplay
