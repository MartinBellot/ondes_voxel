// Block states, and the arithmetic that makes them cheap.
//
// 1.20.1 has 1003 blocks and 24135 block states, numbered 0 to 24134 with no
// gaps. Those numbers are Mojang's: the vanilla client hard-codes them and they
// are never sent over the wire, so ours have to be identical or a connected
// client sees the wrong blocks. See docs/ARCHITECTURE.md § 3.3.
//
// The property layout is what makes this fast. A block's states are contiguous,
// and within that range they are a mixed-radix number over the block's
// properties, most significant first. So changing one property is
//
//     state + (new_index - old_index) * stride
//
// — one multiply and one add, no hashing, no allocation. Redstone, doors,
// fences, stairs and fluids all do this constantly; with a hash lookup instead,
// redstone is unplayable.
//
// The trap, and the reason the data is derived rather than read: the property
// order printed in blocks.json is NOT the order that arithmetic uses. Four
// blocks disagree. tools/ov_datagen recovers the real order from the state ids
// themselves and every one of the 24135 states is checked against it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/loot_data.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::registry {

/// A block state id, as it appears in a chunk palette and on the wire.
///
/// u16 is deliberate: 24135 states fit with room, and a chunk section stores
/// 4096 of them. Id 0 is minecraft:air, so a zeroed section is an empty one.
struct BlockStateTag {};

using BlockStateId = Id<BlockStateTag, u16>;

/// An index into the block registry, not a state.
struct BlockTag {};

using BlockId = Id<BlockTag, u16>;

inline constexpr BlockStateId kAirState{0};

enum class RegistryError {
    FileNotFound,
    /// Not an .ovpack, or truncated.
    Corrupt,
    /// Built by a different version of the emitter.
    VersionMismatch,
};

[[nodiscard]] std::string_view to_string(RegistryError error) noexcept;

/// One property of a block: its name, its possible values, and the stride that
/// moving between them costs in state ids.
struct PropertyView {
    std::string_view name;
    /// Values in declaration order; the index into this is the digit.
    std::span<const std::string_view> values;
    /// How much one step in this property moves the state id.
    u16 stride{0};
};

/// The block registry, loaded from the binary cache.
///
/// Reads a file produced by tools/ov_datagen, which derives it from the
/// official server jar's own reports. Nothing here is hand-maintained: a list
/// of 24135 ids kept by hand would be wrong within a version.
/// What the client needs to paint a biome.
///
/// Grass and foliage are usually *not* here: vanilla derives them from the
/// climate against a colormap texture in the resource pack, and that belongs on
/// the side that has the texture. `grass_override` and `foliage_override` carry
/// the exceptions — badlands, cherry grove, swamp — and are -1 otherwise.
struct BiomeEffects {
    /// f64 because the colormap index truncates, and the boundary matters. See
    /// the comment on BiomeRecord.
    f64 temperature{0.8};
    f64 downfall{0.4};
    u32 water_colour{0x3F76E4};
    u32 water_fog_colour{0x050533};
    u32 fog_colour{0xC0D8FF};
    u32 sky_colour{0x78A7FF};
    i32 grass_override{-1};
    i32 foliage_override{-1};
    /// 0 none, 1 dark_forest, 2 swamp.
    u8 grass_modifier{0};
    u8 temperature_modifier{0};
    bool has_precipitation{true};
};

class BlockRegistry {
public:
    [[nodiscard]] static std::expected<BlockRegistry, RegistryError> load(
        const std::filesystem::path& path);

    /// Load from bytes already in memory, for tests.
    [[nodiscard]] static std::expected<BlockRegistry, RegistryError> from_bytes(
        std::vector<u8> data);

    [[nodiscard]] usize block_count() const noexcept;
    [[nodiscard]] usize state_count() const noexcept;

    /// Look up a block by its resource location, e.g. "minecraft:oak_door".
    [[nodiscard]] std::optional<BlockId> find_block(std::string_view name) const noexcept;

    [[nodiscard]] std::string_view block_name(BlockId block) const noexcept;

    /// The state a freshly placed block takes.
    ///
    /// Not always the block's first state: 484 of the 1003 differ, so this is
    /// stored rather than computed.
    [[nodiscard]] BlockStateId default_state(BlockId block) const noexcept;

    /// The state range a block occupies. Contiguous, which is what the
    /// arithmetic depends on.
    [[nodiscard]] BlockStateId first_state(BlockId block) const noexcept;
    [[nodiscard]] u16          state_count(BlockId block) const noexcept;

    /// Which block a state belongs to. O(1): a reverse table is stored.
    [[nodiscard]] BlockId block_of(BlockStateId state) const noexcept;

    [[nodiscard]] std::span<const PropertyView> properties(BlockId block) const noexcept;

    /// The value a state has for one of its block's properties, as an index
    /// into that property's values.
    [[nodiscard]] u16 property_index(BlockStateId        state,
                                     const PropertyView& property) const noexcept;

    /// The value as text, e.g. "north" or "true".
    [[nodiscard]] std::string_view property_value(BlockStateId        state,
                                                  const PropertyView& property) const noexcept;

    /// The state that differs from this one only in the given property.
    ///
    /// One multiply and one add. Returns the input unchanged when the index is
    /// out of range, since a caller asking for a value a property does not have
    /// is a bug rather than a runtime condition.
    [[nodiscard]] BlockStateId with_property(BlockStateId state, const PropertyView& property,
                                             u16 value_index) const noexcept;

    /// Look up a property of a block by name.
    [[nodiscard]] std::optional<PropertyView> find_property(BlockId          block,
                                                            std::string_view name) const noexcept;

    /// The state matching a block and a set of property values, for parsing
    /// blockstate files and Anvil palettes.
    [[nodiscard]] std::optional<BlockStateId> state_for(
        BlockId                                                        block,
        std::span<const std::pair<std::string_view, std::string_view>> properties) const noexcept;

    /// How a block treats sky light passing through it.
    ///
    /// Not in Mojang's reports — in vanilla it is Java code — so this comes
    /// from measuring the game itself: a bedrock plate pierced with one-block
    /// shafts, the block under test at the top, the light read at the bottom.
    /// See docs/PROVENANCE.md.
    enum class LightOpacity : u8 {
        /// Sky light passes unchanged: air, signs, torches, glass, fences.
        Transparent,
        /// Sky light passes weakened: water, leaves, ice.
        Attenuating,
        /// Sky light stops.
        Opaque,
    };

    /// Measured per **block**, not per state. Whether any block's opacity
    /// varies with its state has not been measured, so a stair and a slab
    /// report the same value as their block.
    [[nodiscard]] LightOpacity light_opacity(BlockId block) const noexcept;

    /// Convenience: does this block stop sky light entirely?
    [[nodiscard]] bool blocks_sky_light(BlockId block) const noexcept {
        return light_opacity(block) == LightOpacity::Opaque;
    }

    // ── What the heightmaps ask ─────────────────────────────────────────────
    //
    // MOTION_BLOCKING and OCEAN_FLOOR are not derivable from anything in
    // Mojang's reports either. These come from the same kind of measurement:
    // put each block on top of a column on a real 1.20.1 server, save, and read
    // back which of its own heightmaps the game decided that column's top
    // belongs to. 996 of the 1003 blocks answered.
    //
    // The seven that did not are the body segments of vertical plants — kelp,
    // weeping and cave vines, a dripleaf stem — which always have something
    // above them and so can never *be* a column's top. The oracle cannot see
    // them, and a heightmap never needs to.

    /// Does this block stop movement? OCEAN_FLOOR is the highest one that does.
    ///
    /// Per **block**, and that is not a simplification: ten pairs of
    /// contrasting states were measured — snow at one layer and at eight, a
    /// trapdoor open and shut, a slab and a double slab — and every pair
    /// agreed. Snow does not stop movement at any depth, which no reasoning
    /// from its collision shape would have predicted.
    [[nodiscard]] bool blocks_motion(BlockId block) const noexcept;

    /// Is this one of the ten blocks MOTION_BLOCKING_NO_LEAVES skips?
    ///
    /// The measurement found exactly the ten members of the `minecraft:leaves`
    /// tag, arrived at independently.
    [[nodiscard]] bool is_leaves(BlockId block) const noexcept;

    /// Is this block air? WORLD_SURFACE is the highest block that is not.
    ///
    /// True for all three of air, cave_air and void_air — measured, since
    /// nothing said the last two counted.
    [[nodiscard]] bool is_air(BlockId block) const noexcept;

    /// Whether the block was measured at all. False leaves the three answers
    /// above meaningless rather than merely false.
    [[nodiscard]] bool motion_measured(BlockId block) const noexcept;

    /// How long this block takes to break, in vanilla's own units, or -1 for
    /// a block that never breaks.
    ///
    /// Not in Mojang's reports either. The value comes from
    /// PrismarineJS/minecraft-data and was then checked block by block against
    /// a real 1.20.1 server, by timing how long the server itself takes —
    /// see docs/PROVENANCE.md.
    [[nodiscard]] f32 hardness(BlockId block) const noexcept;

    /// Does breaking this block need the right kind of tool to drop anything?
    ///
    /// It also makes it five times slower: vanilla divides by 100 instead of
    /// 30. Measured, and the surprise is that the tool's **family** counts and
    /// not only its tier — a netherite shovel on stone takes the full 150
    /// ticks a bare hand does, where a wooden pickaxe takes 23.
    [[nodiscard]] bool requires_correct_tool(BlockId block) const noexcept;

    /// The text an interned offset points at, or nothing for an offset outside
    /// the table. Loot records name properties this way rather than carrying
    /// the characters, so the same name costs four bytes wherever it appears.
    [[nodiscard]] std::string_view string_at(u32 offset) const noexcept;

    /// One box of a collision shape, in units of a thirty-second of a block.
    ///
    /// Signed, because a box can leave the cube: an extended piston head runs
    /// from -8 to 48. Integers rather than floats, because every coordinate the
    /// game uses lands on this grid and comparing them has to be exact.
    struct Box {
        i8 min_x, min_y, min_z;
        i8 max_x, max_y, max_z;
    };

    /// The faces of a block, in the order a placement packet numbers them.
    enum class Face : u8 { Down, Up, North, South, West, East };

    /// The boxes a state collides with. Empty for anything you can walk
    /// through.
    [[nodiscard]] std::span<const Box> collision_boxes(BlockStateId state) const noexcept;

    /// Does this state present a full square on that face?
    ///
    /// The predicate fences, panes and walls attach to. Derived from the shape
    /// at build time rather than at every placement — it is a 32 by 32 grid per
    /// face — and checked against 23358 faces measured on a real 1.20.1 server:
    /// the connection rule rebuilt from these shapes reproduces every one.
    [[nodiscard]] bool face_is_sturdy(BlockStateId state, Face face) const noexcept;

    /// How much light this state gives off, 0 to 15.
    ///
    /// Per state and not per block: a redstone ore lit by a footstep, a candle
    /// by how many are in the cluster, a respawn anchor by its charge. Nothing
    /// in Mojang's reports carries it — it is Java code — so it was read out of
    /// the light the game itself wrote into a windowless room. See
    /// docs/PROVENANCE.md.
    [[nodiscard]] u8 light_emission(BlockStateId state) const noexcept;

    // ── Biomes ──────────────────────────────────────────────────────────────
    //
    // A dynamic registry: the client is told about biomes rather than
    // hardcoding them, so unlike blocks their ids are ours and the lookup is by
    // name. What is stored is exactly what vanilla sends.

    [[nodiscard]] usize biome_count() const noexcept { return biome_count_; }

    /// Index of a biome by resource location, e.g. "minecraft:swamp".
    /// The section is sorted by name, so this is a binary search.
    [[nodiscard]] std::optional<u32> find_biome(std::string_view name) const noexcept;

    [[nodiscard]] std::string_view biome_name(u32 index) const noexcept;

    /// The effects of one biome. Out-of-range gives the plains-like defaults
    /// rather than nothing: a missing biome should look ordinary, not black.
    [[nodiscard]] BiomeEffects biome(u32 index) const noexcept;

    /// The compiled loot tables, indexed by block.
    ///
    /// Handed out as plain arrays rather than as an evaluator: the rules for
    /// reading them are gameplay, and gameplay lives above this module.
    [[nodiscard]] LootData loot() const noexcept { return loot_; }

    /// Does this **state** hold a fluid? MOTION_BLOCKING is the highest block
    /// that either stops movement or does.
    ///
    /// Per state, unlike the rest: `waterlogged` is a property, and
    /// scaffolding[waterlogged=true] raises MOTION_BLOCKING where
    /// scaffolding[waterlogged=false] does not. Both were measured.
    [[nodiscard]] bool holds_fluid(BlockStateId state) const noexcept;

    [[nodiscard]] bool is_valid(BlockStateId state) const noexcept {
        return state.value() < state_count();
    }

private:
    BlockRegistry() = default;

    struct Impl;
    std::vector<u8> data_;

    // Views into data_, built once at load. Rebuilding them per query would
    // undo the point of the format.
    std::vector<std::string_view> strings_;
    std::vector<PropertyView>     properties_;
    std::vector<std::string_view> values_;
    std::span<const u8>           block_flags_;
    std::span<const u8>           fluid_bits_;
    std::span<const f32>          hardness_;
    LootData                      loot_;
    std::span<const Box>          boxes_;
    std::span<const u32>          shape_records_;
    std::span<const u16>          state_shapes_;
    std::span<const u8>           emission_;
    /// BiomeRecord*, type-erased: the record's layout is private to this
    /// module, and naming it here would put the file format in a public
    /// header. Same reason as header_ below.
    const void*      biomes_{nullptr};
    u32              biome_count_{0};
    std::string_view strings_blob_;
    const void*                   header_{nullptr};
};

}  // namespace ov::registry
