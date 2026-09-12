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

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
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

/// What a furnace's input slot held when the pass last left it: item and
/// tags, not count.
///
/// Vanilla resets `CookTime` and reads `CookTimeTotal` again when its input
/// slot is set to something else. This is how the pass sees that without every
/// writer — click, hopper, command — having to say so: it compares the input
/// with what its own last tick left there.
///
/// One gap, named in docs/provenance/crafting-and-smelting.md: the same item
/// taken out and put back within one tick is two changes to vanilla and none
/// to a comparison made once a tick.
struct FurnaceInputMemory {
    bool                    known{false};
    bool                    empty{true};
    registry::ProtocolId    item{};
    std::optional<nbt::Tag> tag;
    i64                     seen_at{-1};

    [[nodiscard]] bool same_as(const gameplay::RecipeStack& input,
                               const nbt::Tag*              input_tag) const;
    /// Copies the tags only when they differ: no allocation for a furnace
    /// whose input stays what it is.
    void remember(const gameplay::RecipeStack& input, const nbt::Tag* input_tag);
};

/// What one furnace's tick did.
struct FurnaceEntityTick {
    gameplay::FurnaceTick step{};
    bool                  counters_changed{false};
    /// Someone else changed the input since the last tick.
    bool                  input_changed{false};
};

/// One tick of the furnace whose block entity is `data`, in place. `memory`
/// is this furnace's; first seen, nothing is compared and the NBT is kept as
/// it is — a missing `CookTimeTotal` included, as vanilla keeps it.
[[nodiscard]] FurnaceEntityTick tick_furnace_entity(const registry::Registries& registries,
                                                    registry::RegistryId        items,
                                                    const gameplay::RecipeBook& book,
                                                    gameplay::FurnaceKind kind, nbt::Tag& data,
                                                    FurnaceInputMemory& memory);

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
/// Found again every tick, not once a second: the input detector has to see a
/// furnace before anything else writes into it — one placed, set by a command
/// or loaded with its chunk — or a hopper's first insertion would be taken for
/// the state it was found in, and the furnace would never cook. The walk is
/// over each chunk's handful of block entities, and the pass runs before the
/// hoppers in the tick.
class FurnaceEntities {
public:
    FurnaceEntities(const registry::Registries& registries, const gameplay::RecipeBook& book);

    FurnaceStats tick(const FurnaceHost& host, i64 now);

    [[nodiscard]] usize indexed() const noexcept { return index_.size(); }

private:
    const registry::Registries*                     registries_;
    const gameplay::RecipeBook*                     book_;
    std::optional<registry::RegistryId>             items_;
    std::vector<BlockPos>                           index_;
    std::unordered_map<u64, FurnaceInputMemory>     memory_;
};

/// The same pass once per dimension — the overworld, the Nether and the End,
/// numbered as `DimensionId` numbers them — each over its own chunks.
///
/// The version before this ticked the overworld's furnaces alone: a furnace
/// in the Nether or the End kept its fuel and its ore and never cooked.
class DimensionFurnaces {
public:
    static constexpr usize kDimensions = 3;

    DimensionFurnaces(const registry::Registries& registries, const gameplay::RecipeBook& book);

    /// `hosts[d]` is that dimension's host, or null when the dimension is not
    /// loaded — then it is skipped.
    FurnaceStats tick(std::span<const FurnaceHost* const> hosts, i64 now);

    [[nodiscard]] const FurnaceEntities& pass(usize dimension) const { return passes_[dimension]; }

private:
    std::array<FurnaceEntities, kDimensions> passes_;
};

}  // namespace ov::server
