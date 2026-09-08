// The compiled block loot tables, as flat arrays.
//
// Loot tables really are data in vanilla — they live in the datapack and are
// regenerated locally — so what is written here is the *shape* they are
// flattened into, not the rules. The rules are ov_gameplay's; this header only
// says what the bytes mean, so that the module holding the file and the module
// holding the behaviour do not have to be the same one.
//
// Flattened at build time for the same reason tags are: walking a tree of
// conditions on every block broken would put a graph traversal on the player's
// path.
#pragma once

#include "ov/base/types.hpp"

#include <span>

namespace ov::registry {

/// Condition kinds, shared with tools/ov_datagen/loot.py.
enum class LootCondition : u8 {
    /// `survives_explosion`, which is true whenever nothing exploded.
    Always,
    SilkTouch,
    /// The held item is one of `count` ids starting at `aux` in the int pool.
    ToolIs,
    /// The state's property named `aux` equals the value named `aux2`.
    StateProperty,
    /// Fortune's chance table: `count` floats starting at `aux`.
    TableBonus,
    /// Negates the single condition at `aux`.
    Inverted,
    /// Any of `count` conditions starting at `aux`.
    AnyOf,
    RandomChance,
    /// An entity broke the block, which a player always does.
    BrokenByEntity,
    /// The block one step up or down is a named block in a named state.
    /// `aux` points at three interned strings — block, property, value — and
    /// `aux2` holds the y offset as an i32 bit pattern. Only plus and minus one
    /// occur, and the emitter refuses anything else.
    Neighbour,
    /// Something this version does not evaluate. Reads as false, never as true:
    /// a condition we do not understand must not open a drop.
    Unsupported,
};

/// Function kinds, shared with the emitter.
enum class LootFunction : u8 {
    CountConstant,
    CountUniform,
    CountBinomial,
    /// A no-op outside explosions.
    ExplosionDecay,
    OreDrops,
    UniformBonus,
    BinomialBonus,
    LimitCount,
    /// copy_name, copy_nbt, copy_state, set_contents — the ones that need block
    /// entity data. They leave the stack alone rather than dropping it.
    Unsupported,
};

enum class LootEntryKind : u8 { Item, Alternatives, Unsupported };

/// One block's table. `present` distinguishes a block with no table at all —
/// air, technical blocks — from one whose table exists and is empty, which is
/// what bedrock has.
struct LootTableRecord {
    u32 present;
    u32 pool_first;
    u32 pool_count;
    u32 function_first;
    u32 function_count;
};

struct LootPoolRecord {
    f32 rolls;
    u32 entry_first;
    u32 entry_count;
    u32 condition_first;
    u32 condition_count;
    u32 function_first;
    u32 function_count;
};

struct LootEntryRecord {
    u32 kind;
    u32 item;
    u32 condition_first;
    u32 condition_count;
    u32 function_first;
    u32 function_count;
    u32 child_first;
    u32 child_count;
};

struct LootConditionRecord {
    u32 kind;
    u32 count;
    f32 value;
    u32 aux;
    u32 aux2;
};

struct LootFunctionRecord {
    u32 kind;
    u32 count;
    f32 a;
    f32 b;
    u32 aux;
    u32 condition_first;
    u32 condition_count;
};

/// Every array at once, so a caller holds one thing rather than seven.
struct LootData {
    std::span<const LootTableRecord>     tables;
    std::span<const LootPoolRecord>      pools;
    std::span<const LootEntryRecord>     entries;
    std::span<const LootConditionRecord> conditions;
    std::span<const LootFunctionRecord>  functions;
    std::span<const f32>                 floats;
    std::span<const u32>                 ints;

    [[nodiscard]] bool empty() const noexcept { return tables.empty(); }
};

}  // namespace ov::registry
