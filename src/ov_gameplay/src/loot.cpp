#include "ov/gameplay/loot.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {

/// Everything an evaluation needs, gathered once so the recursion carries one
/// reference instead of six arguments.
struct LootTables::Context {
    registry::BlockStateId       state;
    const Held*                  held;
    math::XoroshiroRandomSource* random;
    const Neighbours*            around;
    u8                           fortune;
    bool                         silk_touch;
};

LootTables::LootTables(const registry::BlockRegistry& blocks, const registry::Registries&)
    : blocks_{&blocks}, data_{blocks.loot()} {}

bool LootTables::has_table(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    return block.value() < data_.tables.size() && data_.tables[block.value()].present != 0;
}

bool LootTables::test_one(const registry::LootConditionRecord& condition,
                          const Context&                       context) const {
    switch (static_cast<registry::LootCondition>(condition.kind)) {
        case registry::LootCondition::Always:
        case registry::LootCondition::BrokenByEntity: return true;

        case registry::LootCondition::SilkTouch: return context.silk_touch;

        case registry::LootCondition::ToolIs: {
            if (!context.held->item) {
                return false;
            }
            for (u32 i = 0; i < condition.count; ++i) {
                const u32 index = condition.aux + i;
                if (index < data_.ints.size() &&
                    static_cast<registry::ProtocolId>(data_.ints[index]) == *context.held->item) {
                    return true;
                }
            }
            return false;
        }

        case registry::LootCondition::StateProperty: {
            const registry::BlockId block = blocks_->block_of(context.state);
            const auto property = blocks_->find_property(block, blocks_->string_at(condition.aux));
            if (!property) {
                return false;
            }
            return blocks_->property_value(context.state, *property) ==
                   blocks_->string_at(condition.aux2);
        }

        case registry::LootCondition::TableBonus: {
            // Fortune's chance table, indexed by level and clamped at its end:
            // level 5 on a three-entry table takes the last chance, not none.
            if (condition.count == 0) {
                return false;
            }
            const u32 level = std::min<u32>(context.fortune, static_cast<u32>(condition.count) - 1);
            const u32 index = condition.aux + level;
            return index < data_.floats.size() &&
                   context.random->next_float() < data_.floats[index];
        }

        case registry::LootCondition::Inverted:
            return condition.aux < data_.conditions.size() &&
                   !test_one(data_.conditions[condition.aux], context);

        case registry::LootCondition::AnyOf:
            for (u32 i = 0; i < condition.count; ++i) {
                const u32 index = condition.aux + i;
                if (index < data_.conditions.size() && test_one(data_.conditions[index], context)) {
                    return true;
                }
            }
            return false;

        case registry::LootCondition::RandomChance:
            return context.random->next_float() < condition.value;

        case registry::LootCondition::Neighbour: {
            if (condition.count != 3 || condition.aux + 2 >= data_.ints.size()) {
                return false;
            }
            const auto                                  offset = static_cast<i32>(condition.aux2);
            const std::optional<registry::BlockStateId> neighbour =
                offset > 0 ? context.around->above : context.around->below;
            if (!neighbour) {
                // The caller did not say what is there, so we cannot claim it
                // matches. A plant whose other half is unknown drops nothing,
                // which is the same answer as a plant whose other half is gone.
                return false;
            }
            const registry::BlockId block = blocks_->block_of(*neighbour);
            if (blocks_->block_name(block) != blocks_->string_at(data_.ints[condition.aux])) {
                return false;
            }
            const auto property =
                blocks_->find_property(block, blocks_->string_at(data_.ints[condition.aux + 1]));
            return property && blocks_->property_value(*neighbour, *property) ==
                                   blocks_->string_at(data_.ints[condition.aux + 2]);
        }

        case registry::LootCondition::Unsupported:
        default:
            // False, never true. A condition we do not understand must not open
            // a drop that the real game keeps shut.
            return false;
    }
}

bool LootTables::test(u32 first, u32 count, const Context& context) const {
    for (u32 i = 0; i < count; ++i) {
        const u32 index = first + i;
        if (index >= data_.conditions.size() || !test_one(data_.conditions[index], context)) {
            return false;
        }
    }
    return true;
}

namespace {

/// `n` draws at probability `p`, the way vanilla counts them.
[[nodiscard]] i32 binomial(math::XoroshiroRandomSource& random, i32 trials, f32 probability) {
    i32 total = 0;
    for (i32 i = 0; i < trials; ++i) {
        if (random.next_float() < probability) {
            ++total;
        }
    }
    return total;
}

}  // namespace

i32 LootTables::apply(u32 first, u32 count, i32 amount, const Context& context) const {
    for (u32 i = 0; i < count; ++i) {
        const u32 index = first + i;
        if (index >= data_.functions.size()) {
            break;
        }
        const registry::LootFunctionRecord& function = data_.functions[index];
        if (!test(function.condition_first, function.condition_count, context)) {
            continue;
        }
        const i32 fortune = static_cast<i32>(context.fortune);

        switch (static_cast<registry::LootFunction>(function.kind)) {
            case registry::LootFunction::CountConstant:
                amount = static_cast<i32>(function.a);
                break;
            case registry::LootFunction::CountUniform: {
                const auto low  = static_cast<i32>(function.a);
                const auto high = static_cast<i32>(function.b);
                amount          = low + context.random->next_int(high - low + 1);
                break;
            }
            case registry::LootFunction::CountBinomial:
                amount = binomial(*context.random, static_cast<i32>(function.a), function.b);
                break;

            case registry::LootFunction::OreDrops: {
                // Fortune on ores. The draw is `nextInt(level + 2) - 1` floored
                // at zero, and the multiplier is that **plus one** — so at
                // level 3 it is 1, 1, 2, 3, 4 with equal chances, a mean of
                // 2.2. Flooring the draw at one instead gives 1.6, which looks
                // reasonable and is a third short; the real server said 2.25
                // over twelve rolls.
                if (fortune > 0) {
                    const i32 bonus = context.random->next_int(fortune + 2) - 1;
                    amount *= std::max(0, bonus) + 1;
                }
                break;
            }
            case registry::LootFunction::UniformBonus:
                amount += context.random->next_int(static_cast<i32>(function.a) * fortune + 1);
                break;
            case registry::LootFunction::BinomialBonus:
                amount +=
                    binomial(*context.random, static_cast<i32>(function.a) + fortune, function.b);
                break;

            case registry::LootFunction::LimitCount:
                amount =
                    std::clamp(amount, static_cast<i32>(function.a), static_cast<i32>(function.b));
                break;

            case registry::LootFunction::ExplosionDecay:
            case registry::LootFunction::Unsupported:
            default: break;
        }
    }
    return amount;
}

bool LootTables::expand(u32 entry_index, const Context& context, u32& chosen) const {
    if (entry_index >= data_.entries.size()) {
        return false;
    }
    const registry::LootEntryRecord& entry = data_.entries[entry_index];
    if (!test(entry.condition_first, entry.condition_count, context)) {
        return false;
    }
    switch (static_cast<registry::LootEntryKind>(entry.kind)) {
        case registry::LootEntryKind::Item: chosen = entry_index; return true;
        case registry::LootEntryKind::Alternatives:
            // The first child that passes wins, and the rest are not even
            // considered. That ordering is the whole of "silk touch instead of
            // cobblestone".
            for (u32 i = 0; i < entry.child_count; ++i) {
                if (expand(entry.child_first + i, context, chosen)) {
                    return true;
                }
            }
            return false;
        default: return false;
    }
}

void LootTables::drops(registry::BlockStateId state, const Held& held,
                       math::XoroshiroRandomSource& random, std::vector<Drop>& out,
                       const Neighbours& around) const {
    const registry::BlockId block = blocks_->block_of(state);
    if (block.value() >= data_.tables.size()) {
        return;
    }
    const registry::LootTableRecord& table = data_.tables[block.value()];
    if (table.present == 0) {
        return;
    }

    const Context context{state, &held, &random, &around, held.fortune, held.silk_touch > 0};

    for (u32 p = 0; p < table.pool_count; ++p) {
        const u32 pool_index = table.pool_first + p;
        if (pool_index >= data_.pools.size()) {
            break;
        }
        const registry::LootPoolRecord& pool = data_.pools[pool_index];
        if (!test(pool.condition_first, pool.condition_count, context)) {
            continue;
        }

        const auto rolls = static_cast<i32>(pool.rolls);
        for (i32 roll = 0; roll < rolls; ++roll) {
            // Every block table gives its entries the default weight, so the
            // first one that expands is the one that drops. Picking by weight
            // would be the same answer with more arithmetic.
            u32  chosen = 0;
            bool found  = false;
            for (u32 e = 0; e < pool.entry_count && !found; ++e) {
                found = expand(pool.entry_first + e, context, chosen);
            }
            if (!found) {
                continue;
            }

            const registry::LootEntryRecord& entry = data_.entries[chosen];
            i32                              count = 1;
            // Entry, then pool, then table: vanilla's order, and it matters —
            // a set_count on the entry has to happen before a limit_count on
            // the table clamps it.
            count = apply(entry.function_first, entry.function_count, count, context);
            count = apply(pool.function_first, pool.function_count, count, context);
            count = apply(table.function_first, table.function_count, count, context);

            if (count > 0) {
                out.push_back(Drop{static_cast<registry::ProtocolId>(entry.item), count});
            }
        }
    }
}

}  // namespace ov::gameplay
