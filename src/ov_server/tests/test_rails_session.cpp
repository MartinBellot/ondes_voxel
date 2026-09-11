// Carts on disk: what a save writes, what a load reads, and the entities of
// someone else's chunk that must survive both.
#include "../src/rails_session.hpp"

#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/region_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] std::filesystem::path pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs loaded = [] {
        Packs out;
        if (auto blocks = registry::BlockRegistry::load(pack())) {
            out.blocks = std::move(*blocks);
        }
        if (auto registries = registry::Registries::load(pack())) {
            out.registries = std::move(*registries);
        }
        return out;
    }();
    return loaded;
}

[[nodiscard]] std::filesystem::path scratch(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() / std::string{name};
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void put(nbt::Tag& compound, std::string_view name, nbt::Tag value) {
    compound.compound()->push_back(nbt::CompoundEntry{std::string{name}, std::move(value)});
}

[[nodiscard]] nbt::Tag doubles(f64 a, f64 b, f64 c) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    (void)list.push(nbt::Tag{a});
    (void)list.push(nbt::Tag{b});
    (void)list.push(nbt::Tag{c});
    return list;
}

}  // namespace

TEST_CASE("a furnace cart comes back from disk as it went", "[rails]") {
    if (!packs().blocks || !packs().registries) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const auto dir = scratch("ov_rails_session_roundtrip");

    // A chunk as vanilla writes one: a pig beside a furnace cart.
    nbt::Tag pig = nbt::Tag::make_compound();
    put(pig, "id", nbt::Tag{std::string{"minecraft:pig"}});
    put(pig, "Pos", doubles(3.5, -60.0, 3.5));
    nbt::Tag cart = nbt::Tag::make_compound();
    put(cart, "id", nbt::Tag{std::string{"minecraft:furnace_minecart"}});
    put(cart, "Pos", doubles(5.5, -60.9375, 5.5));
    put(cart, "Motion", doubles(0.1, 0.0, 0.0));
    put(cart, "Fuel", nbt::Tag{i16{1200}});
    put(cart, "PushX", nbt::Tag{1.0});
    put(cart, "PushZ", nbt::Tag{0.0});
    put(cart, "CustomDisplayTile", nbt::Tag::make_bool(true));
    put(cart, "DisplayOffset", nbt::Tag{i32{3}});
    nbt::Tag display = nbt::Tag::make_compound();
    put(display, "Name", nbt::Tag{std::string{"minecraft:stone"}});
    put(cart, "DisplayState", std::move(display));
    nbt::Tag entities = nbt::Tag::make_list(nbt::TagType::Compound);
    (void)entities.push(pig);
    (void)entities.push(cart);
    nbt::Tag root = nbt::Tag::make_compound();
    put(root, "DataVersion", nbt::Tag{i32{3465}});
    put(root, "Position", nbt::Tag{nbt::Tag::IntArray{0, 0}});
    put(root, "Entities", std::move(entities));
    std::filesystem::create_directories(dir / "entities");
    auto writer = nbt::RegionWriter::open_or_empty(dir / "entities" / "r.0.0.mca");
    writer.set_chunk(0, 0, nbt::Document{"", std::move(root)}, 0);
    REQUIRE(writer.write(dir / "entities" / "r.0.0.mca"));

    entity::EntityWorld world{*packs().registries};
    RailsSession        session{*packs().blocks, *packs().registries};
    CHECK(session.load(dir, world) == 1);
    REQUIRE(world.size() == 1);
    const entity::EntityState* state = world.state(world.handles()[0]);
    REQUIRE(state != nullptr);
    CHECK(state->position.x == 5.5);
    CHECK(state->velocity.x == 0.1);
    CHECK(session.owns(state->type));

    // Written back: the cart with its fuel, push and display, and the pig
    // untouched.
    CHECK(session.save(dir, world) == 1);
    const auto region = nbt::RegionFile::open(dir / "entities" / "r.0.0.mca");
    REQUIRE(region);
    const auto chunk = region->read_chunk(0, 0);
    REQUIRE(chunk);
    const nbt::Tag* list = chunk->root.find("Entities");
    REQUIRE(list != nullptr);
    REQUIRE(list->list() != nullptr);
    REQUIRE(list->list()->size() == 2);
    const nbt::Tag& kept    = (*list->list())[0];
    const nbt::Tag& written = (*list->list())[1];
    CHECK(kept.find("id")->as_string() == "minecraft:pig");
    CHECK(written.find("id")->as_string() == "minecraft:furnace_minecart");
    CHECK(written.find("Fuel")->as_i64() == 1200);
    CHECK(written.find("PushX")->as_f64() == 1.0);
    CHECK(written.find("DisplayOffset")->as_i64() == 3);
    CHECK(written.find("DisplayState")->find("Name")->as_string() == "minecraft:stone");
    CHECK(written.find("UUID") != nullptr);
    CHECK(chunk->root.find("DataVersion")->as_i64() == 3465);

    // And read again by a fresh server: the same cart.
    entity::EntityWorld again{*packs().registries};
    RailsSession        second{*packs().blocks, *packs().registries};
    CHECK(second.load(dir, again) == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a cart that rolled away is not left behind", "[rails]") {
    if (!packs().blocks || !packs().registries) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const auto          dir = scratch("ov_rails_session_moved");
    entity::EntityWorld world{*packs().registries};
    RailsSession        session{*packs().blocks, *packs().registries};
    const auto handle = world.spawn("minecraft:minecart", Vec3d{1.5, -60.0, 1.5}, net::Uuid{1, 2});
    REQUIRE(handle);
    session.adopt(world, *handle);
    CHECK(session.save(dir, world) == 1);
    // Into the next region.
    world.mutable_state(*handle)->position = Vec3d{600.5, -60.0, 1.5};
    CHECK(session.save(dir, world) == 1);

    entity::EntityWorld again{*packs().registries};
    RailsSession        second{*packs().blocks, *packs().registries};
    CHECK(second.load(dir, again) == 1);
    std::filesystem::remove_all(dir);
}
