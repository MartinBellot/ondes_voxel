// The three furnaces as ticked block entities.
//
// A furnace cooks whether anybody is looking or not. The block entity's NBT is
// the **only** copy of a furnace — `Items`, `BurnTime`, `CookTime`,
// `CookTimeTotal`, `RecipesUsed`, vanilla's names and types — so that a hopper
// feeding it through `block_container`, a player with its screen open and a
// save written mid-smelt all see the same furnace.
//
// The version before this one kept two copies: the open screen's, ticked by
// the screen, and the block entity's, ticked by a headless pass that wrote the
// counters back only when a slot changed. Every tick then reloaded a stale
// `BurnTime` and `CookTime`, and a furnace nobody looked at burnt for ever and
// never finished an item. This file is the single pass that replaces both.
//
// Experience is vanilla's `RecipesUsed`: a count per recipe, turned into points
// when a player takes from the output slot (or the block is broken) — not a
// running float total, which neither vanilla nor its saves know.
//
// Measured against the real 1.20.1 server by `scripts/measure_furnaces.py`;
// docs/provenance/crafting-and-smelting.md.
#pragma once

#include "ov/gameplay/smelting.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace ov::server {

/// The furnace a block entity (or block) name is, if any.
[[nodiscard]] std::optional<gameplay::FurnaceKind> furnace_kind_of(std::string_view name) noexcept;

/// `BurnTime`, `CookTime`, `CookTimeTotal`. `lit_duration` is not stored by
/// vanilla; it is left as it was, and set to `lit_time` only when that is
/// larger (a reload mid-burn draws a full flame, as vanilla's does).
void read_furnace_counters(const nbt::Tag& data, gameplay::FurnaceState& state) noexcept;

/// Written in place, as shorts: no allocation once the keys exist, which they
/// do after the first write.
void write_furnace_counters(nbt::Tag& data, const gameplay::FurnaceState& state);

/// The three slots out of `Items` (by `Slot`, empty ones absent).
void read_furnace_slots(const registry::Registries& registries, registry::RegistryId items,
                        const nbt::Tag& data, gameplay::FurnaceSlots& slots) noexcept;

/// `Items`, rebuilt. Allocates: called only when a slot changed.
void write_furnace_slots(const registry::Registries& registries, registry::RegistryId items,
                         nbt::Tag& data, const gameplay::FurnaceSlots& slots);

/// One more of this recipe in `RecipesUsed` (an int per recipe id).
void count_recipe_used(nbt::Tag& data, std::string_view recipe);

/// How many of this recipe `RecipesUsed` holds.
[[nodiscard]] i32 recipes_used(const nbt::Tag& data, std::string_view recipe) noexcept;

/// Turn `RecipesUsed` into experience and empty it.
///
/// Per recipe, as vanilla: `count × experience` in single precision, the
/// integer part always, one more with probability equal to the fraction.
/// Returns one amount per recipe — each is one `ExperienceOrb.award`, which the
/// caller splits into orbs. Recipes this server does not know are dropped.
[[nodiscard]] std::vector<i32> take_recipes_used_experience(nbt::Tag&                   data,
                                                            const gameplay::RecipeBook& book,
                                                            math::LegacyRandomSource&   random);

/// What one furnace's tick did.
struct FurnaceEntityTick {
    gameplay::FurnaceTick step{};
    bool                  counters_changed{false};
};

/// One tick of the furnace whose block entity is `data`, in place.
[[nodiscard]] FurnaceEntityTick tick_furnace_entity(const registry::Registries& registries,
                                                    registry::RegistryId        items,
                                                    const gameplay::RecipeBook& book,
                                                    gameplay::FurnaceKind kind, nbt::Tag& data);

/// Flip a furnace block's `lit` property, **keeping its block entity**.
///
/// `Chunk::set_block` drops the block entity at the position it writes — right
/// for a new block, fatal for a property change: the version before this one
/// lit a furnace through it while its screen was open, and the furnace lost
/// its ore, its fuel and its experience on the tick it caught. Returns the new
/// state, or nullopt when the block has no `lit` or already had that value.
[[nodiscard]] std::optional<registry::BlockStateId> relight_furnace_block(
    world::Chunk& chunk, const registry::BlockRegistry& blocks, BlockPos at, bool lit);

/// What the pass reaches outside itself for. Built once, not per tick: a
/// `std::function` holding a lambda with many captures allocates.
struct FurnaceHost {
    /// The resident chunk at chunk coordinates, or nullptr.
    std::function<world::Chunk*(i32, i32)> chunk;
    /// Every resident chunk, for the once-a-second index.
    std::function<void(const std::function<void(ChunkPos, const world::Chunk&)>&)> for_each_chunk;
    /// Flip the block's `lit` property **keeping its block entity**
    /// (`relight_furnace_block`), and tell everyone.
    std::function<void(world::Chunk&, BlockPos, bool)> set_lit;
    std::function<void(i32, i32)> mark_dirty;
};

struct FurnaceStats {
    usize furnaces{0};
    usize burning{0};
    usize cooked{0};
};

/// Every furnace in the loaded chunks, every tick.
///
/// The index is rebuilt once a second rather than every block entity of every
/// chunk being walked twenty times a second. A furnace opened, clicked or fed
/// is `note`d, so that one placed a moment ago does not wait for the index.
class FurnaceEntities {
public:
    FurnaceEntities(const registry::Registries& registries, const gameplay::RecipeBook& book);

    /// A furnace is here; tick it from now on even before the next index.
    void note(BlockPos pos);

    FurnaceStats tick(const FurnaceHost& host, i64 now);

    [[nodiscard]] usize indexed() const noexcept { return index_.size(); }

private:
    const registry::Registries*         registries_;
    const gameplay::RecipeBook*         book_;
    std::optional<registry::RegistryId> items_;
    std::vector<BlockPos>               index_;
    i64                                 indexed_at_{-1};
};

}  // namespace ov::server
