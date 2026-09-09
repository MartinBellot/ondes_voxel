#include "ov/gameplay/loot.hpp"

#include "ov/gameplay/smelting.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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


// ── Entity tables ───────────────────────────────────────────────────────────

namespace {

/// Kinds, shared with tools/ov_datagen/entity_loot.py.
enum class EntityCondition : u32 {
    KilledByPlayer,
    RandomChance,
    RandomChanceWithLooting,
    Inverted,
    ThisOnFire,
    KillerInTag,
    SlimeSizeEquals,
    SlimeSizeAtLeast,
    SourceEntityIs,
    SourceFrogVariant,
    SourceIsLightning,
    Unsupported,
};

enum class EntityFunction : u32 {
    SetCountConstant,
    SetCountUniform,
    LootingEnchant,
    FurnaceSmelt,
    SetPotion,
    Unsupported,
};

enum class EntityEntry : u32 { Item, Empty, TagExpand, LootTable, Unsupported };

constexpr u8  kEntityLootMagic[4] = {'O', 'V', 'E', 'L'};
constexpr u32 kEntityLootVersion  = 1;

/// Little-endian words, the way the emitter writes them.
[[nodiscard]] u32 read_u32(std::span<const u8> data, usize offset) noexcept {
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8U) |
           (static_cast<u32>(data[offset + 2]) << 16U) |
           (static_cast<u32>(data[offset + 3]) << 24U);
}

[[nodiscard]] f32 read_f32(std::span<const u8> data, usize offset) noexcept {
    const u32 bits = read_u32(data, offset);
    f32       out{};
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

/// Vanilla's inclusive integer uniform: `nextInt(max - min + 1) + min`.
///
/// Written this way round on purpose. The obvious `min + nextInt(range)` with
/// an exclusive range is off by one at the top, and the error only shows on the
/// rarest outcome of every table that uses one — two mutton rather than one.
[[nodiscard]] i32 uniform_int(math::XoroshiroRandomSource& random, i32 low, i32 high) noexcept {
    if (high <= low) {
        return low;
    }
    return random.next_int(high - low + 1) + low;
}

[[nodiscard]] f32 uniform_float(math::XoroshiroRandomSource& random, f32 low, f32 high) noexcept {
    if (high <= low) {
        return low;
    }
    return random.next_float() * (high - low) + low;
}

}  // namespace

/// What an entity draw carries down the recursion.
struct EntityLootTables::Context {
    const KillContext*           kill;
    math::XoroshiroRandomSource* random;
    DrawResult*                  result;
};

std::expected<EntityLootTables, registry::RegistryError> EntityLootTables::from_bytes(
    std::vector<u8> data, const registry::Registries& registries, const RecipeBook* recipes) {
    constexpr usize kHeader = 4 + 7 * 4;
    if (data.size() < kHeader ||
        !std::equal(std::begin(kEntityLootMagic), std::end(kEntityLootMagic), data.begin())) {
        return std::unexpected(registry::RegistryError::Corrupt);
    }
    const std::span<const u8> bytes{data};
    if (read_u32(bytes, 4) != kEntityLootVersion) {
        return std::unexpected(registry::RegistryError::VersionMismatch);
    }
    const u32 table_count     = read_u32(bytes, 8);
    const u32 pool_count      = read_u32(bytes, 12);
    const u32 entry_count     = read_u32(bytes, 16);
    const u32 condition_count = read_u32(bytes, 20);
    const u32 function_count  = read_u32(bytes, 24);
    const u32 string_bytes    = read_u32(bytes, 28);

    const usize needed = kHeader + usize{table_count} * 12 + usize{pool_count} * 32 +
                         usize{entry_count} * 40 + usize{condition_count} * 24 +
                         usize{function_count} * 24 + string_bytes;
    if (data.size() < needed) {
        return std::unexpected(registry::RegistryError::Corrupt);
    }

    EntityLootTables out;
    out.registries_ = &registries;
    out.recipes_    = recipes;
    usize offset    = kHeader;

    out.tables_.reserve(table_count);
    for (u32 i = 0; i < table_count; ++i, offset += 12) {
        out.tables_.push_back(TableRecord{read_u32(bytes, offset), read_u32(bytes, offset + 4),
                                          read_u32(bytes, offset + 8)});
    }
    out.pools_.reserve(pool_count);
    for (u32 i = 0; i < pool_count; ++i, offset += 32) {
        out.pools_.push_back(PoolRecord{read_f32(bytes, offset), read_f32(bytes, offset + 4),
                                        read_u32(bytes, offset + 8), read_u32(bytes, offset + 12),
                                        read_u32(bytes, offset + 16), read_u32(bytes, offset + 20),
                                        read_u32(bytes, offset + 24),
                                        read_u32(bytes, offset + 28)});
    }
    out.entries_.reserve(entry_count);
    for (u32 i = 0; i < entry_count; ++i, offset += 40) {
        out.entries_.push_back(EntryRecord{
            read_u32(bytes, offset), read_u32(bytes, offset + 4), read_u32(bytes, offset + 8),
            read_u32(bytes, offset + 12), read_u32(bytes, offset + 16),
            read_u32(bytes, offset + 20), read_u32(bytes, offset + 24),
            read_u32(bytes, offset + 28), read_u32(bytes, offset + 32),
            read_u32(bytes, offset + 36)});
    }
    out.conditions_.reserve(condition_count);
    for (u32 i = 0; i < condition_count; ++i, offset += 24) {
        out.conditions_.push_back(ConditionRecord{
            read_u32(bytes, offset), read_u32(bytes, offset + 4), read_f32(bytes, offset + 8),
            read_f32(bytes, offset + 12), read_u32(bytes, offset + 16),
            read_u32(bytes, offset + 20)});
    }
    out.functions_.reserve(function_count);
    for (u32 i = 0; i < function_count; ++i, offset += 24) {
        out.functions_.push_back(FunctionRecord{
            read_u32(bytes, offset), read_f32(bytes, offset + 4), read_f32(bytes, offset + 8),
            read_u32(bytes, offset + 12), read_u32(bytes, offset + 16),
            read_u32(bytes, offset + 20)});
    }
    out.strings_.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                        data.begin() + static_cast<std::ptrdiff_t>(offset + string_bytes));
    return out;
}

usize EntityLootTables::table_count() const noexcept { return tables_.size(); }

std::string_view EntityLootTables::string_at(u32 offset) const noexcept {
    if (static_cast<usize>(offset) + 2 > strings_.size()) {
        return {};
    }
    const auto length =
        static_cast<usize>(strings_[offset]) | (static_cast<usize>(strings_[offset + 1]) << 8U);
    if (static_cast<usize>(offset) + 2 + length > strings_.size()) {
        return {};
    }
    return std::string_view{reinterpret_cast<const char*>(strings_.data()) + offset + 2, length};
}

std::optional<u32> EntityLootTables::find_table(std::string_view name) const noexcept {
    for (u32 index = 0; index < tables_.size(); ++index) {
        if (string_at(tables_[index].name) == name) {
            return index;
        }
    }
    return std::nullopt;
}

bool EntityLootTables::has_table(std::string_view entity_type) const noexcept {
    return find_table(entity_type).has_value();
}

bool EntityLootTables::test_one(u32 index, Context& context) const {
    if (index >= conditions_.size()) {
        return false;
    }
    const ConditionRecord& condition = conditions_[index];
    switch (static_cast<EntityCondition>(condition.kind)) {
        case EntityCondition::KilledByPlayer: return context.kill->killed_by_player;

        case EntityCondition::RandomChance: return context.random->next_float() < condition.value;

        case EntityCondition::RandomChanceWithLooting: {
            // chance + looting * multiplier. The multiplier is an *increment*
            // to the chance, not a factor: Looting III on a drowned's trident
            // takes 11% to 17%, not to three times 11%.
            const f32 chance =
                condition.value + static_cast<f32>(context.kill->looting) * condition.value2;
            return context.random->next_float() < chance;
        }

        case EntityCondition::Inverted: return !test_one(condition.aux, context);

        case EntityCondition::ThisOnFire: return context.kill->on_fire;

        case EntityCondition::KillerInTag: {
            if (context.kill->killer_type.empty()) {
                return false;
            }
            const auto registry = registries_->find("minecraft:entity_type");
            if (!registry) {
                ++context.result->unsupported_conditions;
                return false;
            }
            const auto tag = registries_->find_tag(*registry, string_at(condition.aux));
            const auto id  = registries_->protocol_id(*registry, context.kill->killer_type);
            if (!tag || !id) {
                ++context.result->unsupported_conditions;
                return false;
            }
            return registries_->tag_contains(*tag, *id);
        }

        case EntityCondition::SlimeSizeEquals:
            return context.kill->slime_size == static_cast<i32>(condition.count);

        case EntityCondition::SlimeSizeAtLeast:
            return context.kill->slime_size >= static_cast<i32>(condition.count);

        case EntityCondition::SourceEntityIs:
            return context.kill->source_entity_type == string_at(condition.aux);

        case EntityCondition::SourceFrogVariant:
            return context.kill->source_entity_type == "minecraft:frog" &&
                   context.kill->source_frog_variant == string_at(condition.aux);

        case EntityCondition::SourceIsLightning: return context.kill->source_is_lightning;

        case EntityCondition::Unsupported:
        default:
            // False, never true. A condition we do not understand must not open
            // a drop the real game keeps shut.
            ++context.result->unsupported_conditions;
            return false;
    }
}

bool EntityLootTables::test(u32 first, u32 count, Context& context) const {
    for (u32 i = 0; i < count; ++i) {
        if (!test_one(first + i, context)) {
            return false;
        }
    }
    return true;
}

void EntityLootTables::apply(u32 first, u32 count, Drop& drop, Context& context) const {
    for (u32 i = 0; i < count; ++i) {
        const u32 index = first + i;
        if (index >= functions_.size()) {
            return;
        }
        const FunctionRecord& function = functions_[index];
        if (!test(function.condition_first, function.condition_count, context)) {
            continue;
        }
        switch (static_cast<EntityFunction>(function.kind)) {
            case EntityFunction::SetCountConstant: drop.count = static_cast<i32>(function.a); break;

            case EntityFunction::SetCountUniform:
                drop.count = uniform_int(*context.random, static_cast<i32>(function.a),
                                         static_cast<i32>(function.b));
                break;

            case EntityFunction::LootingEnchant: {
                if (context.kill->looting == 0) {
                    break;
                }
                const f32 rolled = static_cast<f32>(context.kill->looting) *
                                   uniform_float(*context.random, function.a, function.b);
                drop.count += static_cast<i32>(std::lround(rolled));
                if (function.aux != 0 && drop.count > static_cast<i32>(function.aux)) {
                    drop.count = static_cast<i32>(function.aux);
                }
                break;
            }

            case EntityFunction::FurnaceSmelt: {
                if (recipes_ == nullptr) {
                    ++context.result->unsupported_functions;
                    break;
                }
                const auto cooked = match_cooking(*recipes_, FurnaceKind::Furnace, drop.item);
                if (!cooked) {
                    // Not every drop has a recipe — a spider's string does not
                    // — and vanilla leaves those alone rather than losing them.
                    break;
                }
                const auto result = recipes_->result(*cooked);
                if (result) {
                    drop.item = result->item;
                }
                break;
            }

            case EntityFunction::SetPotion:
                // The stack keeps its count and gains NBT this layer does not
                // model. Counted, so a caller sees it rather than wondering why
                // a witch's potion has no effect.
                ++context.result->unsupported_functions;
                break;

            case EntityFunction::Unsupported:
            default: ++context.result->unsupported_functions; break;
        }
    }
}

DrawResult EntityLootTables::drops(const KillContext& kill, math::XoroshiroRandomSource& random,
                                   std::vector<Drop>& out) const {
    DrawResult result;
    const auto table_index = find_table(kill.entity_type);
    if (!table_index) {
        return result;
    }
    Context context{&kill, &random, &result};
    run_table(*table_index, context, out, 0);
    return result;
}

void EntityLootTables::run_table(u32 table_index, Context& context, std::vector<Drop>& out,
                                 i32 depth) const {
    if (table_index >= tables_.size() || depth > 4) {
        return;
    }
    DrawResult&        result = *context.result;
    auto&              random = *context.random;
    const TableRecord& table  = tables_[table_index];
    for (u32 p = 0; p < table.pool_count; ++p) {
        const u32 pool_index = table.pool_first + p;
        if (pool_index >= pools_.size()) {
            break;
        }
        const PoolRecord& pool = pools_[pool_index];
        if (!test(pool.condition_first, pool.condition_count, context)) {
            continue;
        }
        const i32 rolls = uniform_int(random, static_cast<i32>(pool.rolls_min),
                                      static_cast<i32>(pool.rolls_max));
        for (i32 roll = 0; roll < rolls; ++roll) {
            // Every entry whose own conditions pass joins the weighted draw.
            // Entity pools are small — the largest has three entries — so the
            // total is summed rather than cached.
            i32 total = 0;
            for (u32 e = 0; e < pool.entry_count; ++e) {
                const u32 index = pool.entry_first + e;
                if (index < entries_.size() &&
                    test(entries_[index].condition_first, entries_[index].condition_count,
                         context)) {
                    total += static_cast<i32>(entries_[index].weight);
                }
            }
            if (total <= 0) {
                continue;
            }
            i32                pick   = random.next_int(total);
            const EntryRecord* chosen = nullptr;
            for (u32 e = 0; e < pool.entry_count; ++e) {
                const u32 index = pool.entry_first + e;
                if (index >= entries_.size()) {
                    continue;
                }
                const EntryRecord& entry = entries_[index];
                if (!test(entry.condition_first, entry.condition_count, context)) {
                    continue;
                }
                pick -= static_cast<i32>(entry.weight);
                if (pick < 0) {
                    chosen = &entry;
                    break;
                }
            }
            if (chosen == nullptr) {
                continue;
            }

            Drop drop{0, 1};
            switch (static_cast<EntityEntry>(chosen->kind)) {
                case EntityEntry::Empty: continue;

                case EntityEntry::TagExpand: {
                    // `expand: true` means one member of the tag chosen at
                    // random — the creeper's music disc, and nothing else in
                    // the game.
                    const auto registry = registries_->find("minecraft:item");
                    const auto tag =
                        registry ? registries_->find_tag(*registry, string_at(chosen->aux))
                                 : std::nullopt;
                    const auto members =
                        tag ? registries_->tag_members(*tag) : std::span<const registry::ProtocolId>{};
                    if (members.empty()) {
                        ++result.unsupported_entries;
                        continue;
                    }
                    drop.item =
                        members[static_cast<usize>(random.next_int(static_cast<i32>(members.size())))];
                    break;
                }

                case EntityEntry::LootTable: {
                    // A table that is another table. The sixteen wool tables do
                    // this — `entities/sheep/white` is one pool of wool plus a
                    // reference to `entities/sheep` for the mutton — and a
                    // reference this file does hold is followed rather than
                    // reported. Refusing them cost sixteen tables out of
                    // ninety-three in the first confrontation: every colour of
                    // sheep dropped its wool and no meat.
                    //
                    // The name is the datapack's path, `minecraft:entities/x`,
                    // and the tables here are named `minecraft:x`. The prefix is
                    // stripped rather than the names stored both ways.
                    const std::string_view     referenced = string_at(chosen->aux);
                    constexpr std::string_view kPrefix{"minecraft:entities/"};
                    if (referenced.starts_with(kPrefix)) {
                        std::string resolved{"minecraft:"};
                        resolved += referenced.substr(kPrefix.size());
                        if (const auto other = find_table(resolved)) {
                            run_table(*other, context, out, depth + 1);
                            continue;
                        }
                    }
                    // What is left is `minecraft:gameplay/fishing/fish`, which
                    // the two guardian tables name and which is not an entity
                    // table. Reported, never silently empty.
                    ++result.referenced_tables;
                    continue;
                }

                case EntityEntry::Item: drop.item = static_cast<registry::ProtocolId>(chosen->item); break;

                case EntityEntry::Unsupported:
                default: ++result.unsupported_entries; continue;
            }

            // Entry then pool: vanilla's order. A looting_enchant on the entry
            // has to run before anything on the pool could clamp what it made.
            apply(chosen->function_first, chosen->function_count, drop, context);
            apply(pool.function_first, pool.function_count, drop, context);
            if (drop.count > 0) {
                out.push_back(drop);
            }
        }
    }
}

}  // namespace ov::gameplay
