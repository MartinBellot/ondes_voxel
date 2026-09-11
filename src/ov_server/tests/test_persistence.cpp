// ── persistence ── Every entity vanilla saves and this server did not, to and
// from the 1.20.1 entity format, through the one writer of each dimension's
// entities/ (entity_storage.hpp).
//
// The compounds the tests read are the ones the real 1.20.1 server wrote
// (scripts/measure_persistence.py oracle, docs/provenance/persistance-entites.md):
// keys, and types — a `Fuse` short, a `Count` int, a `Time` int, an `Age`
// short on an item and an int on a cloud.
#include "../src/brewing_session.hpp"
#include "../src/dimension_entities.hpp"
#include "../src/end_fight.hpp"
#include "../src/entity_nbt.hpp"
#include "../src/entity_storage.hpp"
#include "../src/ground_entities.hpp"
#include "../src/mob_combat.hpp"
#include "../src/nether_mobs.hpp"
#include "../src/player_data.hpp"
#include "../src/projectiles.hpp"
#include "../src/rails_session.hpp"
#include "../src/tnt_gravity.hpp"
#include "../src/world_ticks.hpp"

#include "ov/gameplay/end_portal.hpp"
#include "ov/gameplay/falling_block.hpp"
#include "ov/gameplay/primed_tnt.hpp"
#include "ov/gameplay/projectile.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/region_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <filesystem>
#include <map>
#include <string>
#include <tuple>
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

[[nodiscard]] const registry::Registries&    registries() { return *packs().registries; }
[[nodiscard]] const registry::BlockRegistry& blocks() { return *packs().blocks; }

[[nodiscard]] std::filesystem::path scratch(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() / std::string{name};
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

[[nodiscard]] nbt::Tag doubles(f64 a, f64 b, f64 c) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    (void)list.push(nbt::Tag{a});
    (void)list.push(nbt::Tag{b});
    (void)list.push(nbt::Tag{c});
    return list;
}

[[nodiscard]] nbt::Tag base(std::string_view id, Vec3d at, Vec3d motion = {}) {
    nbt::Tag out = nbt::Tag::make_compound();
    (void)out.put("id", nbt::Tag{std::string{id}});
    (void)out.put("Pos", doubles(at.x, at.y, at.z));
    (void)out.put("Motion", doubles(motion.x, motion.y, motion.z));
    (void)out.put("UUID", nbt::Tag{nbt::Tag::IntArray{1212967241, -2099360284, -1233664804,
                                                     static_cast<i32>(at.x * 1000.0)}});
    return out;
}

[[nodiscard]] nbt::Tag item_compound(std::string_view id, i8 count) {
    nbt::Tag out = nbt::Tag::make_compound();
    (void)out.put("id", nbt::Tag{std::string{id}});
    (void)out.put("Count", nbt::Tag{count});
    return out;
}

[[nodiscard]] nbt::Tag block_state(std::string_view name,
                                   std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    nbt::Tag out = nbt::Tag::make_compound();
    if (props.size() > 0) {
        nbt::Tag p = nbt::Tag::make_compound();
        for (const auto& [key, value] : props) {
            (void)p.put(std::string{key}, nbt::Tag{std::string{value}});
        }
        (void)out.put("Properties", std::move(p));
    }
    (void)out.put("Name", nbt::Tag{std::string{name}});
    return out;
}

/// Type and value of one key, the way the real server's `data get` reads.
template<typename T>
void check_is(const nbt::Tag& compound, std::string_view key, nbt::TagType type, T value) {
    INFO(key);
    const nbt::Tag* tag = compound.find(key);
    REQUIRE(tag != nullptr);
    CHECK(tag->type() == type);
    if constexpr (std::is_floating_point_v<T>) {
        CHECK(tag->as_f64() == static_cast<f64>(value));
    } else {
        CHECK(tag->as_i64() == static_cast<i64>(value));
    }
}

/// The base keys every entity carries, typed as vanilla types them.
void check_base(const nbt::Tag& compound, i16 fire) {
    check_is(compound, "Fire", nbt::TagType::Short, fire);
    check_is(compound, "Air", nbt::TagType::Short, 300);
    check_is(compound, "Invulnerable", nbt::TagType::Byte, 0);
    check_is(compound, "PortalCooldown", nbt::TagType::Int, 0);
    check_is(compound, "FallDistance", nbt::TagType::Float, 0.0);
    REQUIRE(compound.find("UUID") != nullptr);
    CHECK(compound.find("UUID")->type() == nbt::TagType::IntArray);
    CHECK(compound.find("Pos")->list_element_type() == nbt::TagType::Double);
    CHECK(compound.find("Motion")->list_element_type() == nbt::TagType::Double);
    CHECK(compound.find("Rotation")->list_element_type() == nbt::TagType::Float);
    CHECK(compound.find("OnGround")->type() == nbt::TagType::Byte);
}

[[nodiscard]] usize count_type(const entity::EntityWorld& world, std::string_view name) {
    const auto types = registries().find("minecraft:entity_type");
    const auto id    = registries().protocol_id(*types, name);
    usize      n     = 0;
    for (const entity::EntityHandle handle : world.handles()) {
        n += world.state(handle)->type == static_cast<i32>(*id) ? 1U : 0U;
    }
    return n;
}

/// The items and orbs of one level, and the clock they age by.
struct Ground {
    std::vector<ItemEntity> items;
    std::vector<GroundOrb>  orbs;
    i64                     now{10'000};
    i32                     next_id{500};
    usize                   announced{0};
    GroundHost              host() {
        GroundHost h;
        h.now            = [this] { return now; };
        h.next_entity_id = [this] { return next_id++; };
        h.uuid_for       = [](i32 id) { return net::Uuid{7, static_cast<u64>(id)}; };
        h.announce_item  = [this](const ItemEntity&) { ++announced; };
        h.announce_orb   = [this](const GroundOrb&) { ++announced; };
        return h;
    }
};

}  // namespace

// ── Items and orbs ──────────────────────────────────────────────────────────

TEST_CASE("an item keeps its stack, its age and its delay across a save", "[persistence]") {
    Ground         ground;
    GroundEntities adopter{DimensionId::Overworld, registries(), ground.items, ground.orbs,
                           ground.host()};
    ItemEntity     item;
    item.entity_id    = 3;
    item.uuid         = net::Uuid{1, 2};
    item.x            = 4.5;
    item.y            = -60.0;
    item.z            = 4.5;
    item.stack        = net::ItemStack{registries()
                                           .protocol_id(*registries().find("minecraft:item"),
                                                        "minecraft:diamond")
                                           .value_or(0),
                                       3, {}};
    item.born         = ground.now - 161;
    item.pickup_delay = 40;
    const auto compound = adopter.item_nbt(item, ground.now);
    REQUIRE(compound);
    CHECK(compound->find("id")->as_string() == "minecraft:item");
    check_base(*compound, -1);
    check_is(*compound, "Age", nbt::TagType::Short, 161);
    check_is(*compound, "PickupDelay", nbt::TagType::Short, 40);
    check_is(*compound, "Health", nbt::TagType::Short, 5);  // a dropped stack's (measured)
    const nbt::Tag* stack = compound->find("Item");
    REQUIRE(stack != nullptr);
    CHECK(stack->find("id")->as_string() == "minecraft:diamond");
    check_is(*stack, "Count", nbt::TagType::Byte, 3);

    // Read back an hour of ticks later: the same age, so the same despawn.
    Ground         later;
    later.now = 80'000;
    GroundEntities reader{DimensionId::Overworld, registries(), later.items, later.orbs,
                          later.host()};
    REQUIRE(reader.adopt_saved(*compound));
    REQUIRE(later.items.size() == 1);
    CHECK(later.announced == 1);
    const ItemEntity& back = later.items.front();
    CHECK(later.now - back.born == 161);
    CHECK(back.pickup_delay == 40);
    CHECK(back.stack.count == 3);
    CHECK(back.uuid == net::Uuid{1, 2});
    CHECK(back.x == 4.5);
    CHECK(back.dimension == DimensionId::Overworld);
}

TEST_CASE("the item vanilla wrote is read, and never-aging stays never-aging", "[persistence]") {
    Ground         ground;
    GroundEntities adopter{DimensionId::Nether, registries(), ground.items, ground.orbs,
                           ground.host()};
    // As the real server saved a dirt stack a crater dropped.
    nbt::Tag vanilla = base("minecraft:item", Vec3d{21.94, -63.0, 5.21}, Vec3d{0.0, -0.08, 0.0});
    (void)vanilla.put("Health", nbt::Tag{i16{5}});
    (void)vanilla.put("Age", nbt::Tag{i16{-32768}});
    (void)vanilla.put("PickupDelay", nbt::Tag{i16{32767}});
    (void)vanilla.put("Item", item_compound("minecraft:dirt", 16));
    (void)vanilla.put("Thrower", nbt::Tag{nbt::Tag::IntArray{1, 2, 3, 4}});
    REQUIRE(adopter.adopt_saved(vanilla));
    REQUIRE(ground.items.size() == 1);
    CHECK(ground.items.front().dimension == DimensionId::Nether);
    ground.now += 1'000'000;  // far past five minutes
    CHECK(ground.now - ground.items.front().born < 6000);
    const auto again = adopter.item_nbt(ground.items.front(), ground.now);
    REQUIRE(again);
    check_is(*again, "Age", nbt::TagType::Short, -32768);
    check_is(*again, "PickupDelay", nbt::TagType::Short, 32767);
    // What this server does not model goes back as it came.
    CHECK(again->find("Thrower") != nullptr);

    // An item of something this registry does not have is refused, not guessed.
    nbt::Tag unknown = base("minecraft:item", Vec3d{});
    (void)unknown.put("Item", item_compound("minecraft:not_an_item", 1));
    CHECK_FALSE(adopter.adopt_saved(unknown));
}

TEST_CASE("an orb is a Value short and a Count int, and a big one splits", "[persistence]") {
    Ground         ground;
    GroundEntities adopter{DimensionId::Overworld, registries(), ground.items, ground.orbs,
                           ground.host()};
    GroundOrb orb;
    orb.entity_id = 9;
    orb.x         = 7.5;
    orb.y         = -60.0;
    orb.z         = 4.5;
    orb.value     = 7;
    orb.born      = ground.now - 62;
    ground.orbs.push_back(orb);
    std::vector<LooseEntity> out;
    adopter.save(out);
    REQUIRE(out.size() == 1);
    const nbt::Tag& saved = out.front().compound;
    CHECK(saved.find("id")->as_string() == "minecraft:experience_orb");
    check_base(saved, -1);
    check_is(saved, "Value", nbt::TagType::Short, 7);
    check_is(saved, "Count", nbt::TagType::Int, 1);
    check_is(saved, "Age", nbt::TagType::Short, 62);
    check_is(saved, "Health", nbt::TagType::Short, 5);

    // Count orbs of Value each: the whole of it comes back.
    nbt::Tag three = saved;
    (void)three.put("Count", nbt::Tag{i32{3}});
    ground.orbs.clear();
    REQUIRE(adopter.adopt_saved(three));
    REQUIRE(ground.orbs.size() == 1);
    CHECK(ground.orbs.front().value == 21);
    CHECK(ground.now - ground.orbs.front().born == 62);

    ground.orbs.front().value = 40'000;
    out.clear();
    adopter.save(out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].compound.find("Value")->as_i64() + out[1].compound.find("Value")->as_i64() == 40'000);
}

TEST_CASE("items of another level are not this level's to write", "[persistence]") {
    Ground         ground;
    GroundEntities overworld{DimensionId::Overworld, registries(), ground.items, ground.orbs,
                             ground.host()};
    ItemEntity nether_item;
    nether_item.dimension = DimensionId::Nether;
    nether_item.stack     = net::ItemStack{1, 1, {}};
    ground.items.push_back(nether_item);
    std::vector<LooseEntity> out;
    overworld.save(out);
    CHECK(out.empty());
    std::vector<i32> removed;
    overworld.release([](ChunkPos) { return true; }, out, removed);
    CHECK(out.empty());
    CHECK(ground.items.size() == 1);
}

// ── Through the storage: a chunk of things that are not mobs ────────────────

namespace {

void write_chunk(const std::filesystem::path& dir, i32 cx, i32 cz, std::vector<nbt::Tag> entities) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    for (nbt::Tag& entity : entities) {
        (void)list.push(std::move(entity));
    }
    nbt::Tag root = nbt::Tag::make_compound();
    (void)root.put("DataVersion", nbt::Tag{i32{3465}});
    (void)root.put("Position", nbt::Tag{nbt::Tag::IntArray{cx, cz}});
    (void)root.put("Entities", std::move(list));
    const auto file = dir / fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5);
    std::filesystem::create_directories(file.parent_path());
    auto writer = nbt::RegionWriter::open_or_empty(file);
    writer.set_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31),
                     nbt::Document{"", std::move(root)}, 0);
    REQUIRE(writer.write(file));
}

[[nodiscard]] std::vector<nbt::Tag> read_chunk(const std::filesystem::path& dir, i32 cx, i32 cz) {
    std::vector<nbt::Tag> out;
    const auto file = nbt::RegionFile::open(dir / fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5));
    if (!file || !file->has_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31))) {
        return out;
    }
    const auto chunk = file->read_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31));
    REQUIRE(chunk);
    for (const nbt::Tag& entity : *chunk->root.find("Entities")->list()) {
        out.push_back(entity);
    }
    return out;
}

[[nodiscard]] const nbt::Tag* find_id(const std::vector<nbt::Tag>& list, std::string_view id) {
    for (const nbt::Tag& entity : list) {
        if (entity.find("id")->as_string() == id) {
            return &entity;
        }
    }
    return nullptr;
}

/// One overworld's entity state: the world, the modules that run what is
/// not a mob, the ground, the storage over them.
struct Overworld {
    entity::EntityWorld world{registries()};
    TntGravity          tnt{blocks(), registries(), nullptr, nullptr};
    Projectiles         projectiles{registries(), blocks(), nullptr};
    Brewing             brewing{registries()};
    PotionHost          potion_host;
    Ground              ground;
    GroundEntities      items;
    MobRecords          records;
    EntityStorage       storage;
    EntityStorageHost   host;
    i32                 next_cloud{900};

    explicit Overworld(const std::filesystem::path& dir)
        : items{DimensionId::Overworld, registries(), ground.items, ground.orbs, ground.host()},
          storage{registries(), dir} {
        storage.add_adopter(tnt);
        storage.add_adopter(projectiles);
        storage.add_loose(brewing);
        storage.add_loose(items);
        potion_host.next_entity_id = [this] { return next_cloud++; };
        brewing.set_persistence_host(&potion_host);
        // What the server calls transient (despawn leaves them alone): the
        // storage saves them all the same, since an adopter runs them.
        host.transient = [this](i32 type) { return tnt.owns(type) || projectiles.owns(type); };
    }
};

[[nodiscard]] nbt::Tag vanilla_arrow() {
    // As the real server saved an arrow fired straight down into grass.
    nbt::Tag arrow = base("minecraft:arrow", Vec3d{10.5, -59.94999999925494, 4.5},
                          Vec3d{0.0, -0.8039799858373768, 0.0});
    (void)arrow.put("life", nbt::Tag{i16{58}});
    (void)arrow.put("shake", nbt::Tag{i8{0}});
    (void)arrow.put("inGround", nbt::Tag::make_bool(true));
    (void)arrow.put("inBlockState", block_state("minecraft:grass_block", {{"snowy", "false"}}));
    (void)arrow.put("pickup", nbt::Tag{i8{1}});
    (void)arrow.put("damage", nbt::Tag{2.0});
    (void)arrow.put("crit", nbt::Tag::make_bool(false));
    (void)arrow.put("ShotFromCrossbow", nbt::Tag::make_bool(false));
    (void)arrow.put("PierceLevel", nbt::Tag{i8{0}});
    (void)arrow.put("SoundEvent", nbt::Tag{std::string{"minecraft:entity.arrow.hit"}});
    (void)arrow.put("LeftOwner", nbt::Tag::make_bool(true));
    (void)arrow.put("HasBeenShot", nbt::Tag::make_bool(true));
    return arrow;
}

}  // namespace

TEST_CASE("a TNT, a falling sand, an arrow, a trident, a potion, a cloud, an item: read, run, "
          "written back",
          "[persistence]") {
    const auto dir = scratch("ov_persistence_chunk");
    nbt::Tag   tnt = base("minecraft:tnt", Vec3d{22.5, -60.0, 4.5});
    (void)tnt.put("Fuse", nbt::Tag{i16{334}});
    nbt::Tag sand = base("minecraft:falling_block", Vec3d{25.5, -40.0, 4.5}, Vec3d{0.0, -0.5, 0.0});
    (void)sand.put("BlockState", block_state("minecraft:sand"));
    (void)sand.put("Time", nbt::Tag{i32{17}});
    (void)sand.put("DropItem", nbt::Tag::make_bool(false));
    (void)sand.put("HurtEntities", nbt::Tag::make_bool(false));
    (void)sand.put("FallHurtMax", nbt::Tag{i32{40}});
    nbt::Tag trident = vanilla_arrow();
    (void)trident.put("id", nbt::Tag{std::string{"minecraft:trident"}});
    (void)trident.put("Pos", doubles(16.5, -59.94999999925494, 4.5));
    nbt::Tag trident_item = item_compound("minecraft:trident", 1);
    nbt::Tag damage_tag   = nbt::Tag::make_compound();
    (void)damage_tag.put("Damage", nbt::Tag{i32{3}});
    (void)trident_item.put("tag", std::move(damage_tag));
    (void)trident.put("Trident", std::move(trident_item));
    (void)trident.put("DealtDamage", nbt::Tag::make_bool(true));
    nbt::Tag potion = base("minecraft:potion", Vec3d{9.5, 60.0, 4.5}, Vec3d{0.0, 0.5, 0.0});
    nbt::Tag splash = item_compound("minecraft:splash_potion", 1);
    nbt::Tag potion_tag = nbt::Tag::make_compound();
    (void)potion_tag.put("Potion", nbt::Tag{std::string{"minecraft:poison"}});
    (void)splash.put("tag", std::move(potion_tag));
    (void)potion.put("Item", std::move(splash));
    nbt::Tag cloud = base("minecraft:area_effect_cloud", Vec3d{12.5, -60.0, 4.5});
    (void)cloud.put("Potion", nbt::Tag{std::string{"minecraft:poison"}});
    (void)cloud.put("Duration", nbt::Tag{i32{6000}});
    (void)cloud.put("Radius", nbt::Tag{3.0F});
    (void)cloud.put("Age", nbt::Tag{i32{67}});
    nbt::Tag item = base("minecraft:item", Vec3d{4.5, -60.0, 4.5});
    (void)item.put("Age", nbt::Tag{i16{100}});
    (void)item.put("Item", item_compound("minecraft:diamond", 3));
    write_chunk(dir, 0, 0, {tnt, sand, vanilla_arrow(), potion, cloud, item});
    write_chunk(dir, 1, 0, {trident});

    {
        Overworld  server{dir};
        const auto first  = server.storage.load_chunk(ChunkPos{0, 0}, server.world, server.records,
                                                      server.host);
        const auto second = server.storage.load_chunk(ChunkPos{1, 0}, server.world, server.records,
                                                      server.host);
        CHECK(first.adopted == 6);
        CHECK(first.carried == 0);
        CHECK(second.adopted == 1);
        CHECK(count_type(server.world, "minecraft:tnt") == 1);
        CHECK(count_type(server.world, "minecraft:falling_block") == 1);
        CHECK(count_type(server.world, "minecraft:arrow") == 1);
        CHECK(count_type(server.world, "minecraft:trident") == 1);
        CHECK(count_type(server.world, "minecraft:potion") == 1);
        CHECK(server.brewing.clouds() == 1);
        CHECK(server.ground.items.size() == 1);

        // The TNT burns on from 334, the sand from 17, the arrow stays stuck.
        for (const entity::EntityHandle handle : server.world.handles()) {
            const entity::IEntityLogic* logic = server.world.logic(handle);
            if (const auto* fuse = dynamic_cast<const gameplay::PrimedTntLogic*>(logic)) {
                CHECK(fuse->fuse() == 334);
            } else if (const auto* falling = dynamic_cast<const gameplay::FallingBlockLogic*>(logic)) {
                CHECK(falling->time() == 17);
                CHECK(blocks().block_name(blocks().block_of(falling->state())) == "minecraft:sand");
            } else if (const auto* shot = dynamic_cast<const gameplay::ProjectileLogic*>(logic)) {
                const gameplay::ProjectileData& data = shot->data();
                if (data.kind == gameplay::ProjectileKind::Arrow) {
                    CHECK(data.in_ground);
                    CHECK(data.pickup == 1);
                    CHECK(data.life == 58);
                    // The superflat's grass block is at -61; its top face at -60.
                    CHECK(data.stuck == BlockPos{10, -61, 4});
                    CHECK(blocks().block_name(blocks().block_of(data.stuck_state)) ==
                          "minecraft:grass_block");
                } else if (data.kind == gameplay::ProjectileKind::Trident) {
                    CHECK(data.in_ground);
                    CHECK(data.dealt_damage);
                } else {
                    CHECK(data.kind == gameplay::ProjectileKind::Potion);
                    CHECK_FALSE(data.in_ground);
                }
            }
        }
        const auto saved = server.storage.save_all(server.world, server.records, server.host);
        CHECK(saved.entities == 7);
    }

    // Each written where it stands: the TNT (x 22.5) and the sand (x 25.5)
    // were read from chunk (0,0) and stand in (1,0), beside the trident.
    const auto chunk = read_chunk(dir, 0, 0);
    const auto next  = read_chunk(dir, 1, 0);
    CHECK(chunk.size() == 4);
    CHECK(next.size() == 3);
    const nbt::Tag* tnt_back = find_id(next, "minecraft:tnt");
    REQUIRE(tnt_back != nullptr);
    check_base(*tnt_back, -1);
    check_is(*tnt_back, "Fuse", nbt::TagType::Short, 334);
    const nbt::Tag* sand_back = find_id(next, "minecraft:falling_block");
    REQUIRE(sand_back != nullptr);
    check_base(*sand_back, 0);
    check_is(*sand_back, "Time", nbt::TagType::Int, 17);
    check_is(*sand_back, "DropItem", nbt::TagType::Byte, 0);  // carried, not reset
    check_is(*sand_back, "FallHurtMax", nbt::TagType::Int, 40);
    CHECK(sand_back->find("BlockState")->find("Name")->as_string() == "minecraft:sand");
    const nbt::Tag* arrow_back = find_id(chunk, "minecraft:arrow");
    REQUIRE(arrow_back != nullptr);
    check_base(*arrow_back, 0);
    check_is(*arrow_back, "life", nbt::TagType::Short, 58);
    check_is(*arrow_back, "inGround", nbt::TagType::Byte, 1);
    check_is(*arrow_back, "pickup", nbt::TagType::Byte, 1);
    check_is(*arrow_back, "damage", nbt::TagType::Double, 2.0);
    check_is(*arrow_back, "PierceLevel", nbt::TagType::Byte, 0);
    CHECK(arrow_back->find("inBlockState")->find("Name")->as_string() == "minecraft:grass_block");
    CHECK(arrow_back->find("inBlockState")->find("Properties")->find("snowy")->as_string() ==
          "false");
    const nbt::Tag* potion_back = find_id(chunk, "minecraft:potion");
    REQUIRE(potion_back != nullptr);
    CHECK(potion_back->find("Item")->find("tag")->find("Potion")->as_string() == "minecraft:poison");
    const nbt::Tag* cloud_back = find_id(chunk, "minecraft:area_effect_cloud");
    REQUIRE(cloud_back != nullptr);
    check_is(*cloud_back, "Duration", nbt::TagType::Int, 6000);
    check_is(*cloud_back, "Age", nbt::TagType::Int, 67);
    check_is(*cloud_back, "Radius", nbt::TagType::Float, 3.0);
    CHECK(cloud_back->find("Potion")->as_string() == "minecraft:poison");
    const nbt::Tag* item_back = find_id(chunk, "minecraft:item");
    REQUIRE(item_back != nullptr);
    CHECK(item_back->find("Item")->find("id")->as_string() == "minecraft:diamond");

    const nbt::Tag* trident_back = find_id(next, "minecraft:trident");
    REQUIRE(trident_back != nullptr);
    check_is(*trident_back, "DealtDamage", nbt::TagType::Byte, 1);
    CHECK(trident_back->find("Trident")->find("tag")->find("Damage")->as_i64() == 3);

    // A restart reads each of them once.
    Overworld restart{dir};
    (void)restart.storage.load_chunk(ChunkPos{0, 0}, restart.world, restart.records, restart.host);
    (void)restart.storage.load_chunk(ChunkPos{1, 0}, restart.world, restart.records, restart.host);
    CHECK(restart.world.size() == 5);
    CHECK(restart.brewing.clouds() == 1);
    CHECK(restart.ground.items.size() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a chunk's unload takes its items and TNT to disk and out of the world",
          "[persistence]") {
    const auto dir = scratch("ov_persistence_unload");
    Overworld  server{dir};
    for (const ChunkPos chunk : {ChunkPos{0, 0}, ChunkPos{5, 0}}) {
        (void)server.storage.load_chunk(chunk, server.world, server.records, server.host);
    }
    nbt::Tag tnt = base("minecraft:tnt", Vec3d{2.5, -60.0, 2.5});
    (void)tnt.put("Fuse", nbt::Tag{i16{80}});
    REQUIRE(server.tnt.adopt_saved(server.world, tnt));
    ItemEntity near_item;
    near_item.entity_id = 11;
    near_item.x         = 3.5;
    near_item.z         = 3.5;
    near_item.stack     = net::ItemStack{1, 1, {}};
    near_item.born      = server.ground.now;
    server.ground.items.push_back(near_item);
    ItemEntity far_item = near_item;
    far_item.entity_id  = 12;
    far_item.x          = 88.5;  // chunk 5
    server.ground.items.push_back(far_item);

    std::vector<i32>          removed;
    const std::array<ChunkPos, 1> leaving{ChunkPos{0, 0}};
    const auto stats = server.storage.unload_chunks(leaving, server.world, server.records,
                                                    server.host, removed);
    CHECK(stats.entities == 2);
    CHECK(server.world.size() == 0);
    REQUIRE(server.ground.items.size() == 1);
    CHECK(server.ground.items.front().entity_id == 12);
    CHECK(std::ranges::find(removed, 11) != removed.end());
    const auto chunk = read_chunk(dir, 0, 0);
    CHECK(chunk.size() == 2);
    CHECK(find_id(chunk, "minecraft:tnt") != nullptr);
    CHECK(find_id(chunk, "minecraft:item") != nullptr);
    std::filesystem::remove_all(dir);
}

// ── The Nether's mobs ───────────────────────────────────────────────────────

namespace {

constexpr std::array<std::string_view, 5> kNetherBiomes{
    "minecraft:basalt_deltas", "minecraft:crimson_forest", "minecraft:nether_wastes",
    "minecraft:soul_sand_valley", "minecraft:warped_forest"};

[[nodiscard]] std::filesystem::path generated_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated";
}

}  // namespace

TEST_CASE("the Nether's mobs go to DIM-1/entities with what they hold, and come back",
          "[persistence][nether]") {
    if (!std::filesystem::is_directory(generated_root() / "data" / "minecraft" / "loot_tables")) {
        SKIP("generated data absent");
    }
    const auto dir = scratch("ov_persistence_nether");
    MobCombat  combat{registries(), nullptr};
    NetherMobHost host;
    host.block_at    = [](BlockPos) { return registry::kAirState; };
    host.loaded      = [](BlockPos) { return true; };
    host.ticking     = [](ChunkPos) { return false; };
    host.biome_at    = [](BlockPos) -> u16 { return 2; };
    host.block_light = [](BlockPos) -> u8 { return 0; };
    host.players     = [](std::vector<NetherPlayer>&) {};
    host.broadcast   = [](i32, std::span<const u8>) {};
    {
        NetherMobs mobs{registries(), blocks(), &combat, nullptr, kNetherBiomes, generated_root(), 42};
        DimensionEntities nether{registries(), dir / "DIM-1" / "entities", &mobs.world(),
                                 mobs.storage_host(host)};
        const std::array<ChunkPos, 1> home{ChunkPos{0, 0}};
        (void)nether.storage().load_chunk(home[0], mobs.world(), *std::make_unique<MobRecords>(),
                                          mobs.storage_host(host));
        REQUIRE(mobs.summon("minecraft:zombified_piglin", Vec3d{2.5, 65.0, 2.5}, host));
        REQUIRE(mobs.summon("minecraft:magma_cube", Vec3d{5.5, 65.0, 2.5}, host));
        REQUIRE(mobs.summon("minecraft:ghast", Vec3d{8.5, 80.0, 2.5}, host));
        const auto saved = nether.save();
        CHECK(saved.entities == 3);
    }
    const auto chunk = read_chunk(dir / "DIM-1" / "entities", 0, 0);
    REQUIRE(chunk.size() == 3);
    const nbt::Tag* zombified = find_id(chunk, "minecraft:zombified_piglin");
    REQUIRE(zombified != nullptr);
    check_base(*zombified, -1);
    check_is(*zombified, "Health", nbt::TagType::Float, 20.0);
    check_is(*zombified, "AngerTime", nbt::TagType::Int, 0);
    CHECK(zombified->find("HandItems")->list()->front().find("id")->as_string() ==
          "minecraft:golden_sword");
    const nbt::Tag* magma = find_id(chunk, "minecraft:magma_cube");
    REQUIRE(magma != nullptr);
    CHECK(magma->find("Size")->type() == nbt::TagType::Int);
    const i64 size = magma->find("Size")->as_i64();
    CHECK(find_id(chunk, "minecraft:ghast")->find("ExplosionPower")->as_i64() == 1);

    // A restart: the three come back into the Nether's world, the magma cube
    // at its size, and none of the players' stand-ins was ever written.
    NetherMobs        mobs{registries(), blocks(), &combat, nullptr, kNetherBiomes, generated_root(), 42};
    DimensionEntities nether{registries(), dir / "DIM-1" / "entities", &mobs.world(),
                             mobs.storage_host(host)};
    const auto read = nether.storage().load_chunk(ChunkPos{0, 0}, mobs.world(),
                                                  *std::make_unique<MobRecords>(),
                                                  mobs.storage_host(host));
    CHECK(read.entities == 3);
    CHECK(mobs.size() == 3);
    const nbt::Tag* magma_again = nullptr;
    const auto      again_chunk = [&] {
        (void)nether.save();
        return read_chunk(dir / "DIM-1" / "entities", 0, 0);
    }();
    magma_again = find_id(again_chunk, "minecraft:magma_cube");
    REQUIRE(magma_again != nullptr);
    CHECK(magma_again->find("Size")->as_i64() == size);
    std::filesystem::remove_all(dir);
}

// ── The End: the dragon and the crystals ───────────────────────────────────

namespace {

class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find({pos.x, pos.y, pos.z});
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return world::WorldShape::the_end(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {false, false}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }
    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[{pos.x, pos.y, pos.z}] = state;
    }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

private:
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells_;
};

[[nodiscard]] std::vector<registry::BlockId> block_ids(
    std::initializer_list<std::string_view> names) {
    std::vector<registry::BlockId> out;
    for (const std::string_view name : names) {
        if (const auto block = blocks().find_block(name)) {
            out.push_back(*block);
        }
    }
    return out;
}

[[nodiscard]] EndFight make_fight(const gameplay::EndPortalRules& rules) {
    static const auto immune      = block_ids({"minecraft:bedrock", "minecraft:obsidian",
                                               "minecraft:end_stone", "minecraft:iron_bars"});
    static const auto transparent = block_ids({"minecraft:fire", "minecraft:light"});
    return EndFight{rules, blocks(), 1234567890, EndFightTypes{}, immune, transparent};
}

struct EndHost {
    i32          next{100};
    EndFightHost host;
    EndHost() {
        host.broadcast          = [](i32, std::span<const u8>) {};
        host.reserve_entity_ids = [this](i32 count) {
            const i32 first = next;
            next += count;
            return first;
        };
        host.players = [](std::vector<EndFightPlayer>&) {};
    }
};

}  // namespace

TEST_CASE("the dragon comes back with its health, and a destroyed crystal does not",
          "[persistence][end]") {
    const gameplay::EndPortalRules rules{blocks()};
    REQUIRE(rules.valid());
    const auto dir = scratch("ov_persistence_end");
    nbt::Tag   fight_saved;
    {
        MapLevel level;
        EndHost  end;
        EndFight fight = make_fight(rules);
        fight.set_host(&end.host);
        const auto stone = blocks().default_state(*blocks().find_block("minecraft:end_stone"));
        for (i32 y = 0; y < 62; ++y) {
            level.set_block({0, y, 0}, stone);
        }
        fight.start(level, 62, end.host);
        REQUIRE(fight.crystals_alive() == 10);
        REQUIRE(fight.dragon() != nullptr);
        fight.dragon()->set_health(137.0F);
        // One crystal goes, by /kill: it must not come back.
        std::vector<EndFightEntity> seen;
        fight.entities(seen);
        i32 crystal = 0;
        for (const EndFightEntity& e : seen) {
            if (e.type == "minecraft:end_crystal") {
                crystal = e.id;
                break;
            }
        }
        REQUIRE(fight.kill(crystal, end.host));
        REQUIRE(fight.crystals_alive() == 9);

        const auto dragon = fight.dragon_nbt();
        REQUIRE(dragon);
        check_base(*dragon, 0);
        check_is(*dragon, "Health", nbt::TagType::Float, 137.0);
        CHECK(dragon->find("DragonPhase")->type() == nbt::TagType::Int);
        check_is(*dragon, "DragonDeathTime", nbt::TagType::Int, 0);
        CHECK(dragon->find("Brain") != nullptr);

        DimensionEntities storage{registries(), dir / "DIM1" / "entities", nullptr, {}};
        storage.storage().add_loose(fight);
        const auto saved = storage.save();
        CHECK(saved.entities == 10);  // the dragon and nine crystals
        fight_saved = fight.save();
    }
    CHECK(fight_saved.find("NeedsStateScanning")->as_i64() == 0);

    // A restart: level.dat's fight is read, and the fight does not start again
    // — the dragon and the crystals are on disk.
    EndHost  end;
    EndFight fight = make_fight(rules);
    fight.set_host(&end.host);
    fight.load(fight_saved);
    CHECK(fight.started());
    CHECK(fight.awaiting_dragon());
    DimensionEntities storage{registries(), dir / "DIM1" / "entities", nullptr, {}};
    storage.storage().add_loose(fight);
    for (i32 cx = -4; cx <= 3; ++cx) {
        for (i32 cz = -4; cz <= 3; ++cz) {
            (void)storage.storage().load_chunk(ChunkPos{cx, cz}, *std::make_unique<entity::EntityWorld>(registries()),
                                               *std::make_unique<MobRecords>(), EntityStorageHost{});
        }
    }
    REQUIRE(fight.dragon() != nullptr);
    CHECK(fight.dragon()->health() == 137.0F);
    CHECK_FALSE(fight.awaiting_dragon());
    CHECK(fight.crystals_alive() == 9);
    // A second read of the same crystal is the same crystal.
    std::vector<LooseEntity> out;
    fight.save(out);
    CHECK(out.size() == 10);
    CHECK(fight.adopt_saved(out.back().compound));
    CHECK(fight.crystals_alive() == 9);
    std::filesystem::remove_all(dir);
}

// ── RootVehicle ─────────────────────────────────────────────────────────────

TEST_CASE("a player's RootVehicle is written while they ride and taken out when they do not",
          "[persistence][player]") {
    const ItemNames names{&registries(), registries().find("minecraft:item")};
    const net::Uuid uuid{0x1234, 0x5678};
    PlayerRecord    record;
    nbt::Tag        cart = base("minecraft:minecart", Vec3d{60.5, -60.0, -8.5});
    nbt::Tag        root = nbt::Tag::make_compound();
    (void)root.put("Attach", *cart.find("UUID"));
    (void)root.put("Entity", cart);
    record.root_vehicle = root;
    const nbt::Tag written = write_player(record, nullptr, uuid, names);
    REQUIRE(written.find("RootVehicle") != nullptr);
    CHECK(written.find("RootVehicle")->find("Entity")->find("id")->as_string() == "minecraft:minecart");
    const auto read = read_player(written, &uuid, names, "test");
    REQUIRE(read);
    REQUIRE(read->record.root_vehicle);
    CHECK(read->record.root_vehicle->find("Attach")->type() == nbt::TagType::IntArray);

    record.root_vehicle.reset();
    const nbt::Tag after = write_player(record, &written, uuid, names);
    CHECK(after.find("RootVehicle") == nullptr);
}

TEST_CASE("a player leaving in a cart takes it; coming back, they ride it again",
          "[persistence][rails]") {
    entity::EntityWorld world{registries()};
    RailsSession        rails{blocks(), registries()};
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells;
    LevelHooks hooks;
    hooks.block_at = [&](BlockPos p) {
        const auto found = cells.find({p.x, p.y, p.z});
        return found == cells.end() ? registry::kAirState : found->second;
    };
    hooks.is_loaded = [](BlockPos) { return true; };
    hooks.set_block = [&](BlockPos p, registry::BlockStateId s) { cells[{p.x, p.y, p.z}] = s; };
    ServerLevel level{blocks(), std::move(hooks)};
    WorldTicks  ticks{blocks(), registries()};
    bool        ready = false;
    RailsHost   host;
    host.broadcast    = [](i32, std::span<const u8>) {};
    host.drop_item    = [](Vec3d, std::string_view, i32) {};
    host.carry_rider  = [](i32, Vec3d) { return true; };
    host.set_down     = [](i32, Vec3d) {};
    host.player_ready = [&](i32) { return ready; };

    nbt::Tag cart = base("minecraft:minecart", Vec3d{60.5, -60.0, -8.5});
    nbt::Tag root = nbt::Tag::make_compound();
    (void)root.put("Attach", *cart.find("UUID"));
    (void)root.put("Entity", cart);
    rails.request_restore(7, root);
    // Not in the world yet: the cart waits for its rider.
    rails.before_entity_tick(world, level, ticks, host);
    CHECK(world.size() == 0);
    ready = true;
    rails.before_entity_tick(world, level, ticks, host);
    REQUIRE(world.size() == 1);
    const i32 cart_id = rails.vehicle_of(7);
    CHECK(cart_id >= 0);
    CHECK(world.state(world.find(cart_id))->uuid == uuid_from(cart.find("UUID")));

    // They leave: the cart goes with them, in their RootVehicle.
    const auto taken = rails.take_vehicle(world, 7);
    REQUIRE(taken);
    CHECK(taken->cart_id == cart_id);
    CHECK(world.size() == 0);
    CHECK(rails.vehicle_of(7) == -1);
    CHECK(uuid_from(taken->root_vehicle.find("Attach")) == uuid_from(cart.find("UUID")));
    CHECK(taken->root_vehicle.find("Entity")->find("id")->as_string() == "minecraft:minecart");
    CHECK_FALSE(rails.take_vehicle(world, 7));

    // And back: one cart, the same, ridden.
    rails.request_restore(8, taken->root_vehicle);
    rails.before_entity_tick(world, level, ticks, host);
    CHECK(world.size() == 1);
    CHECK(rails.vehicle_of(8) >= 0);
}
