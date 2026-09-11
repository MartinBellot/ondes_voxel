// ── mobs-3 ── The server half of the third mob wave: the Anvil entity files,
// despawn, and the zombie villager's rise and cure (docs/provenance/mobs-3.md).
#include "../src/entity_storage.hpp"
#include "../src/mob_despawn.hpp"
#include "../src/zombie_villagers.hpp"

#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] i32 type_id(std::string_view name) {
    const auto types = registries()->find("minecraft:entity_type");
    return registries()->protocol_id(*types, name).value_or(-1);
}

/// A directory of its own under the system's temporary directory, removed
/// when the test is done.
struct ScratchDir {
    std::filesystem::path path;
    ScratchDir() {
        std::random_device device;
        path = std::filesystem::temp_directory_path() /
               ("ov_mobs3_" + std::to_string(device()) + "_" + std::to_string(device()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    ScratchDir(const ScratchDir&)            = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

/// The server's side of a load, reduced to what a test can hold.
struct Host {
    entity::EntityWorld*                      world{nullptr};
    std::unordered_map<i32, i32>              slimes;
    std::unordered_map<i32, bool>             creepers;
    std::unordered_map<i32, gameplay::VillagerState> zombies;
    std::unordered_map<i32, i32>              conversions;
    usize                                     announced{0};

    [[nodiscard]] EntityStorageHost make() {
        EntityStorageHost host;
        host.attach = [this](entity::EntityHandle handle, std::string_view type) {
            const entity::EntityState* state = world->state(handle);
            if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
                world->set_logic(handle, std::make_unique<gameplay::Mob>(
                                             *kind, state->width, state->height,
                                             state->network_id, type_id("minecraft:player")));
            } else {
                world->set_logic(handle, std::make_unique<gameplay::FallingMob>());
            }
        };
        host.announce       = [this](const entity::EntityState&) { ++announced; };
        host.slime_size     = [this](i32 id) { return slimes.contains(id) ? slimes[id] : 1; };
        host.set_slime_size = [this](entity::EntityState& state, i32 size) {
            slimes[state.network_id] = size;
        };
        host.creeper_powered = [this](i32 id) { return creepers.contains(id) && creepers[id]; };
        host.charge_creeper  = [this](i32 id) { creepers[id] = true; };
        host.zombie_villager = [this](i32 id) { return &zombies[id]; };
        host.conversion_time = [this](i32 id) {
            return conversions.contains(id) ? conversions[id] : -1;
        };
        host.set_conversion_time = [this](i32 id, i32 ticks) { conversions[id] = ticks; };
        return host;
    }
};

entity::EntityHandle spawn_mob(entity::EntityWorld& world, const EntityStorageHost& host,
                               std::string_view type, Vec3d at) {
    const auto handle = world.spawn(type, at, net::Uuid{0x1234'5678'9ABC'DEF0ULL,
                                                        static_cast<u64>(at.x * 1000.0)});
    REQUIRE(handle.has_value());
    host.attach(*handle, type);
    return *handle;
}

}  // namespace

TEST_CASE("mobs-3: mobs go to entities/ and come back", "[server][mobs3][anvil]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    ScratchDir scratch;
    Host       holder;
    entity::EntityWorld world{*registries()};
    holder.world                  = &world;
    const EntityStorageHost host  = holder.make();
    MobRecords          records;
    EntityStorage       storage{*registries(), scratch.path / "entities"};

    const auto zombie = spawn_mob(world, host, "minecraft:zombie", Vec3d{3.5, 64.0, 5.5});
    records.at(world.state(zombie)->network_id).custom_name = R"({"text":"Bob"})";
    records.pin(world.state(zombie)->network_id);
    const auto sheep = spawn_mob(world, host, "minecraft:sheep", Vec3d{7.25, 64.0, 2.75});
    {
        auto& animal   = dynamic_cast<gameplay::Mob*>(world.logic(sheep))->mutable_brain().animal;
        animal.colour  = 3;
        animal.sheared = true;
    }
    const auto villager = spawn_mob(world, host, "minecraft:villager", Vec3d{20.5, 64.0, 20.5});
    {
        auto& v        = dynamic_cast<gameplay::Mob*>(world.logic(villager))->mutable_brain().villager;
        v.type         = gameplay::VillagerType::Desert;
        v.profession   = gameplay::Profession::Librarian;
        v.level        = 3;
        v.xp           = 20;
        v.offers_drawn = true;
        gameplay::MerchantOffer glass;
        glass.cost_a   = gameplay::TradeItem{.item = "minecraft:emerald", .count = 1};
        glass.result   = gameplay::TradeItem{.item = "minecraft:glass", .count = 4};
        glass.max_uses = 12;
        glass.xp       = 10;
        glass.uses     = 5;
        glass.demand   = 2;
        v.offers.push_back(glass);
    }
    const auto slime = spawn_mob(world, host, "minecraft:slime", Vec3d{-3.5, 64.0, -3.5});
    holder.slimes[world.state(slime)->network_id] = 4;
    const auto creeper = spawn_mob(world, host, "minecraft:creeper", Vec3d{-20.5, 64.0, 3.5});
    holder.creepers[world.state(creeper)->network_id] = true;
    const net::Uuid zombie_uuid = world.state(zombie)->uuid;

    const EntityStorageStats saved = storage.save_all(world, records, host);
    CHECK(saved.entities == 5);

    // The file, as vanilla reads it: DataVersion, Position, Entities.
    const auto file = nbt::RegionFile::open(scratch.path / "entities" / "r.0.0.mca");
    REQUIRE(file.has_value());
    const auto chunk = file->read_chunk(0, 0);
    REQUIRE(chunk.has_value());
    CHECK(chunk->root.find("DataVersion")->as_i64() == world::kDataVersion1201);
    const auto* position = chunk->root.find("Position")->get_if<nbt::Tag::IntArray>();
    REQUIRE(position != nullptr);
    CHECK(*position == nbt::Tag::IntArray{0, 0});
    const nbt::Tag* entities = chunk->root.find("Entities");
    REQUIRE(entities != nullptr);
    REQUIRE(entities->list()->size() == 2);  // the zombie and the sheep
    const nbt::Tag& first = entities->list()->front();
    CHECK(first.find("id")->as_string() == "minecraft:zombie");
    CHECK(first.find("CustomName")->as_string() == R"({"text":"Bob"})");
    CHECK(first.find("PersistenceRequired")->as_bool());
    CHECK(first.find("UUID")->get_if<nbt::Tag::IntArray>()->size() == 4);
    CHECK(first.find("DrownedConversionTime")->as_i64() == -1);

    // A second world reads it all back.
    entity::EntityWorld again{*registries()};
    Host                holder2;
    holder2.world                  = &again;
    const EntityStorageHost host2  = holder2.make();
    MobRecords          records2;
    EntityStorage       storage2{*registries(), scratch.path / "entities"};
    usize               read = 0;
    for (const ChunkPos pos : {ChunkPos{0, 0}, ChunkPos{1, 1}, ChunkPos{-1, -1}, ChunkPos{-2, 0}}) {
        read += storage2.load_chunk(pos, again, records2, host2).entities;
    }
    CHECK(read == 5);
    CHECK(holder2.announced == 5);
    bool seen_zombie = false, seen_sheep = false, seen_villager = false, seen_slime = false,
         seen_creeper = false;
    for (const entity::EntityHandle handle : again.handles()) {
        const entity::EntityState* state = again.state(handle);
        const auto*                mob   = dynamic_cast<gameplay::Mob*>(again.logic(handle));
        if (state->type == type_id("minecraft:zombie")) {
            seen_zombie = true;
            CHECK(state->uuid == zombie_uuid);
            CHECK(state->position.x == 3.5);
            CHECK(records2.find(state->network_id)->persistence_required);
            CHECK(records2.find(state->network_id)->custom_name == R"({"text":"Bob"})");
        } else if (state->type == type_id("minecraft:sheep")) {
            seen_sheep = true;
            CHECK(mob->brain().animal.colour == 3);
            CHECK(mob->brain().animal.sheared);
            CHECK(state->position.z == 2.75);
        } else if (state->type == type_id("minecraft:villager")) {
            seen_villager = true;
            const auto& v = mob->brain().villager;
            CHECK(v.profession == gameplay::Profession::Librarian);
            CHECK(v.type == gameplay::VillagerType::Desert);
            CHECK(v.level == 3);
            CHECK(v.xp == 20);
            REQUIRE(v.offers.size() == 1);
            CHECK(v.offers[0].result.item == "minecraft:glass");
            CHECK(v.offers[0].result.count == 4);
            CHECK(v.offers[0].uses == 5);
            CHECK(v.offers[0].demand == 2);
        } else if (state->type == type_id("minecraft:slime")) {
            seen_slime = true;
            CHECK(holder2.slimes[state->network_id] == 4);
        } else if (state->type == type_id("minecraft:creeper")) {
            seen_creeper = true;
            CHECK(holder2.creepers[state->network_id]);
        }
    }
    CHECK((seen_zombie && seen_sheep && seen_villager && seen_slime && seen_creeper));
}

TEST_CASE("mobs-3: an unloaded chunk takes its mobs to disk and gives them back",
          "[server][mobs3][anvil]") {
    if (registries() == nullptr) {
        return;
    }
    ScratchDir          scratch;
    Host                holder;
    entity::EntityWorld world{*registries()};
    holder.world                 = &world;
    const EntityStorageHost host = holder.make();
    MobRecords              records;
    EntityStorage           storage{*registries(), scratch.path / "entities"};
    (void)storage.load_chunk(ChunkPos{2, 2}, world, records, host);  // nothing on disk yet
    CHECK(storage.is_loaded(ChunkPos{2, 2}));
    (void)spawn_mob(world, host, "minecraft:cow", Vec3d{40.5, 64.0, 40.5});
    (void)spawn_mob(world, host, "minecraft:cow", Vec3d{41.5, 64.0, 42.5});
    (void)spawn_mob(world, host, "minecraft:zombie", Vec3d{100.5, 64.0, 100.5});  // elsewhere

    std::vector<i32> removed;
    const std::array<ChunkPos, 1> leaving{ChunkPos{2, 2}};
    const EntityStorageStats gone = storage.unload_chunks(leaving, world, records, host, removed);
    CHECK(gone.entities == 2);
    CHECK(removed.size() == 2);
    CHECK(world.size() == 1);
    CHECK_FALSE(storage.is_loaded(ChunkPos{2, 2}));

    const EntityStorageStats back = storage.load_chunk(ChunkPos{2, 2}, world, records, host);
    CHECK(back.entities == 2);
    CHECK(world.size() == 3);
}

TEST_CASE("mobs-3: a world the real server wrote, read by this one", "[server][mobs3][parity]") {
    if (registries() == nullptr) {
        return;
    }
    // Written by scripts/measure_mobs3.py `anvil` (24 NoAI mobs with species
    // NBT) and kept, untracked, under .scratch/. Absent on a fresh checkout.
    const auto entities =
        std::filesystem::path{OV_SOURCE_DIR} / ".scratch" / "mobs3-anvil-vanilla" / "entities";
    if (!std::filesystem::exists(entities)) {
        WARN("no vanilla zoo at " << entities.string() << " — run measure_mobs3.py anvil");
        return;
    }
    Host                holder;
    entity::EntityWorld world{*registries()};
    holder.world                 = &world;
    const EntityStorageHost host = holder.make();
    MobRecords              records;
    EntityStorage           storage{*registries(), entities};
    usize                   read = 0;
    for (i32 x = -2; x <= 3; ++x) {
        for (i32 z = -2; z <= 3; ++z) {
            read += storage.load_chunk(ChunkPos{x, z}, world, records, host).entities;
        }
    }
    // The copy was taken after the other campaigns had run in the same world,
    // so it holds their leftovers too (every one read, 38): the zoo is picked
    // out by the tag it was summoned with, which comes back out untouched.
    CHECK(read >= 24);
    usize zoo     = 0;
    usize checked = 0;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        const auto*                mob   = dynamic_cast<gameplay::Mob*>(world.logic(handle));
        const nbt::Tag             out   = storage.encode(world, handle, records, host);
        const nbt::Tag*            tags  = out.find("Tags");
        const bool in_zoo = tags != nullptr && tags->list() != nullptr &&
                            std::ranges::any_of(*tags->list(), [](const nbt::Tag& tag) {
                                return tag.as_string() == "zoo";
                            });
        if (!in_zoo) {
            continue;
        }
        ++zoo;
        CHECK(records.find(state->network_id)->persistence_required);
        if (state->type == type_id("minecraft:villager")) {
            const auto& v = mob->brain().villager;
            CHECK(v.profession == gameplay::Profession::Librarian);
            CHECK(v.type == gameplay::VillagerType::Desert);
            CHECK(v.level == 3);
            CHECK(v.xp == 20);
            CHECK(v.offers.size() == 2);
            ++checked;
        } else if (state->type == type_id("minecraft:zombie_villager")) {
            const auto& kept = holder.zombies[state->network_id];
            CHECK(kept.profession == gameplay::Profession::Farmer);
            CHECK(kept.type == gameplay::VillagerType::Taiga);
            CHECK(kept.level == 2);
            CHECK(kept.xp == 12);
            ++checked;
        } else if (state->type == type_id("minecraft:sheep")) {
            CHECK(mob->brain().animal.colour == 3);
            CHECK(mob->brain().animal.sheared);
            ++checked;
        } else if (state->type == type_id("minecraft:slime")) {
            CHECK(holder.slimes[state->network_id] == 2);
            ++checked;
        } else if (state->type == type_id("minecraft:creeper")) {
            CHECK(holder.creepers[state->network_id]);
            CHECK(out.find("Fuse")->as_i64() == 40);  // carried through, not reset to 30
            ++checked;
        } else if (state->type == type_id("minecraft:wolf")) {
            // Not modelled here, and not lost: the owner goes back out.
            const auto* owner = out.find("Owner")->get_if<nbt::Tag::IntArray>();
            REQUIRE(owner != nullptr);
            CHECK(*owner == nbt::Tag::IntArray{1, 2, 3, 4});
            ++checked;
        }
    }
    CHECK(zoo == 24);
    CHECK(checked == 6);
}

TEST_CASE("mobs-3: despawn by distance, and what never despawns", "[server][mobs3][despawn]") {
    if (registries() == nullptr) {
        return;
    }
    Host                holder;
    entity::EntityWorld world{*registries()};
    holder.world                 = &world;
    const EntityStorageHost host = holder.make();
    MobRecords              records;
    MobDespawn              despawn{*registries()};
    const std::array<Vec3d, 1> players{Vec3d{0.5, 64.0, 0.5}};

    const auto far       = spawn_mob(world, host, "minecraft:zombie", Vec3d{150.5, 64.0, 0.5});
    const auto pinned    = spawn_mob(world, host, "minecraft:zombie", Vec3d{150.5, 64.0, 5.5});
    const auto named     = spawn_mob(world, host, "minecraft:zombie", Vec3d{150.5, 64.0, 9.5});
    const auto near      = spawn_mob(world, host, "minecraft:zombie", Vec3d{16.5, 64.0, 0.5});
    const auto middling  = spawn_mob(world, host, "minecraft:zombie", Vec3d{64.5, 64.0, 0.5});
    const auto villager  = spawn_mob(world, host, "minecraft:villager", Vec3d{150.5, 64.0, 20.5});
    const auto cow       = spawn_mob(world, host, "minecraft:cow", Vec3d{150.5, 64.0, 30.5});
    records.pin(world.state(pinned)->network_id);
    records.at(world.state(named)->network_id).custom_name = R"({"text":"Bob"})";

    const auto none = [](i32) { return false; };
    (void)despawn.tick(world, records, players, gameplay::Difficulty::Normal, none);
    // Measured: the zombie at 140 went at once, the named one too (a name
    // alone does not pin), the persistent one and the cows stayed.
    CHECK(world.state(far)->removed);
    CHECK_FALSE(world.state(pinned)->removed);
    CHECK(world.state(named)->removed);
    CHECK_FALSE(world.state(cow)->removed);
    CHECK_FALSE(world.state(near)->removed);
    CHECK_FALSE(world.state(middling)->removed);
    CHECK_FALSE(world.state(villager)->removed);

    // Between 32 and 128: nothing for the first 600 idle ticks, then one in
    // 800 a tick — gone well within 20 000.
    i64 removed_at = -1;
    for (i64 tick = 1; tick < 20000 && removed_at < 0; ++tick) {
        (void)despawn.tick(world, records, players, gameplay::Difficulty::Normal, none);
        if (world.state(middling)->removed) {
            removed_at = tick;
        }
    }
    CHECK(removed_at > 600);
    CHECK_FALSE(world.state(near)->removed);

    // No player at all: nothing is measured, nothing goes.
    entity::EntityWorld lonely{*registries()};
    holder.world = &lonely;
    const auto alone = spawn_mob(lonely, host, "minecraft:zombie", Vec3d{500.5, 64.0, 0.5});
    (void)despawn.tick(lonely, records, {}, gameplay::Difficulty::Normal, none);
    CHECK_FALSE(lonely.state(alone)->removed);

    // Peaceful takes every monster, pinned or not.
    (void)despawn.tick(world, records, players, gameplay::Difficulty::Peaceful, none);
    CHECK(world.state(pinned)->removed);
    CHECK(world.state(near)->removed);
    CHECK_FALSE(world.state(villager)->removed);
    CHECK_FALSE(world.state(cow)->removed);
}

TEST_CASE("mobs-3: a villager a zombie kills rises, keeping itself, and is cured",
          "[server][mobs3][villager]") {
    if (registries() == nullptr) {
        return;
    }
    Host                holder;
    entity::EntityWorld world{*registries()};
    holder.world                 = &world;
    const EntityStorageHost host = holder.make();
    ZombieVillagers         zombies{*registries()};
    usize                   announced = 0;
    std::string_view        held      = "minecraft:golden_apple";
    usize                   eaten     = 0;
    ZombieVillagerHost      zhost;
    zhost.create_mob = [&](std::string_view type, Vec3d at) {
        return spawn_mob(world, host, type, at);
    };
    zhost.announce     = [&](const entity::EntityState&) { ++announced; };
    zhost.held         = [&](i32) { return held; };
    zhost.creative     = [](i32) { return false; };
    zhost.consume_held = [&](i32) { ++eaten; };
    zhost.speeds_cure  = [](BlockPos) { return false; };
    const AttackDeliver deliver = [](i32, std::span<const u8>) {};

    const auto make_villager = [&](Vec3d at) {
        const auto handle = spawn_mob(world, host, "minecraft:villager", at);
        auto& v = dynamic_cast<gameplay::Mob*>(world.logic(handle))->mutable_brain().villager;
        v.profession   = gameplay::Profession::Armorer;
        v.type         = gameplay::VillagerType::Savanna;
        v.level        = 4;
        v.xp           = 160;
        v.offers_drawn = true;
        gameplay::MerchantOffer coal;
        coal.cost_a = gameplay::TradeItem{.item = "minecraft:coal", .count = 15};
        coal.result = gameplay::TradeItem{.item = "minecraft:emerald", .count = 1};
        v.offers.push_back(coal);
        return handle;
    };
    const auto kill_of = [&](entity::EntityHandle victim) {
        const entity::EntityState* state = world.state(victim);
        return MobKill{state->network_id, victim, "minecraft:villager", "minecraft:zombie",
                       state->position, 0.0F};
    };

    // Easy: never (measured 0/10). A skeleton's kill: never.
    CHECK_FALSE(zombies.on_villager_killed(world, kill_of(make_villager({0.5, 64, 0.5})),
                                           gameplay::Difficulty::Easy, zhost));
    MobKill by_skeleton = kill_of(make_villager({1.5, 64, 0.5}));
    by_skeleton.attacker_type = "minecraft:skeleton";
    CHECK_FALSE(zombies.on_villager_killed(world, by_skeleton, gameplay::Difficulty::Hard, zhost));
    // Normal: about half (measured 37/90).
    usize rose = 0;
    for (i32 i = 0; i < 200; ++i) {
        rose += zombies.on_villager_killed(world, kill_of(make_villager({2.5, 64, 0.5})),
                                           gameplay::Difficulty::Normal, zhost)
                    ? 1U
                    : 0U;
    }
    CHECK(rose > 70);
    CHECK(rose < 130);

    // Hard: always (measured 10/10), and the risen one is the same villager.
    const auto victim = make_villager({30.5, 64.0, 30.5});
    REQUIRE(zombies.on_villager_killed(world, kill_of(victim), gameplay::Difficulty::Hard, zhost));
    entity::EntityHandle risen = entity::kNoEntity;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state->type == type_id("minecraft:zombie_villager") && state->position.x == 30.5) {
            risen = handle;
        }
    }
    REQUIRE(risen != entity::kNoEntity);
    const i32 risen_id = world.state(risen)->network_id;
    const gameplay::VillagerState* kept = zombies.kept(risen_id);
    CHECK(kept->profession == gameplay::Profession::Armorer);
    CHECK(kept->type == gameplay::VillagerType::Savanna);
    CHECK(kept->level == 4);
    CHECK(kept->xp == 160);
    CHECK(kept->offers.size() == 1);

    // A golden apple without Weakness: nothing.
    zombies.queue_interact(1, risen_id);
    (void)zombies.tick(world, zhost, deliver);
    CHECK(zombies.conversion_time(risen_id) == -1);
    CHECK(eaten == 0);
    // Under Weakness: the cure starts, 3600 to 6000 ticks.
    zombies.weaken(risen_id, 1800);
    zombies.queue_interact(1, risen_id);
    (void)zombies.tick(world, zhost, deliver);
    const i32 conversion = zombies.conversion_time(risen_id);
    CHECK(conversion >= 3600);
    CHECK(conversion <= 6000);
    CHECK(eaten == 1);

    // It runs down, one a tick with nothing near, and a villager stands there.
    usize cured = 0;
    for (i32 tick = 0; tick < 6100 && cured == 0; ++tick) {
        cured = zombies.tick(world, zhost, deliver).cured;
    }
    REQUIRE(cured == 1);
    CHECK(world.state(risen)->removed);
    bool found = false;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state->type == type_id("minecraft:villager") && state->position.x == 30.5 &&
            !state->removed) {
            const auto& v = dynamic_cast<gameplay::Mob*>(world.logic(handle))->brain().villager;
            found = v.profession == gameplay::Profession::Armorer && v.level == 4 && v.xp == 160 &&
                    v.offers.size() == 1 && v.type == gameplay::VillagerType::Savanna;
        }
    }
    CHECK(found);
}
