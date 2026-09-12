// The furnace as a ticked block entity (furnace_entity.hpp).
//
// Two regressions are pinned here, both found by reading the server rather than
// by a player:
//
//   * the headless pass wrote `BurnTime` and `CookTime` back only when a slot
//     changed, so every tick reloaded stale counters: a furnace nobody watched
//     burnt for ever and never finished an item;
//   * the screen lit its furnace through `Chunk::set_block`, which drops the
//     block entity at the position it writes: a furnace that caught while its
//     screen was open lost its ore, its fuel and its experience.
#include "../src/block_container.hpp"
#include "../src/furnace_entity.hpp"
#include "../src/workbench.hpp"

#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

struct Loaded {
    std::optional<registry::Registries>    registries;
    std::optional<gameplay::RecipeBook>    book;
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::RegistryId>    items;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        if (auto regs = registry::Registries::load(path)) {
            out.registries = std::move(*regs);
            out.book.emplace(*out.registries);
            out.items = out.registries->find("minecraft:item");
        }
        if (auto blocks = registry::BlockRegistry::load(path)) {
            out.blocks = std::move(*blocks);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] bool have_data() {
    const Loaded& l = loaded();
    return l.registries && l.book && l.blocks && l.items;
}

[[nodiscard]] registry::ProtocolId item_of(std::string_view name) {
    const auto id = loaded().registries->protocol_id(*loaded().items, name);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] nbt::Tag stack(i8 slot, std::string_view id, i8 count) {
    nbt::Tag entry = nbt::Tag::make_compound();
    (void)entry.put("Slot", nbt::Tag{slot});
    (void)entry.put("id", nbt::Tag{std::string{id}});
    (void)entry.put("Count", nbt::Tag{count});
    return entry;
}

/// A furnace as `/setblock` or a hopper would leave it: items, no counters.
[[nodiscard]] nbt::Tag furnace_with(std::vector<nbt::Tag> stacks) {
    nbt::Tag data  = nbt::Tag::make_compound();
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    for (nbt::Tag& entry : stacks) {
        (void)items.push(std::move(entry));
    }
    (void)data.put("Items", std::move(items));
    return data;
}

[[nodiscard]] gameplay::FurnaceSlots slots_of(const nbt::Tag& data) {
    gameplay::FurnaceSlots slots;
    read_furnace_slots(*loaded().registries, *loaded().items, data, slots);
    return slots;
}

[[nodiscard]] i32 number(const nbt::Tag& data, std::string_view key) {
    const nbt::Tag* found = data.find(key);
    return found != nullptr ? static_cast<i32>(found->as_i64()) : -1;
}

[[nodiscard]] std::string recipe_name(gameplay::FurnaceKind kind, std::string_view input) {
    const auto recipe = gameplay::match_cooking(*loaded().book, kind, item_of(input));
    REQUIRE(recipe.has_value());
    return std::string{loaded().book->name(*recipe)};
}

/// One tick of a furnace, the pass's way, with that furnace's memory.
FurnaceEntityTick tick_once(nbt::Tag& data, FurnaceInputMemory& memory,
                            gameplay::FurnaceKind kind = gameplay::FurnaceKind::Furnace) {
    return tick_furnace_entity(*loaded().registries, *loaded().items, *loaded().book, kind, data,
                               memory);
}

/// Replace the whole `Items` list, as a hopper or a click would leave it.
void fill(nbt::Tag& data, std::vector<nbt::Tag> stacks) {
    const nbt::Tag fresh = furnace_with(std::move(stacks));
    (void)data.put("Items", *fresh.find("Items"));
}

/// Put this in slot 0 and keep the others: `item replace … container.0`.
void set_input(nbt::Tag& data, std::optional<nbt::Tag> entry) {
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    if (const nbt::Tag* list = data.find("Items"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& old : *list->list()) {
            const nbt::Tag* slot = old.find("Slot");
            if (slot != nullptr && slot->as_i64() != 0) {
                nbt::Tag kept = old;
                (void)items.push(std::move(kept));
            }
        }
    }
    if (entry) {
        (void)items.push(std::move(*entry));
    }
    (void)data.put("Items", std::move(items));
}

/// As a save carries it: vanilla's and ours always write the total.
[[nodiscard]] nbt::Tag with_total(nbt::Tag data, i16 total) {
    (void)data.put("CookTimeTotal", nbt::Tag{total});
    return data;
}

/// One chunk holding one furnace block and its block entity at (3, 64, 5).
struct OneFurnace {
    world::Chunk chunk;
    BlockPos     at{3, 64, 5};

    explicit OneFurnace(nbt::Tag data)
        : chunk{ChunkPos{0, 0}, world::WorldShape::overworld(),
                world::AirStates::from(*loaded().blocks), &*loaded().blocks} {
        const registry::BlockRegistry& blocks  = *loaded().blocks;
        const auto                     furnace = blocks.find_block("minecraft:furnace");
        REQUIRE(furnace.has_value());
        chunk.set_block(3, 64, 5, blocks.default_state(*furnace));
        world::BlockEntity entity;
        entity.x    = 3;
        entity.y    = 64;
        entity.z    = 5;
        entity.type = "minecraft:furnace";
        entity.data = std::move(data);
        chunk.set_block_entity(std::move(entity));
    }

    [[nodiscard]] world::BlockEntity* entity() { return chunk.block_entity_at(3, 64, 5); }

    [[nodiscard]] std::string_view lit() const {
        const registry::BlockRegistry& blocks = *loaded().blocks;
        const registry::BlockStateId   state  = chunk.get_block(3, 64, 5);
        const auto property = blocks.find_property(blocks.block_of(state), "lit");
        REQUIRE(property.has_value());
        return blocks.property_value(state, *property);
    }
};

}  // namespace

TEST_CASE("a furnace nobody watches keeps its counters in its block entity", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    // As in play: the pass sees the furnace empty, then something fills it.
    nbt::Tag           data = furnace_with({});
    FurnaceInputMemory memory;
    (void)tick_once(data, memory);
    fill(data, {stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)});
    for (int tick = 0; tick < 10; ++tick) {
        (void)tick_once(data, memory);
    }
    // The regression: these were written back only when a slot changed, and
    // the next tick read them again from the block entity.
    CHECK(number(data, "CookTime") == 10);
    CHECK(number(data, "BurnTime") == 1600 - 9);
    CHECK(number(data, "CookTimeTotal") == 200);
    // Vanilla's types: shorts.
    REQUIRE(data.find("BurnTime") != nullptr);
    CHECK(data.find("BurnTime")->get_if<i16>() != nullptr);
    CHECK(data.find("CookTime")->get_if<i16>() != nullptr);
}

TEST_CASE("one coal smelts exactly eight items, watched or not", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    // Filled after the pass first saw it empty: a hopper's or a player's way.
    nbt::Tag           data = furnace_with({});
    FurnaceInputMemory memory;
    (void)tick_once(data, memory);
    fill(data, {stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)});
    int produced = 0;
    for (int tick = 0; tick < 1600; ++tick) {
        produced += tick_once(data, memory).step.produced ? 1 : 0;
    }
    const gameplay::FurnaceSlots slots = slots_of(data);
    CHECK(produced == 8);
    CHECK(slots.input.empty());
    CHECK(slots.fuel.empty());
    CHECK(slots.output.item == item_of("minecraft:iron_ingot"));
    CHECK(slots.output.count == 8);
    CHECK(recipes_used(data, recipe_name(gameplay::FurnaceKind::Furnace, "minecraft:iron_ore")) ==
          8);
    // Burnt out on the tick the eighth came out.
    (void)tick_once(data, memory);
    CHECK(number(data, "BurnTime") == 0);
}

TEST_CASE("Chunk::set_block drops a block entity; relight_furnace_block does not", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    const registry::BlockRegistry& blocks = *loaded().blocks;

    // The trap the screen fell into: a property change written as a new block.
    {
        OneFurnace one{furnace_with({stack(0, "minecraft:iron_ore", 4)})};
        const registry::BlockStateId state    = one.chunk.get_block(3, 64, 5);
        const auto                   property = blocks.find_property(blocks.block_of(state), "lit");
        REQUIRE(property.has_value());
        u16 lit_true = 0;
        for (u16 i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == "true") {
                lit_true = i;
            }
        }
        one.chunk.set_block(3, 64, 5, blocks.with_property(state, *property, lit_true));
        CHECK(one.entity() == nullptr);
    }

    OneFurnace one{furnace_with({stack(0, "minecraft:iron_ore", 4), stack(1, "minecraft:coal", 2)})};
    const auto next = relight_furnace_block(one.chunk, blocks, one.at, true);
    REQUIRE(next.has_value());
    CHECK(one.lit() == "true");
    REQUIRE(one.entity() != nullptr);
    CHECK(slots_of(one.entity()->data).input.count == 4);
    CHECK(slots_of(one.entity()->data).fuel.count == 2);
    // Same value again: nothing to write.
    CHECK_FALSE(relight_furnace_block(one.chunk, blocks, one.at, true).has_value());
}

TEST_CASE("the furnace pass lights, cooks and puts out a furnace in a loaded chunk", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    const registry::BlockRegistry& blocks = *loaded().blocks;
    OneFurnace one{
        with_total(furnace_with({stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)}),
                   200)};

    int         flips = 0;
    int         dirty = 0;
    FurnaceHost host;
    host.chunk = [&](i32 cx, i32 cz) -> world::Chunk* {
        return cx == 0 && cz == 0 ? &one.chunk : nullptr;
    };
    host.for_each_chunk = [&](const std::function<void(ChunkPos, const world::Chunk&)>& visit) {
        visit(ChunkPos{0, 0}, one.chunk);
    };
    host.set_lit = [&](world::Chunk& chunk, BlockPos at, bool lit) {
        if (relight_furnace_block(chunk, blocks, at, lit)) {
            ++flips;
        }
    };
    host.mark_dirty = [&](i32, i32) { ++dirty; };

    FurnaceEntities pass{*loaded().registries, *loaded().book};
    FurnaceStats    first = pass.tick(host, 0);
    CHECK(first.furnaces == 1);
    CHECK(first.burning == 1);
    CHECK(one.lit() == "true");
    // The flip kept the block entity, and with it the furnace.
    REQUIRE(one.entity() != nullptr);
    CHECK(slots_of(one.entity()->data).input.count == 8);

    usize cooked = 0;
    for (i64 now = 1; now < 1601; ++now) {
        cooked += pass.tick(host, now).cooked;
    }
    CHECK(cooked == 8);
    CHECK(one.lit() == "false");
    CHECK(flips == 2);
    CHECK(dirty > 0);
    REQUIRE(one.entity() != nullptr);
    CHECK(slots_of(one.entity()->data).output.count == 8);
}

TEST_CASE("RecipesUsed turns into experience at extraction, per recipe", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    const std::string iron  = recipe_name(gameplay::FurnaceKind::Furnace, "minecraft:iron_ore");
    const std::string stone = recipe_name(gameplay::FurnaceKind::Furnace, "minecraft:cobblestone");

    // Ten iron ingots at 0.7: exactly 7, whatever the draw.
    {
        nbt::Tag data = furnace_with({});
        for (int i = 0; i < 10; ++i) {
            count_recipe_used(data, iron);
        }
        math::LegacyRandomSource random{1};
        const std::vector<i32>   awards = take_recipes_used_experience(data, *loaded().book, random);
        REQUIRE(awards.size() == 1);
        CHECK(awards[0] == 7);
        // Emptied: a second extraction pays nothing.
        CHECK(recipes_used(data, iron) == 0);
        CHECK(take_recipes_used_experience(data, *loaded().book, random).empty());
    }

    // Five stone at 0.1: 0.5, so sometimes a point and sometimes none. One
    // generator for every trial, as the server has: the first draw of a
    // Java-style generator barely moves across small consecutive seeds, and
    // 200 seeds 0..199 all drew above 0.5.
    int                      ones  = 0;
    int                      zeros = 0;
    math::LegacyRandomSource draws{0x5354'4f4e'45LL};
    for (int trial = 0; trial < 200; ++trial) {
        nbt::Tag data = furnace_with({});
        for (int i = 0; i < 5; ++i) {
            count_recipe_used(data, stone);
        }
        const std::vector<i32> awards = take_recipes_used_experience(data, *loaded().book, draws);
        if (awards.empty()) {
            ++zeros;
        } else {
            REQUIRE(awards.size() == 1);
            CHECK(awards[0] == 1);
            ++ones;
        }
    }
    CHECK(ones > 50);
    CHECK(zeros > 50);

    // A recipe this server does not know is dropped, not a crash.
    nbt::Tag data = furnace_with({});
    count_recipe_used(data, "minecraft:not_a_recipe");
    math::LegacyRandomSource random{7};
    CHECK(take_recipes_used_experience(data, *loaded().book, random).empty());
}

namespace {

// ── The vanilla oracle ──────────────────────────────────────────────────────
//
// `scripts/measure_furnaces.py ticks` against the real 1.20.1 server: twelve
// furnaces placed by `/setblock` with their NBT and `CookTimeTotal`, no
// player online, read with `data get block` `dt` ticks later. `dt` is exact:
// placing and reading each run in one batch with a `time query gametime`.
//
// The last row is the control, placed **without** `CookTimeTotal`: vanilla
// burns its coal and counts `CookTime` up to 1202 without ever finishing an
// item, and so does this server — the total is read again only when the
// input changes (FurnaceInputMemory).

struct VanillaSlot {
    std::string_view item;  // empty: nothing there
    i32              count{0};
};

struct VanillaCell {
    i32                        dt{0};
    i32                        burn{0};
    i32                        cook{0};
    i32                        total{0};
    std::array<VanillaSlot, 3> slots{};
};

struct VanillaFurnace {
    std::string_view                                  name;
    gameplay::FurnaceKind                             kind;
    std::vector<std::tuple<i8, std::string_view, i8>> setup;
    i32                                               total;
    std::vector<VanillaCell>                          cells;
};

using K = gameplay::FurnaceKind;

[[nodiscard]] const std::vector<VanillaFurnace>& vanilla_furnaces() {
    static const std::vector<VanillaFurnace> table{
        {"exact", K::Furnace, {{0, "iron_ore", 8}, {1, "coal", 1}}, 200,
         {{22, 1579, 22, 200, {{{"iron_ore", 8}, {}, {}}}},
          {153, 1448, 153, 200, {{{"iron_ore", 8}, {}, {}}}},
          {603, 998, 3, 200, {{{"iron_ore", 5}, {}, {"iron_ingot", 3}}}},
          {1202, 399, 2, 200, {{{"iron_ore", 2}, {}, {"iron_ingot", 6}}}},
          {1702, 0, 0, 200, {{{}, {}, {"iron_ingot", 8}}}},
          {2503, 0, 0, 200, {{{}, {}, {"iron_ingot", 8}}}}}},
        {"runs-out", K::Furnace, {{0, "iron_ore", 10}, {1, "coal", 1}}, 200,
         {{22, 1579, 22, 200, {{{"iron_ore", 10}, {}, {}}}},
          {153, 1448, 153, 200, {{{"iron_ore", 10}, {}, {}}}},
          {603, 998, 3, 200, {{{"iron_ore", 7}, {}, {"iron_ingot", 3}}}},
          {1202, 399, 2, 200, {{{"iron_ore", 4}, {}, {"iron_ingot", 6}}}},
          {1702, 0, 0, 200, {{{"iron_ore", 2}, {}, {"iron_ingot", 8}}}},
          {2503, 0, 0, 200, {{{"iron_ore", 2}, {}, {"iron_ingot", 8}}}}}},
        {"blast", K::BlastFurnace, {{0, "iron_ore", 10}, {1, "coal", 1}}, 100,
         {{22, 779, 22, 100, {{{"iron_ore", 10}, {}, {}}}},
          {153, 648, 53, 100, {{{"iron_ore", 9}, {}, {"iron_ingot", 1}}}},
          {603, 198, 3, 100, {{{"iron_ore", 4}, {}, {"iron_ingot", 6}}}},
          {1202, 0, 0, 100, {{{"iron_ore", 2}, {}, {"iron_ingot", 8}}}},
          {2503, 0, 0, 100, {{{"iron_ore", 2}, {}, {"iron_ingot", 8}}}}}},
        {"smoker", K::Smoker, {{0, "potato", 10}, {1, "coal", 1}}, 100,
         {{22, 779, 22, 100, {{{"potato", 10}, {}, {}}}},
          {153, 648, 53, 100, {{{"potato", 9}, {}, {"baked_potato", 1}}}},
          {603, 198, 3, 100, {{{"potato", 4}, {}, {"baked_potato", 6}}}},
          {1202, 0, 0, 100, {{{"potato", 2}, {}, {"baked_potato", 8}}}},
          {2503, 0, 0, 100, {{{"potato", 2}, {}, {"baked_potato", 8}}}}}},
        {"stick", K::Furnace, {{0, "cobblestone", 3}, {1, "stick", 1}}, 200,
         {{22, 79, 22, 200, {{{"cobblestone", 3}, {}, {}}}},
          {153, 0, 0, 200, {{{"cobblestone", 3}, {}, {}}}},
          {2503, 0, 0, 200, {{{"cobblestone", 3}, {}, {}}}}}},
        {"two-sticks", K::Furnace, {{0, "cobblestone", 3}, {1, "stick", 2}}, 200,
         {{22, 79, 22, 200, {{{"cobblestone", 3}, {"stick", 1}, {}}}},
          {153, 48, 153, 200, {{{"cobblestone", 3}, {}, {}}}},
          {603, 0, 0, 200, {{{"cobblestone", 2}, {}, {"stone", 1}}}},
          {2503, 0, 0, 200, {{{"cobblestone", 2}, {}, {"stone", 1}}}}}},
        {"lava", K::Furnace, {{0, "sand", 20}, {1, "lava_bucket", 1}}, 200,
         {{22, 19979, 22, 200, {{{"sand", 20}, {"bucket", 1}, {}}}},
          {153, 19848, 153, 200, {{{"sand", 20}, {"bucket", 1}, {}}}},
          {603, 19398, 3, 200, {{{"sand", 17}, {"bucket", 1}, {"glass", 3}}}},
          {1202, 18799, 2, 200, {{{"sand", 14}, {"bucket", 1}, {"glass", 6}}}},
          {1702, 18299, 102, 200, {{{"sand", 12}, {"bucket", 1}, {"glass", 8}}}},
          {2503, 17498, 103, 200, {{{"sand", 8}, {"bucket", 1}, {"glass", 12}}}}}},
        {"blocked", K::Furnace,
         {{0, "iron_ore", 4}, {1, "coal", 1}, {2, "gold_ingot", 1}}, 200,
         {{22, 0, 0, 200, {{{"iron_ore", 4}, {"coal", 1}, {"gold_ingot", 1}}}},
          {2503, 0, 0, 200, {{{"iron_ore", 4}, {"coal", 1}, {"gold_ingot", 1}}}}}},
        {"nearly-full", K::Furnace,
         {{0, "iron_ore", 4}, {1, "coal", 1}, {2, "iron_ingot", 63}}, 200,
         {{22, 1579, 22, 200, {{{"iron_ore", 4}, {}, {"iron_ingot", 63}}}},
          {153, 1448, 153, 200, {{{"iron_ore", 4}, {}, {"iron_ingot", 63}}}},
          {603, 998, 0, 200, {{{"iron_ore", 3}, {}, {"iron_ingot", 64}}}},
          {1202, 399, 0, 200, {{{"iron_ore", 3}, {}, {"iron_ingot", 64}}}},
          {2503, 0, 0, 200, {{{"iron_ore", 3}, {}, {"iron_ingot", 64}}}}}},
        {"smoker-refuses", K::Smoker, {{0, "iron_ore", 4}, {1, "coal", 1}}, 100,
         {{22, 0, 0, 100, {{{"iron_ore", 4}, {"coal", 1}, {}}}},
          {2503, 0, 0, 100, {{{"iron_ore", 4}, {"coal", 1}, {}}}}}},
        {"blast-kelp", K::BlastFurnace, {{0, "raw_iron", 30}, {1, "dried_kelp_block", 1}}, 100,
         {{22, 1979, 22, 100, {{{"raw_iron", 30}, {}, {}}}},
          {153, 1848, 53, 100, {{{"raw_iron", 29}, {}, {"iron_ingot", 1}}}},
          {603, 1398, 3, 100, {{{"raw_iron", 24}, {}, {"iron_ingot", 6}}}},
          {1202, 799, 2, 100, {{{"raw_iron", 18}, {}, {"iron_ingot", 12}}}},
          {1702, 299, 2, 100, {{{"raw_iron", 13}, {}, {"iron_ingot", 17}}}},
          {2503, 0, 0, 100, {{{"raw_iron", 10}, {}, {"iron_ingot", 20}}}}}},
        {"no-fuel", K::Furnace, {{0, "iron_ore", 4}}, 200,
         {{22, 0, 0, 200, {{{"iron_ore", 4}, {}, {}}}},
          {2503, 0, 0, 200, {{{"iron_ore", 4}, {}, {}}}}}},
        {"no-total", K::Furnace, {{0, "iron_ore", 8}, {1, "coal", 1}}, -1,
         {{22, 1579, 22, 0, {{{"iron_ore", 8}, {}, {}}}},
          {153, 1448, 153, 0, {{{"iron_ore", 8}, {}, {}}}},
          {603, 998, 603, 0, {{{"iron_ore", 8}, {}, {}}}},
          {1202, 399, 1202, 0, {{{"iron_ore", 8}, {}, {}}}},
          {1702, 0, 0, 0, {{{"iron_ore", 8}, {}, {}}}},
          {2503, 0, 0, 0, {{{"iron_ore", 8}, {}, {}}}}}},
    };
    return table;
}

/// A counter as vanilla's NBT always has it: absent means 0.
[[nodiscard]] i32 counter(const nbt::Tag& data, std::string_view key) {
    const nbt::Tag* found = data.find(key);
    return found != nullptr ? static_cast<i32>(found->as_i64()) : 0;
}

[[nodiscard]] WorkbenchContext bench_context() {
    WorkbenchContext context;
    context.registries    = &*loaded().registries;
    context.book          = &*loaded().book;
    context.item_registry = *loaded().items;
    if (const auto menus = loaded().registries->find("minecraft:menu")) {
        context.menu_registry = *menus;
    }
    return context;
}

/// A furnace screen whose output holds three iron ingots.
struct OutputScreen {
    Workbench                      bench;
    std::array<net::ItemStack, 46> inventory{};
    net::ItemStack                 carried{};

    OutputScreen() {
        bench.kind                 = WorkbenchKind::Furnace;
        bench.furnace_slots.output = gameplay::RecipeStack{item_of("minecraft:iron_ingot"), 3};
    }

    WorkbenchOutcome click(i8 button, i32 mode) {
        net::ContainerClick click;
        click.slot   = 2;
        click.button = button;
        click.mode   = mode;
        return apply_click(bench_context(), bench, click, inventory, carried);
    }
};

}  // namespace

TEST_CASE("twelve furnaces and the control tick as the real server's do", "[furnace][parity]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    for (const VanillaFurnace& furnace : vanilla_furnaces()) {
        std::vector<nbt::Tag> stacks;
        for (const auto& [slot, item, count] : furnace.setup) {
            stacks.push_back(stack(slot, std::string{"minecraft:"} + std::string{item}, count));
        }
        nbt::Tag data = furnace_with(std::move(stacks));
        if (furnace.total >= 0) {  // -1: placed without it, as the control was
            (void)data.put("CookTimeTotal", nbt::Tag{static_cast<i16>(furnace.total)});
        }

        FurnaceInputMemory memory;
        i32                now = 0;
        for (const VanillaCell& cell : furnace.cells) {
            while (now < cell.dt) {
                (void)tick_once(data, memory, furnace.kind);
                ++now;
            }
            INFO(furnace.name << " at dt=" << cell.dt);
            CHECK(counter(data, "BurnTime") == cell.burn);
            CHECK(counter(data, "CookTime") == cell.cook);
            CHECK(counter(data, "CookTimeTotal") == cell.total);
            const gameplay::FurnaceSlots slots = slots_of(data);
            const std::array<const gameplay::RecipeStack*, 3> ours{&slots.input, &slots.fuel,
                                                                  &slots.output};
            for (usize i = 0; i < 3; ++i) {
                INFO("slot " << i);
                if (cell.slots[i].item.empty()) {
                    CHECK(ours[i]->empty());
                } else {
                    CHECK(ours[i]->item ==
                          item_of(std::string{"minecraft:"} + std::string{cell.slots[i].item}));
                    CHECK(ours[i]->count == cell.slots[i].count);
                }
            }
        }
    }
}

TEST_CASE("an input changed by someone else resets it, as the real server does", "[furnace][parity]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    // `scripts/measure_furnaces.py changes`: furnaces with 8 iron ore and a
    // coal, their input set by `item replace … container.0` — the path a
    // hopper or a click takes — after 52 ticks, and read 251 ticks later.
    // Left out: the same item removed and put back within one tick, which
    // vanilla resets and a once-a-tick comparison cannot see.
    const auto run = [](bool total, const std::function<void(nbt::Tag&)>& change) {
        nbt::Tag data =
            furnace_with({stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)});
        if (total) {
            data = with_total(std::move(data), 200);
        }
        FurnaceInputMemory memory;
        for (int t = 0; t < 52; ++t) {
            (void)tick_once(data, memory);
        }
        change(data);
        for (int t = 0; t < 251; ++t) {
            (void)tick_once(data, memory);
        }
        return data;
    };
    const auto input_is = [](const nbt::Tag& data, std::string_view item, i32 count) {
        const gameplay::FurnaceSlots slots = slots_of(data);
        if (item.empty()) {
            CHECK(slots.input.empty());
            return;
        }
        CHECK(slots.input.item == item_of(item));
        CHECK(slots.input.count == count);
    };
    const auto output_is = [](const nbt::Tag& data, std::string_view item, i32 count) {
        const gameplay::FurnaceSlots slots = slots_of(data);
        if (item.empty()) {
            CHECK(slots.output.empty());
            return;
        }
        CHECK(slots.output.item == item_of(item));
        CHECK(slots.output.count == count);
    };

    SECTION("another item: progress lost, the new recipe's total") {
        const nbt::Tag data =
            run(true, [](nbt::Tag& d) { set_input(d, stack(0, "minecraft:sand", 8)); });
        CHECK(counter(data, "CookTime") == 51);
        CHECK(counter(data, "CookTimeTotal") == 200);
        CHECK(counter(data, "BurnTime") == 1298);
        input_is(data, "minecraft:sand", 7);
        output_is(data, "minecraft:glass", 1);
    }
    SECTION("something that does not cook: progress lost, the total kept") {
        const nbt::Tag data =
            run(true, [](nbt::Tag& d) { set_input(d, stack(0, "minecraft:dirt", 8)); });
        CHECK(counter(data, "CookTime") == 0);
        CHECK(counter(data, "CookTimeTotal") == 200);
        CHECK(counter(data, "BurnTime") == 1298);
        output_is(data, "", 0);
    }
    SECTION("the input taken out") {
        const nbt::Tag data = run(true, [](nbt::Tag& d) { set_input(d, std::nullopt); });
        CHECK(counter(data, "CookTime") == 0);
        CHECK(counter(data, "CookTimeTotal") == 200);
        input_is(data, "", 0);
    }
    SECTION("the same item, another count: no change") {
        const nbt::Tag data =
            run(true, [](nbt::Tag& d) { set_input(d, stack(0, "minecraft:iron_ore", 3)); });
        CHECK(counter(data, "CookTime") == 103);
        input_is(data, "minecraft:iron_ore", 2);
        output_is(data, "minecraft:iron_ingot", 1);
    }
    SECTION("the same item with a tag: a change") {
        const nbt::Tag data = run(true, [](nbt::Tag& d) {
            nbt::Tag name = nbt::Tag::make_compound();
            (void)name.put("Name", nbt::Tag{std::string{"\"x\""}});
            nbt::Tag tag = nbt::Tag::make_compound();
            (void)tag.put("display", std::move(name));
            nbt::Tag tagged = stack(0, "minecraft:iron_ore", 8);
            (void)tagged.put("tag", std::move(tag));
            set_input(d, std::move(tagged));
        });
        CHECK(counter(data, "CookTime") == 51);
        input_is(data, "minecraft:iron_ore", 7);
        output_is(data, "minecraft:iron_ingot", 1);
    }
    SECTION("the control, the same item: still stuck") {
        const nbt::Tag data =
            run(false, [](nbt::Tag& d) { set_input(d, stack(0, "minecraft:iron_ore", 7)); });
        CHECK(counter(data, "CookTime") == 303);
        CHECK(counter(data, "CookTimeTotal") == 0);
        input_is(data, "minecraft:iron_ore", 7);
        output_is(data, "", 0);
    }
    SECTION("the control, another item: unstuck") {
        const nbt::Tag data =
            run(false, [](nbt::Tag& d) { set_input(d, stack(0, "minecraft:raw_iron", 8)); });
        CHECK(counter(data, "CookTime") == 51);
        CHECK(counter(data, "CookTimeTotal") == 200);
        input_is(data, "minecraft:raw_iron", 7);
        output_is(data, "minecraft:iron_ingot", 1);
    }
}

TEST_CASE("throwing from a furnace's output pays its RecipesUsed, as vanilla does", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    // Measured: one thrown, two stay, the whole RecipesUsed paid (2 points).
    OutputScreen          one;
    const WorkbenchOutcome thrown_one = one.click(0, 4);
    CHECK(thrown_one.took_furnace_output);
    REQUIRE(thrown_one.overflow.size() == 1);
    CHECK(thrown_one.overflow[0].count == 1);
    CHECK(one.bench.furnace_slots.output.count == 2);

    OutputScreen          all;
    const WorkbenchOutcome thrown_all = all.click(1, 4);
    CHECK(thrown_all.took_furnace_output);
    REQUIRE(thrown_all.overflow.size() == 1);
    CHECK(thrown_all.overflow[0].count == 3);
    CHECK(all.bench.furnace_slots.output.empty());
}

TEST_CASE("a number key takes a furnace's output only into an empty hotbar slot", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    OutputScreen          empty;
    const WorkbenchOutcome taken = empty.click(0, 2);
    CHECK(taken.took_furnace_output);
    CHECK(empty.bench.furnace_slots.output.empty());
    CHECK(empty.inventory[36].item_id == item_of("minecraft:iron_ingot"));
    CHECK(empty.inventory[36].count == 3);

    // Measured with a cobblestone there: nothing moves, nothing is paid.
    OutputScreen occupied;
    occupied.inventory[36].item_id = item_of("minecraft:cobblestone");
    occupied.inventory[36].count   = 1;
    const WorkbenchOutcome refused = occupied.click(0, 2);
    CHECK_FALSE(refused.took_furnace_output);
    CHECK(occupied.bench.furnace_slots.output.count == 3);
    CHECK(occupied.inventory[36].item_id == item_of("minecraft:cobblestone"));
}

TEST_CASE("the Nether's and the End's furnaces cook, each over its own chunks", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    const registry::BlockRegistry& blocks = *loaded().blocks;
    // The same coordinates in two dimensions, with different contents: the
    // overworld's cooks ore into iron, the Nether's cobblestone into stone.
    OneFurnace overworld{with_total(
        furnace_with({stack(0, "minecraft:iron_ore", 1), stack(1, "minecraft:coal", 1)}), 200)};
    OneFurnace nether{with_total(
        furnace_with({stack(0, "minecraft:cobblestone", 1), stack(1, "minecraft:coal", 1)}), 200)};

    const auto host_over = [&](OneFurnace& one) {
        FurnaceHost host;
        host.chunk = [&one](i32 cx, i32 cz) -> world::Chunk* {
            return cx == 0 && cz == 0 ? &one.chunk : nullptr;
        };
        host.for_each_chunk =
            [&one](const std::function<void(ChunkPos, const world::Chunk&)>& visit) {
                visit(ChunkPos{0, 0}, one.chunk);
            };
        host.set_lit = [&blocks](world::Chunk& chunk, BlockPos at, bool lit) {
            (void)relight_furnace_block(chunk, blocks, at, lit);
        };
        host.mark_dirty = [](i32, i32) {};
        return host;
    };
    const FurnaceHost over_host   = host_over(overworld);
    const FurnaceHost nether_host = host_over(nether);
    // The End is not loaded: no host, and nothing to tick.
    const std::array<const FurnaceHost*, DimensionFurnaces::kDimensions> hosts{
        &over_host, &nether_host, nullptr};

    DimensionFurnaces passes{*loaded().registries, *loaded().book};
    usize             cooked = 0;
    for (i64 now = 0; now < 200; ++now) {
        cooked += passes.tick(hosts, now).cooked;
    }
    CHECK(cooked == 2);
    CHECK(slots_of(overworld.entity()->data).output.item == item_of("minecraft:iron_ingot"));
    CHECK(slots_of(nether.entity()->data).output.item == item_of("minecraft:stone"));
    CHECK(passes.pass(1).indexed() == 1);
    CHECK(passes.pass(2).indexed() == 0);
}

TEST_CASE("a hopper emptying the output leaves RecipesUsed for the next player", "[furnace]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    // Measured: a hopper under a furnace took its 3 ingots and the furnace
    // still held `RecipesUsed` = 3 — only a player's extraction pays it out.
    const std::string iron = recipe_name(gameplay::FurnaceKind::Furnace, "minecraft:iron_ore");
    nbt::Tag          data = furnace_with({stack(2, "minecraft:iron_ingot", 3)});
    for (int i = 0; i < 3; ++i) {
        count_recipe_used(data, iron);
    }

    const ContainerSpec* spec = container_spec_for_block("minecraft:furnace");
    REQUIRE(spec != nullptr);
    BlockInventory inventory{*spec, &*loaded().registries, loaded().items};
    inventory.load(data);
    REQUIRE(inventory.stacks()[2].count == 3);
    inventory.stacks()[2] = {};  // what the hopper pulls
    inventory.store(data);

    CHECK(slots_of(data).output.empty());
    CHECK(recipes_used(data, iron) == 3);
}
