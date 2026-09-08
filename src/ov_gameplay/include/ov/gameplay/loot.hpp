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
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <optional>
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

}  // namespace ov::gameplay
