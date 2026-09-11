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

#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
    nbt::Tag data = furnace_with({stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)});
    for (int tick = 0; tick < 10; ++tick) {
        (void)tick_furnace_entity(*loaded().registries, *loaded().items, *loaded().book,
                                  gameplay::FurnaceKind::Furnace, data);
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
    nbt::Tag data = furnace_with({stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)});
    int      produced = 0;
    for (int tick = 0; tick < 1600; ++tick) {
        produced += tick_furnace_entity(*loaded().registries, *loaded().items, *loaded().book,
                                        gameplay::FurnaceKind::Furnace, data)
                        .step.produced
                        ? 1
                        : 0;
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
    (void)tick_furnace_entity(*loaded().registries, *loaded().items, *loaded().book,
                              gameplay::FurnaceKind::Furnace, data);
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
    OneFurnace one{furnace_with({stack(0, "minecraft:iron_ore", 8), stack(1, "minecraft:coal", 1)})};

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
