// The container model: the catalogue, the NBT round trip, and the faces.
//
// Every assertion here is a thing that was hard-coded to a chest before and is
// now data, plus the two behaviours that are neither obvious nor uniform: what
// vanilla's `Items` list looks like on disk, and which face of a furnace admits
// which slot.
#include "../src/block_container.hpp"
#include "../src/item_transport.hpp"

#include "ov/nbt/binary.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

struct Loaded {
    std::optional<registry::Registries>  registries;
    std::optional<registry::RegistryId>  items;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        Loaded out;
        if (auto regs = registry::Registries::load(pack_path())) {
            out.registries = std::move(*regs);
            out.items      = out.registries->find("minecraft:item");
        }
        return out;
    }();
    return state;
}

[[nodiscard]] registry::ProtocolId item(std::string_view name) {
    REQUIRE(loaded().registries.has_value());
    REQUIRE(loaded().items.has_value());
    const auto id = loaded().registries->protocol_id(*loaded().items, name);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] BlockInventory inventory_of(std::string_view block_name) {
    const ContainerSpec* spec = container_spec_for_block(block_name);
    REQUIRE(spec != nullptr);
    return BlockInventory{*spec, &*loaded().registries, loaded().items};
}

}  // namespace

TEST_CASE("the catalogue answers for every container the server opens", "[container]") {
    struct Case {
        std::string_view block;
        std::string_view entity;
        i32              slots;
    };
    const std::array<Case, 8> cases{{
        {"minecraft:chest", "minecraft:chest", 27},
        {"minecraft:trapped_chest", "minecraft:trapped_chest", 27},
        {"minecraft:barrel", "minecraft:barrel", 27},
        {"minecraft:hopper", "minecraft:hopper", 5},
        {"minecraft:dispenser", "minecraft:dispenser", 9},
        {"minecraft:dropper", "minecraft:dropper", 9},
        {"minecraft:furnace", "minecraft:furnace", 3},
        {"minecraft:blue_shulker_box", "minecraft:shulker_box", 27},
    }};
    for (const Case& one : cases) {
        const ContainerSpec* spec = container_spec_for_block(one.block);
        REQUIRE(spec != nullptr);
        CHECK(spec->entity_type == one.entity);
        CHECK(spec->slots == one.slots);
    }

    // Stone is not a container, and neither is a block that does not exist.
    CHECK(container_spec_for_block("minecraft:stone") == nullptr);
    CHECK(container_spec_for_block("minecraft:not_a_block") == nullptr);

    // A container this project does not model is *named*, not silently absent.
    CHECK(container_spec_for_block("minecraft:brewing_stand") == nullptr);
    CHECK(std::ranges::find(unmodelled_containers(), "minecraft:brewing_stand") !=
          unmodelled_containers().end());
}

TEST_CASE("Items round-trips through the shape vanilla writes", "[container][anvil]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack: run scripts/setup_vanilla.sh");
        return;
    }

    BlockInventory chest = inventory_of("minecraft:chest");
    chest.stacks()[0]    = net::ItemStack{item("minecraft:cobblestone"), 64, {}};
    chest.stacks()[26]   = net::ItemStack{item("minecraft:diamond"), 1, {}};

    nbt::Tag data = nbt::Tag::make_compound();
    chest.store(data);

    // The list holds only the two non-empty slots, and each entry carries the
    // slot it belongs to. A reader that treated the list as an array would put
    // the diamond in slot 1.
    const nbt::Tag* items = data.find("Items");
    REQUIRE(items != nullptr);
    REQUIRE(items->list() != nullptr);
    REQUIRE(items->list()->size() == 2);
    CHECK((*items->list())[1].find("Slot")->as_i64() == 26);
    CHECK((*items->list())[1].find("id")->as_string() == "minecraft:diamond");
    CHECK((*items->list())[1].find("Count")->as_i64() == 1);

    BlockInventory reloaded = inventory_of("minecraft:chest");
    reloaded.load(data);
    CHECK(reloaded.stacks()[0].item_id == item("minecraft:cobblestone"));
    CHECK(reloaded.stacks()[0].count == 64);
    CHECK(reloaded.stacks()[26].item_id == item("minecraft:diamond"));
    for (usize i = 1; i < 26; ++i) {
        CHECK(reloaded.stacks()[i].empty());
    }
}

TEST_CASE("an item's own NBT survives the round trip", "[container][anvil]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack");
        return;
    }
    // A `tag` compound as the wire carries it: a whole named document.
    nbt::Tag tag = nbt::Tag::make_compound();
    (void)tag.put("Damage", nbt::Tag{i32{7}});

    BlockInventory chest = inventory_of("minecraft:chest");
    chest.stacks()[3] = net::ItemStack{item("minecraft:iron_pickaxe"), 1,
                                       nbt::write(nbt::Document{"tag", tag})};

    nbt::Tag data = nbt::Tag::make_compound();
    chest.store(data);

    // On disk it is a `tag` compound beside `id` and `Count`, which is what the
    // real game reads. Storing the raw wire bytes in a byte array would load
    // as an unenchanted, undamaged pickaxe in vanilla and never say so.
    const nbt::Tag* entry = &(*data.find("Items")->list())[0];
    REQUIRE(entry->find("tag") != nullptr);
    CHECK(entry->find("tag")->find("Damage")->as_i64() == 7);

    BlockInventory reloaded = inventory_of("minecraft:chest");
    reloaded.load(data);
    CHECK(reloaded.stacks()[3].nbt == chest.stacks()[3].nbt);
}

TEST_CASE("a furnace admits each face's slot and no other", "[container][faces]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack");
        return;
    }
    BlockInventory  furnace = inventory_of("minecraft:furnace");
    TagPool         pool;
    ContainerBridge bridge{furnace, &*loaded().registries, pool};

    const gameplay::SlotStack coal{item("minecraft:coal"), 1, 0};
    const gameplay::SlotStack ore{item("minecraft:iron_ore"), 1, 0};

    // From above: the input slot, and only the input slot.
    CHECK(bridge.can_place_into(0, ore, Direction::Up));
    CHECK_FALSE(bridge.can_place_into(1, coal, Direction::Up));
    CHECK_FALSE(bridge.can_place_into(2, ore, Direction::Up));

    // From the side: the fuel slot, and only the fuel slot.
    CHECK(bridge.can_place_into(1, coal, Direction::North));
    CHECK_FALSE(bridge.can_place_into(0, ore, Direction::North));
    CHECK(bridge.can_place_into(1, coal, Direction::East));

    // From below: nothing goes in. The bottom is an exit.
    CHECK_FALSE(bridge.can_place_into(0, ore, Direction::Down));
    CHECK_FALSE(bridge.can_place_into(1, coal, Direction::Down));
    CHECK_FALSE(bridge.can_place_into(2, ore, Direction::Down));

    // And what comes out comes out of the bottom, from the output slot.
    CHECK(bridge.can_take_from(2, Direction::Down));
    CHECK_FALSE(bridge.can_take_from(0, Direction::Down));
    CHECK_FALSE(bridge.can_take_from(2, Direction::Up));
}

TEST_CASE("a chest admits everything through every face", "[container][faces]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack");
        return;
    }
    BlockInventory  chest = inventory_of("minecraft:chest");
    TagPool         pool;
    ContainerBridge bridge{chest, &*loaded().registries, pool};
    const gameplay::SlotStack stone{item("minecraft:stone"), 1, 0};
    for (u8 face = 0; face < kDirectionCount; ++face) {
        CHECK(bridge.can_place_into(0, stone, static_cast<Direction>(face)));
        CHECK(bridge.can_take_from(0, static_cast<Direction>(face)));
    }
}

TEST_CASE("a shulker box refuses another shulker box", "[container][faces]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack");
        return;
    }
    BlockInventory  box = inventory_of("minecraft:shulker_box");
    TagPool         pool;
    ContainerBridge bridge{box, &*loaded().registries, pool};
    CHECK(bridge.can_place_into(0, gameplay::SlotStack{item("minecraft:stone"), 1, 0},
                                Direction::Up));
    CHECK_FALSE(bridge.can_place_into(
        0, gameplay::SlotStack{item("minecraft:red_shulker_box"), 1, 0}, Direction::Up));
}

TEST_CASE("a hopper moves one item per push through the bridge", "[container][hopper]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack");
        return;
    }
    BlockInventory hopper = inventory_of("minecraft:hopper");
    BlockInventory chest  = inventory_of("minecraft:chest");
    hopper.stacks()[0]    = net::ItemStack{item("minecraft:cobblestone"), 5, {}};

    TagPool         pool;
    ContainerBridge from{hopper, &*loaded().registries, pool};
    ContainerBridge into{chest, &*loaded().registries, pool};

    // Facing down: the face the target sees is Up.
    const auto result = gameplay::HopperRules::tick(from, &into, Direction::Down, nullptr);
    CHECK(result.pushed);
    CHECK(hopper.stacks()[0].count == 4);
    CHECK(chest.stacks()[0].item_id == item("minecraft:cobblestone"));
    CHECK(chest.stacks()[0].count == 1);
}

TEST_CASE("the comparator reads floor(14n/27)+1 out of a chest", "[container][comparator]") {
    if (!loaded().registries) {
        SUCCEED("no registry pack");
        return;
    }
    // The rule is measured (28/28 fill levels, docs/provenance/redstone.md);
    // what is checked here is that a real inventory feeds it the right
    // fullness — one full stack in 27 slots is 1/27 of the container, not 1/27
    // of one slot.
    for (i32 stacks = 0; stacks <= 27; ++stacks) {
        BlockInventory chest = inventory_of("minecraft:chest");
        for (i32 i = 0; i < stacks; ++i) {
            chest.stacks()[static_cast<usize>(i)] =
                net::ItemStack{item("minecraft:cobblestone"), 64, {}};
        }
        const i32 expected = stacks == 0 ? 0 : (14 * stacks) / 27 + 1;
        CHECK(chest.comparator_reading() == expected);
    }
}

TEST_CASE("a hopper's suck box is the block above it, not the block itself",
          "[container][hopper]") {
    const BlockPos hopper{10, 64, 20};
    // Dead centre of the block above: taken.
    CHECK(hopper_suck_contains(hopper, 10.5, 65.0, 20.5));
    // Resting on the hopper's own top face is still inside the box, because the
    // box starts at y+1 and an item is a quarter tall.
    CHECK(hopper_suck_contains(hopper, 10.5, 64.9, 20.5));
    // Two blocks up: out of reach.
    CHECK_FALSE(hopper_suck_contains(hopper, 10.5, 66.0, 20.5));
    // The next column over: out of reach.
    CHECK_FALSE(hopper_suck_contains(hopper, 12.5, 65.0, 20.5));
}

TEST_CASE("a tag pool gives back exactly the bytes it was given", "[container]") {
    TagPool               pool;
    const std::vector<u8> bytes{1, 2, 3, 4};
    const u64             tag = pool.intern(bytes);
    CHECK(tag != 0);
    CHECK(pool.bytes(tag) == bytes);
    // Empty means "no tag", both ways.
    CHECK(pool.intern({}) == 0);
    CHECK(pool.bytes(0).empty());
    // The same bytes give the same handle, which is what makes two stacks with
    // the same tag merge.
    CHECK(pool.intern(bytes) == tag);
}
