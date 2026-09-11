// Campfires: raw food on the grill, and what falls off it cooked.
//
// A campfire is a block entity with four slots. A right-click with something a
// `minecraft:campfire_cooking` recipe accepts puts one of it in the first free
// slot, with that recipe's cooking time; every tick a lit campfire advances
// every occupied slot, and a slot that reaches its time drops the recipe's
// result on the ground and empties. An unlit one lets its progress fall back by
// two a tick. The rules are `gameplay::tick_campfire`; this file is the
// wiring — the block entity's NBT, the recipe lookup, the packets.
//
// The NBT is vanilla's: `Items` (Slot, id, Count), `CookingTimes` and
// `CookingTotalTimes` as int arrays of four, so a campfire this server writes
// is one vanilla reads, and back.
//
// Every recipe in 1.20.1's datapack cooks for 600 ticks; the time is read from
// the recipe, never assumed. The `entity` campaign times a real campfire fed
// by a player's click against it — see docs/provenance/feu.md.
#pragma once

#include "ov/gameplay/fire.hpp"
#include "ov/gameplay/recipe.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::server {

/// What the campfire pass reaches outside itself for. Called with both the
/// player and the chunk locks held.
struct CampfireHost {
    /// Every resident chunk, once.
    std::function<void(const std::function<void(world::Chunk&)>&)> each_chunk;
    /// Put a stack on the ground at a point.
    std::function<void(Vec3d, const net::ItemStack&)> drop_item;
    /// Tell every client a block entity changed (Block Entity Data).
    std::function<void(BlockPos, const world::BlockEntity&)> send_entity;
    /// The chunk must be saved.
    std::function<void(i32 cx, i32 cz)> mark_dirty;
};

struct CampfireStats {
    usize campfires{0};
    usize cooked{0};
};

class Campfires {
public:
    Campfires(const registry::BlockRegistry& blocks, const registry::Registries& registries,
              const gameplay::RecipeBook& recipes);

    /// Is this state a campfire (either kind)?
    [[nodiscard]] bool is_campfire(registry::BlockStateId state) const noexcept;

    /// The cooking time of the campfire recipe that accepts `item`, or nothing.
    [[nodiscard]] std::optional<i32> cook_time(registry::ProtocolId item) const;

    /// A player right-clicked a campfire at `pos` with `item`. True when one
    /// was put on the grill — the caller takes it from the hand (not in
    /// creative) and the clients have been told. False when the item does not
    /// cook here or all four slots are taken.
    bool place_food(world::Chunk& chunk, BlockPos pos, registry::ProtocolId item,
                    const CampfireHost& host);

    /// One tick of every campfire in the resident chunks.
    CampfireStats tick(const CampfireHost& host);

private:
    struct Slots {
        std::array<gameplay::CampfireSlot, 4> slot{};
    };

    [[nodiscard]] Slots read(const world::BlockEntity& entity) const;
    void                write(world::BlockEntity& entity, const Slots& slots) const;
    [[nodiscard]] bool  lit(registry::BlockStateId state) const;

    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    const gameplay::RecipeBook*    recipes_;
    std::optional<registry::RegistryId> item_registry_;
    i32                            entity_type_{-1};
    registry::BlockId              campfire_{0xFFFF};
    registry::BlockId              soul_campfire_{0xFFFF};
    std::vector<BlockPos>          scratch_;
};

}  // namespace ov::server
