// What a block drops when it is broken.
//
// The tables themselves are data — vanilla's own, regenerated locally and
// flattened into the pack — so what lives here is only the interpreter. That
// split matters: a block's drops change with a datapack, its *rules* do not.
//
// The vocabulary the 925 block tables actually use is small and closed: four
// `match_tool` predicates, three shapes of `set_count`, three `apply_bonus`
// formulas. Anything outside it is refused at build time rather than guessed
// at here, and the few functions that need block entity data — a named chest,
// a filled shulker box — leave the stack alone instead of dropping it.
#pragma once

#include "ov/gameplay/breaking.hpp"
#include "ov/gameplay/recipe.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// One stack a block gave up.
struct Drop {
    registry::ProtocolId item{0};
    i32                  count{0};
};

/// The blocks a table is allowed to ask about.
///
/// Only two, and only these: vanilla's block tables never look further than one
/// step up or down, and both uses are the same one — a two-block plant checking
/// that its other half is still there. Breaking the lower half of tall grass
/// gives seeds; breaking it after the top is already gone gives nothing.
struct Neighbours {
    std::optional<registry::BlockStateId> above;
    std::optional<registry::BlockStateId> below;
};

/// Reads the compiled tables. Holds no state of its own beyond the registry,
/// so one instance serves every player.
class LootTables {
public:
    LootTables(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// Does this block have a table at all?
    ///
    /// Bedrock does, and it is empty; air does not. The two are different
    /// answers to "why did nothing drop".
    [[nodiscard]] bool has_table(registry::BlockStateId state) const noexcept;

    /// What breaking this state with this tool yields.
    ///
    /// Appends rather than clears: a caller breaking several blocks in a tick
    /// keeps one vector and one allocation.
    void drops(registry::BlockStateId state, const Held& held, math::XoroshiroRandomSource& random,
               std::vector<Drop>& out, const Neighbours& around = {}) const;

private:
    struct Context;

    [[nodiscard]] bool test(u32 first, u32 count, const Context& context) const;
    [[nodiscard]] bool test_one(const registry::LootConditionRecord& condition,
                                const Context&                       context) const;
    [[nodiscard]] i32  apply(u32 first, u32 count, i32 amount, const Context& context) const;
    [[nodiscard]] bool expand(u32 entry_index, const Context& context, u32& chosen) const;

    const registry::BlockRegistry* blocks_{nullptr};
    registry::LootData             data_;
};

// ── What a mob drops ────────────────────────────────────────────────────────
//
// Entity tables are the same *format* as block tables and a different
// vocabulary. A block's table asks about the tool, Silk Touch and Fortune; a
// mob's asks who killed it, whether it was on fire, how much Looting the sword
// had — and, for a magma cube, which variety of frog ate it. Sharing the
// interpreter would mean one enum with eighteen cases of which each table uses
// half, so the two stay apart and only the shape is shared.
//
// The compiled tables come from tools/ov_datagen/entity_loot.py and live in
// their own file, `data/vanilla/1.20.1/entity_loot.ovpack`, rather than inside
// registry.ovpack: the registry pack is shared by every branch in flight and
// adding a section to it would oblige all of them to regenerate at once.

/// Everything a kill knows about itself.
///
/// Deliberately not an entity. A caller fills this in from whatever it has —
/// the server from its entity world, a test from literals — which is what keeps
/// the rules testable without a world.
struct KillContext {
    /// The registry name of the thing that died, e.g. "minecraft:zombie".
    /// It is what selects the table, and it is also read by the table: a sheep
    /// looks up "minecraft:sheep/white" and a slime looks at its own size.
    std::string_view entity_type;

    /// Killed by a player, directly or by something a player owns. Seventeen
    /// tables gate a drop on this and every one of them drops nothing without
    /// it — which is why a mob pushed into lava leaves no gunpowder.
    bool killed_by_player{false};

    /// Looting level on the weapon that landed the killing blow.
    u8 looting{0};

    /// The victim was burning: nineteen tables smelt their own drop.
    bool on_fire{false};

    /// A slime's or magma cube's size. Zero for anything that is neither, and
    /// the tables that ask are the only ones that read it.
    i32 slime_size{0};

    /// The registry name of the killer, for the one table that asks whether it
    /// was a skeleton. Empty for none.
    std::string_view killer_type;

    /// The registry name of the entity behind the damage, and — for the magma
    /// cube — that entity's frog variant. Empty for none.
    std::string_view source_entity_type;
    std::string_view source_frog_variant;

    /// The damage was lightning, which is the only reason a turtle drops a
    /// shell fragment... and in fact the reason a turtle does not.
    bool source_is_lightning{false};
};

/// What one draw left out, and why.
///
/// Counted rather than silent. Two entity tables in 1.20.1 delegate to
/// `minecraft:gameplay/fishing/fish`, which is not an entity table and is not
/// in this file; a draw that reaches one says so here instead of dropping
/// nothing and looking correct.
struct DrawResult {
    u32 referenced_tables{0};
    u32 unsupported_entries{0};
    u32 unsupported_functions{0};
    u32 unsupported_conditions{0};

    [[nodiscard]] bool complete() const noexcept {
        return referenced_tables == 0 && unsupported_entries == 0 &&
               unsupported_functions == 0 && unsupported_conditions == 0;
    }
};

/// Reads the compiled entity tables.
class EntityLootTables {
public:
    /// Load from the bytes of an entity_loot.ovpack.
    ///
    /// `recipes` is used by `furnace_smelt` — a burning cow drops steak — and
    /// may be null, in which case a smelt is reported as an unsupported
    /// function rather than quietly leaving the raw item.
    [[nodiscard]] static std::expected<EntityLootTables, registry::RegistryError> from_bytes(
        std::vector<u8> data, const registry::Registries& registries,
        const RecipeBook* recipes);

    [[nodiscard]] usize table_count() const noexcept;

    /// Does this entity type have a table at all?
    [[nodiscard]] bool has_table(std::string_view entity_type) const noexcept;

    /// Draw once. Appends rather than clears, like the block tables.
    DrawResult drops(const KillContext& context, math::XoroshiroRandomSource& random,
                     std::vector<Drop>& out) const;

private:
    struct Context;

    void                             run_table(u32 table_index, Context& context,
                                               std::vector<Drop>& out, i32 depth) const;
    [[nodiscard]] std::optional<u32> find_table(std::string_view name) const noexcept;
    [[nodiscard]] bool              test(u32 first, u32 count, Context& context) const;
    [[nodiscard]] bool              test_one(u32 index, Context& context) const;
    void                            apply(u32 first, u32 count, Drop& drop, Context& context) const;
    [[nodiscard]] std::string_view  string_at(u32 offset) const noexcept;

    struct TableRecord {
        u32 name;
        u32 pool_first;
        u32 pool_count;
    };
    struct PoolRecord {
        f32 rolls_min;
        f32 rolls_max;
        u32 entry_first;
        u32 entry_count;
        u32 condition_first;
        u32 condition_count;
        u32 function_first;
        u32 function_count;
    };
    struct EntryRecord {
        u32 kind;
        u32 item;
        u32 weight;
        u32 aux;
        u32 condition_first;
        u32 condition_count;
        u32 function_first;
        u32 function_count;
        u32 child_first;
        u32 child_count;
    };
    struct ConditionRecord {
        u32 kind;
        u32 count;
        f32 value;
        f32 value2;
        u32 aux;
        u32 aux2;
    };
    struct FunctionRecord {
        u32 kind;
        f32 a;
        f32 b;
        u32 aux;
        u32 condition_first;
        u32 condition_count;
    };

    std::vector<TableRecord>     tables_;
    std::vector<PoolRecord>      pools_;
    std::vector<EntryRecord>     entries_;
    std::vector<ConditionRecord> conditions_;
    std::vector<FunctionRecord>  functions_;
    std::vector<u8>              strings_;

    const registry::Registries* registries_{nullptr};
    const RecipeBook*           recipes_{nullptr};
};

}  // namespace ov::gameplay
