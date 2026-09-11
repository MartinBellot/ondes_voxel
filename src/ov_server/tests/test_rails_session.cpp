// Carts on disk, through the one writer of entities/: the entity storage reads
// and writes the files, the rails session runs the carts it is handed
// (entity_storage.hpp, EntityAdopter). What must hold: a chunk holding a mob,
// a cart and an entity nobody here runs comes back whole; a cart that crossed
// a chunk border is written where it is and nowhere else; a chunk holding
// only a cart is written, and unloading it takes the cart out of the world;
// a restart reads each cart once.
#include "../src/entity_storage.hpp"
#include "../src/rails_session.hpp"

#include "ov/gameplay/mob_logic.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/region_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

[[nodiscard]] nbt::Tag entity_at(std::string_view id, f64 x, f64 y, f64 z) {
    nbt::Tag out = nbt::Tag::make_compound();
    put(out, "id", nbt::Tag{std::string{id}});
    put(out, "Pos", doubles(x, y, z));
    return out;
}

/// Write one chunk of entities/ as vanilla would.
void write_chunk(const std::filesystem::path& dir, i32 cx, i32 cz, std::vector<nbt::Tag> entities) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    for (nbt::Tag& entity : entities) {
        (void)list.push(std::move(entity));
    }
    nbt::Tag root = nbt::Tag::make_compound();
    put(root, "DataVersion", nbt::Tag{i32{3465}});
    put(root, "Position", nbt::Tag{nbt::Tag::IntArray{cx, cz}});
    put(root, "Entities", std::move(list));
    const auto file = dir / "entities" / fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5);
    std::filesystem::create_directories(file.parent_path());
    auto writer = nbt::RegionWriter::open_or_empty(file);
    writer.set_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31),
                     nbt::Document{"", std::move(root)}, 0);
    REQUIRE(writer.write(file));
}

/// The `id` of every entity in one chunk of entities/, in file order. Empty
/// when the chunk has no entry.
[[nodiscard]] std::vector<std::string> ids_in(const std::filesystem::path& dir, i32 cx, i32 cz,
                                              std::vector<nbt::Tag>* compounds = nullptr) {
    std::vector<std::string> out;
    const auto file = nbt::RegionFile::open(dir / "entities" /
                                            fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5));
    if (!file || !file->has_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31))) {
        return out;
    }
    const auto chunk = file->read_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31));
    REQUIRE(chunk);
    CHECK(chunk->root.find("DataVersion")->as_i64() == 3465);
    const nbt::Tag* list = chunk->root.find("Entities");
    REQUIRE(list != nullptr);
    REQUIRE(list->list() != nullptr);
    for (const nbt::Tag& entity : *list->list()) {
        out.emplace_back(entity.find("id")->as_string());
        if (compounds != nullptr) {
            compounds->push_back(entity);
        }
    }
    return out;
}

/// One server's worth of entity state: its world, its rails, its storage.
struct Server {
    entity::EntityWorld world{*packs().registries};
    RailsSession        rails{*packs().blocks, *packs().registries};
    MobRecords          records;
    EntityStorage       storage;
    EntityStorageHost   host;

    explicit Server(const std::filesystem::path& dir)
        : storage{*packs().registries, dir / "entities"} {
        storage.add_adopter(rails);
        host.attach = [this](entity::EntityHandle handle, std::string_view type) {
            const entity::EntityState* state = world.state(handle);
            if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
                world.set_logic(handle, std::make_unique<gameplay::Mob>(
                                            *kind, state->width, state->height,
                                            state->network_id, gameplay::kNoQuarry));
            } else {
                world.set_logic(handle, std::make_unique<gameplay::FallingMob>());
            }
        };
    }

    entity::EntityHandle cart(Vec3d at, u64 salt) {
        const auto handle = world.spawn("minecraft:minecart", at, net::Uuid{7, salt});
        REQUIRE(handle);
        rails.adopt(world, *handle);
        return *handle;
    }

    entity::EntityHandle mob(std::string_view type, Vec3d at, u64 salt) {
        const auto handle = world.spawn(type, at, net::Uuid{9, salt});
        REQUIRE(handle);
        host.attach(*handle, type);
        return *handle;
    }

    [[nodiscard]] usize count(std::string_view type) const {
        const auto types = packs().registries->find("minecraft:entity_type");
        const auto id    = packs().registries->protocol_id(*types, type);
        usize      n     = 0;
        for (const entity::EntityHandle handle : world.handles()) {
            n += world.state(handle)->type == static_cast<i32>(*id) ? 1U : 0U;
        }
        return n;
    }
};

}  // namespace

TEST_CASE("a mob, a furnace cart and a painting share a chunk, and all come back",
          "[rails][entities]") {
    if (!packs().blocks || !packs().registries) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const auto dir = scratch("ov_rails_one_writer_roundtrip");

    // A chunk as vanilla writes one: a pig, a furnace cart, a painting.
    nbt::Tag cart = entity_at("minecraft:furnace_minecart", 5.5, -60.9375, 5.5);
    put(cart, "Motion", doubles(0.1, 0.0, 0.0));
    put(cart, "Fuel", nbt::Tag{i16{1200}});
    put(cart, "PushX", nbt::Tag{1.0});
    put(cart, "PushZ", nbt::Tag{0.0});
    put(cart, "CustomDisplayTile", nbt::Tag::make_bool(true));
    put(cart, "DisplayOffset", nbt::Tag{i32{3}});
    nbt::Tag display = nbt::Tag::make_compound();
    put(display, "Name", nbt::Tag{std::string{"minecraft:stone"}});
    put(cart, "DisplayState", std::move(display));
    nbt::Tag painting = entity_at("minecraft:painting", 8.5, -59.5, 0.03125);
    put(painting, "variant", nbt::Tag{std::string{"minecraft:kebab"}});
    write_chunk(dir, 0, 0,
                {entity_at("minecraft:pig", 3.5, -60.0, 3.5), std::move(cart), std::move(painting)});

    {
        Server server{dir};
        const EntityStorageStats read =
            server.storage.load_chunk(ChunkPos{0, 0}, server.world, server.records, server.host);
        CHECK(read.entities == 2);  // the pig, and the cart the rails run
        CHECK(read.adopted == 1);
        CHECK(read.carried == 1);   // the painting
        REQUIRE(server.world.size() == 2);
        CHECK(server.count("minecraft:furnace_minecart") == 1);
        CHECK(server.count("minecraft:pig") == 1);

        // Two saves in a row write one of each, not two.
        (void)server.storage.save_all(server.world, server.records, server.host);
        const EntityStorageStats saved =
            server.storage.save_all(server.world, server.records, server.host);
        CHECK(saved.entities == 2);
        CHECK(saved.adopted == 1);
    }

    std::vector<nbt::Tag> compounds;
    const auto            ids = ids_in(dir, 0, 0, &compounds);
    REQUIRE(ids.size() == 3);
    CHECK(std::ranges::count(ids, "minecraft:pig") == 1);
    CHECK(std::ranges::count(ids, "minecraft:furnace_minecart") == 1);
    CHECK(std::ranges::count(ids, "minecraft:painting") == 1);
    for (const nbt::Tag& entity : compounds) {
        if (entity.find("id")->as_string() == "minecraft:furnace_minecart") {
            CHECK(entity.find("Fuel")->as_i64() == 1200);
            CHECK(entity.find("PushX")->as_f64() == 1.0);
            CHECK(entity.find("DisplayOffset")->as_i64() == 3);
            CHECK(entity.find("DisplayState")->find("Name")->as_string() == "minecraft:stone");
            CHECK(entity.find("UUID") != nullptr);
        } else if (entity.find("id")->as_string() == "minecraft:painting") {
            CHECK(entity.find("variant")->as_string() == "minecraft:kebab");  // untouched
        }
    }

    // A restart: one pig and one cart, the cart still burning.
    Server again{dir};
    (void)again.storage.load_chunk(ChunkPos{0, 0}, again.world, again.records, again.host);
    CHECK(again.world.size() == 2);
    CHECK(again.count("minecraft:furnace_minecart") == 1);
    CHECK(again.count("minecraft:pig") == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a cart that crossed into another chunk is written there, and only there",
          "[rails][entities]") {
    if (!packs().blocks || !packs().registries) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const auto dir = scratch("ov_rails_one_writer_moved");
    {
        Server server{dir};
        (void)server.storage.load_chunk(ChunkPos{0, 0}, server.world, server.records, server.host);
        // The case the two writers lost: a cart in a chunk that also holds a mob.
        (void)server.mob("minecraft:cow", Vec3d{3.5, -60.0, 3.5}, 1);
        const entity::EntityHandle cart = server.cart(Vec3d{1.5, -60.0, 1.5}, 2);
        (void)server.storage.save_all(server.world, server.records, server.host);
        CHECK(ids_in(dir, 0, 0).size() == 2);

        // Into chunk 1,0, whose file was never read: it is read, then written.
        server.world.mutable_state(cart)->position = Vec3d{20.5, -60.0, 1.5};
        (void)server.storage.save_all(server.world, server.records, server.host);
        CHECK(ids_in(dir, 0, 0) == std::vector<std::string>{"minecraft:cow"});
        CHECK(ids_in(dir, 1, 0) == std::vector<std::string>{"minecraft:minecart"});

        // And on into the next region.
        server.world.mutable_state(cart)->position = Vec3d{600.5, -60.0, 1.5};
        (void)server.storage.save_all(server.world, server.records, server.host);
        CHECK(ids_in(dir, 1, 0).empty());
        CHECK(ids_in(dir, 37, 0) == std::vector<std::string>{"minecraft:minecart"});
    }

    // A restart reading every chunk it ever stood in finds it once.
    Server again{dir};
    for (const ChunkPos pos : {ChunkPos{0, 0}, ChunkPos{1, 0}, ChunkPos{37, 0}}) {
        (void)again.storage.load_chunk(pos, again.world, again.records, again.host);
    }
    CHECK(again.count("minecraft:minecart") == 1);
    CHECK(again.count("minecraft:cow") == 1);
    const entity::EntityState* state = nullptr;
    for (const entity::EntityHandle handle : again.world.handles()) {
        if (again.rails.owns(again.world.state(handle)->type)) {
            state = again.world.state(handle);
        }
    }
    REQUIRE(state != nullptr);
    CHECK(state->position.x == 600.5);
    CHECK(state->uuid == net::Uuid{7, 2});
    std::filesystem::remove_all(dir);
}

TEST_CASE("a chunk holding only a cart is written, and its unload takes the cart away",
          "[rails][entities]") {
    if (!packs().blocks || !packs().registries) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const auto dir = scratch("ov_rails_one_writer_unload");
    Server     server{dir};
    (void)server.storage.load_chunk(ChunkPos{2, 2}, server.world, server.records, server.host);
    const entity::EntityHandle cart = server.cart(Vec3d{40.5, -60.0, 40.5}, 3);
    const i32                  id   = server.world.state(cart)->network_id;
    (void)server.mob("minecraft:cow", Vec3d{100.5, -60.0, 100.5}, 4);  // elsewhere

    std::vector<i32>              removed;
    const std::array<ChunkPos, 1> leaving{ChunkPos{2, 2}};
    const EntityStorageStats      gone =
        server.storage.unload_chunks(leaving, server.world, server.records, server.host, removed);
    CHECK(gone.entities == 1);
    CHECK(gone.adopted == 1);
    CHECK(removed == std::vector<i32>{id});
    CHECK(server.world.size() == 1);  // the cow stays
    CHECK_FALSE(server.storage.is_loaded(ChunkPos{2, 2}));
    CHECK(ids_in(dir, 2, 2) == std::vector<std::string>{"minecraft:minecart"});

    // The chunk comes back, and the cart with it — once.
    const EntityStorageStats back =
        server.storage.load_chunk(ChunkPos{2, 2}, server.world, server.records, server.host);
    CHECK(back.adopted == 1);
    CHECK(server.count("minecraft:minecart") == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a save does not erase a chunk whose file was not read yet", "[rails][entities]") {
    if (!packs().blocks || !packs().registries) {
        SUCCEED("no registry.ovpack");
        return;
    }
    const auto dir = scratch("ov_rails_one_writer_unread");
    write_chunk(dir, 5, 5, {entity_at("minecraft:painting", 88.5, -59.5, 80.03125)});
    {
        Server server{dir};
        // A cart rolls into chunk 5,5 before the storage has read it.
        (void)server.cart(Vec3d{85.5, -60.0, 85.5}, 5);
        (void)server.storage.save_all(server.world, server.records, server.host);
        CHECK(server.storage.is_loaded(ChunkPos{5, 5}));
    }
    const auto ids = ids_in(dir, 5, 5);
    CHECK(ids.size() == 2);
    CHECK(std::ranges::count(ids, "minecraft:painting") == 1);
    CHECK(std::ranges::count(ids, "minecraft:minecart") == 1);
    std::filesystem::remove_all(dir);
}
