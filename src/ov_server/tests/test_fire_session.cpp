// The fire's server half: the campfire grill in a real chunk, what an entity's
// box touches, and where rain falls. The rules themselves are tested in
// ov_gameplay (test_fire.cpp); this is the wiring they depend on.
#include "../src/campfire.hpp"
#include "../src/fire_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<gameplay::RecipeBook>    recipes;
    std::optional<registry::RegistryId>    items;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        Loaded out;
        auto   blocks = registry::BlockRegistry::load(pack_path());
        auto   regs   = registry::Registries::load(pack_path());
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
            out.recipes.emplace(*out.registries);
            out.items = out.registries->find("minecraft:item");
        }
        return out;
    }();
    return state;
}

[[nodiscard]] const registry::BlockRegistry& blocks() { return *loaded().blocks; }

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks().default_state(*id);
    for (const auto& [key, value] : props) {
        const auto property = blocks().find_property(*id, key);
        REQUIRE(property.has_value());
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks().with_property(state, *property, static_cast<u16>(i));
            }
        }
    }
    return state;
}

[[nodiscard]] registry::ProtocolId item(std::string_view name) {
    const auto id = loaded().registries->protocol_id(*loaded().items, name);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] world::Chunk make_chunk() {
    return world::Chunk{ChunkPos{0, 0}, world::WorldShape::overworld(),
                        world::AirStates::from(blocks()), &blocks()};
}

struct Grill {
    world::Chunk                                                chunk = make_chunk();
    std::vector<std::pair<Vec3d, net::ItemStack>>               dropped;
    usize                                                       sent{0};
    CampfireHost                                                host;

    Grill() {
        host.each_chunk = [this](const std::function<void(world::Chunk&)>& visit) {
            visit(chunk);
        };
        host.drop_item   = [this](Vec3d at, const net::ItemStack& stack) {
            dropped.emplace_back(at, stack);
        };
        host.send_entity = [this](BlockPos, const world::BlockEntity&) { ++sent; };
        host.mark_dirty  = [](i32, i32) {};
    }
};

constexpr BlockPos kGrill{1, -60, 1};

}  // namespace

TEST_CASE("campfire: raw food goes on the grill with its recipe's time", "[fire][campfire]") {
    REQUIRE(loaded().recipes.has_value());
    Campfires campfires{blocks(), *loaded().registries, *loaded().recipes};
    Grill     grill;
    grill.chunk.set_block(1, -60, 1, state_of("minecraft:campfire"));

    // Every campfire recipe in 1.20.1 cooks for 600 ticks.
    REQUIRE(campfires.cook_time(item("minecraft:beef")) == 600);
    CHECK_FALSE(campfires.cook_time(item("minecraft:stone")).has_value());
    CHECK_FALSE(campfires.place_food(grill.chunk, kGrill, item("minecraft:stone"), grill.host));

    for (int i = 0; i < 4; ++i) {
        REQUIRE(campfires.place_food(grill.chunk, kGrill, item("minecraft:beef"), grill.host));
    }
    // Four slots, and the fifth is refused.
    CHECK_FALSE(campfires.place_food(grill.chunk, kGrill, item("minecraft:beef"), grill.host));
    CHECK(grill.sent == 4);

    const world::BlockEntity* entity = grill.chunk.block_entity_at(1, -60, 1);
    REQUIRE(entity != nullptr);
    CHECK(entity->type == "minecraft:campfire");
    const nbt::Tag* items = entity->data.find("Items");
    REQUIRE(items != nullptr);
    REQUIRE(items->list() != nullptr);
    CHECK(items->list()->size() == 4);
    CHECK(items->list()->front().find("id")->as_string() == "minecraft:beef");
    const auto* totals = entity->data.find("CookingTotalTimes")->get_if<nbt::Tag::IntArray>();
    REQUIRE(totals != nullptr);
    CHECK((*totals)[0] == 600);
}

TEST_CASE("campfire: a lit grill drops the steak on tick 600 and empties", "[fire][campfire]") {
    Campfires campfires{blocks(), *loaded().registries, *loaded().recipes};
    Grill     grill;
    grill.chunk.set_block(1, -60, 1, state_of("minecraft:campfire"));
    REQUIRE(campfires.place_food(grill.chunk, kGrill, item("minecraft:beef"), grill.host));

    int cooked_at = -1;
    for (int t = 1; t <= 700 && cooked_at < 0; ++t) {
        (void)campfires.tick(grill.host);
        if (!grill.dropped.empty()) {
            cooked_at = t;
        }
    }
    CHECK(cooked_at == 600);
    REQUIRE(grill.dropped.size() == 1);
    CHECK(grill.dropped[0].second.item_id == item("minecraft:cooked_beef"));
    const nbt::Tag* items = grill.chunk.block_entity_at(1, -60, 1)->data.find("Items");
    CHECK(items->list()->empty());
}

TEST_CASE("campfire: an unlit grill cools by two a tick", "[fire][campfire]") {
    Campfires campfires{blocks(), *loaded().registries, *loaded().recipes};
    Grill     grill;
    grill.chunk.set_block(1, -60, 1, state_of("minecraft:campfire"));
    REQUIRE(campfires.place_food(grill.chunk, kGrill, item("minecraft:beef"), grill.host));
    for (int t = 0; t < 100; ++t) {
        (void)campfires.tick(grill.host);
    }
    // Put out the way the server does it: `Chunk::set_block` drops any block
    // entity on a change, and the server's `write_block` keeps it when the
    // block stays the same and only a property moves — a shovel on a campfire
    // leaves the food on the grill, as in vanilla. The test goes through that
    // rule rather than around it.
    {
        const world::BlockEntity kept = *grill.chunk.block_entity_at(1, -60, 1);
        grill.chunk.set_block(1, -60, 1, state_of("minecraft:campfire", {{"lit", "false"}}));
        grill.chunk.set_block_entity(kept);
    }
    for (int t = 0; t < 10; ++t) {
        (void)campfires.tick(grill.host);
    }
    const auto* times = grill.chunk.block_entity_at(1, -60, 1)
                            ->data.find("CookingTimes")
                            ->get_if<nbt::Tag::IntArray>();
    REQUIRE(times != nullptr);
    CHECK((*times)[0] == 80);
    CHECK(grill.dropped.empty());
}

namespace {

struct World {
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells;
    i32                                                         top{-61};
    i32                                                         biome{0};

    [[nodiscard]] FireHost host() {
        FireHost out;
        out.block_at = [this](BlockPos pos) {
            const auto found = cells.find({pos.x, pos.y, pos.z});
            return found == cells.end() ? registry::kAirState : found->second;
        };
        out.biome_at    = [this](BlockPos) { return biome; };
        out.rain_top    = [this](i32, i32) { return top; };
        out.sky_light   = [](BlockPos) { return u8{15}; };
        out.block_light = [](BlockPos) { return u8{0}; };
        out.prime_tnt   = [](BlockPos) {};
        return out;
    }
};

}  // namespace

TEST_CASE("fire session: what a box touches", "[fire]") {
    World       world;
    FireSession session{blocks(), *loaded().registries, world.host(), nullptr};
    const Vec3d feet{0.5, -60.0, 0.5};

    world.cells[{0, -60, 0}] = state_of("minecraft:fire");
    CHECK(session.contact(feet, 0.9, 1.4).in_fire);

    world.cells[{0, -60, 0}] = state_of("minecraft:soul_fire");
    CHECK(session.contact(feet, 0.9, 1.4).in_soul_fire);

    world.cells[{0, -60, 0}] = state_of("minecraft:lava");
    CHECK(session.contact(feet, 0.9, 1.4).in_lava);
    // A flowing lava of level 7 stands 1/9 high: a box whose floor is half a
    // block up is above its surface and not in it.
    world.cells[{0, -60, 0}] = state_of("minecraft:lava", {{"level", "7"}});
    CHECK_FALSE(session.contact(Vec3d{0.5, -59.5, 0.5}, 0.9, 1.4).in_lava);
    CHECK(session.contact(feet, 0.9, 1.4).in_lava);

    world.cells[{0, -60, 0}] = state_of("minecraft:water");
    const gameplay::FireContact wet = session.contact(feet, 0.9, 1.4);
    CHECK(wet.in_water);
    CHECK(wet.wet);

    world.cells[{0, -60, 0}] = state_of("minecraft:campfire");
    CHECK(session.contact(feet, 0.9, 1.4).campfire == 1);
    world.cells[{0, -60, 0}] = state_of("minecraft:soul_campfire");
    CHECK(session.contact(feet, 0.9, 1.4).campfire == 2);
    world.cells[{0, -60, 0}] = state_of("minecraft:campfire", {{"lit", "false"}});
    CHECK(session.contact(feet, 0.9, 1.4).campfire == 0);

    CHECK(session.ticks_randomly(state_of("minecraft:lava")));
    CHECK_FALSE(session.ticks_randomly(state_of("minecraft:water")));
}

TEST_CASE("fire session: rain falls on the open sky of a biome where it rains", "[fire]") {
    World world;
    const auto plains = blocks().find_biome("minecraft:plains");
    const auto desert = blocks().find_biome("minecraft:desert");
    REQUIRE(plains.has_value());
    REQUIRE(desert.has_value());
    world.biome = static_cast<i32>(*plains);
    FireSession session{blocks(), *loaded().registries, world.host(), nullptr};
    const BlockPos at{0, -60, 0};

    session.set_world(FireWorld{.fire_tick = true, .raining = false});
    CHECK_FALSE(session.is_raining_at(at));

    session.set_world(FireWorld{.fire_tick = true, .raining = true});
    CHECK(session.is_raining_at(at));

    world.top = -50;  // a roof: the column's surface is above the fire
    CHECK_FALSE(session.is_raining_at(at));

    world.top   = -61;
    world.biome = static_cast<i32>(*desert);  // no precipitation
    CHECK_FALSE(session.is_raining_at(at));
}
