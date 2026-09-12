// ── brains ── Villager life on the server: gossip, memories and pockets on
// disk at the vanilla keys (and a vanilla villager's read back), a trading
// screen priced for its player, the type a biome gives, and the births,
// golems and witnesses `VillagerLife` finishes. docs/provenance/cerveaux.md.
#include "../src/entity_storage.hpp"
#include "../src/merchant_session.hpp"
#include "../src/villager_life.hpp"

#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/brain/memory.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/trading.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

using namespace ov;
using namespace ov::server;
namespace b = ov::gameplay::brain;

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

struct ScratchDir {
    std::filesystem::path path;
    ScratchDir() {
        std::random_device device;
        path = std::filesystem::temp_directory_path() /
               ("ov_brains_" + std::to_string(device()) + "_" + std::to_string(device()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    ScratchDir(const ScratchDir&)            = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

[[nodiscard]] EntityStorageHost storage_host(entity::EntityWorld& world) {
    EntityStorageHost host;
    host.attach = [&world](entity::EntityHandle handle, std::string_view type) {
        const entity::EntityState* state = world.state(handle);
        if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
            world.set_logic(handle, std::make_unique<gameplay::Mob>(*kind, state->width,
                                                                    state->height,
                                                                    state->network_id));
        }
    };
    host.announce = [](const entity::EntityState&) {};
    return host;
}

entity::EntityHandle spawn(entity::EntityWorld& world, std::string_view type, Vec3d at) {
    const auto handle = world.spawn(type, at, net::Uuid{0x4F56'0000ULL, static_cast<u64>(at.x * 100.0)});
    REQUIRE(handle.has_value());
    const entity::EntityState* state = world.state(*handle);
    world.set_logic(*handle, std::make_unique<gameplay::Mob>(*gameplay::mob_kind(type), state->width,
                                                             state->height, state->network_id));
    return *handle;
}

[[nodiscard]] gameplay::MobBrain& brain(entity::EntityWorld& world, entity::EntityHandle h) {
    return dynamic_cast<gameplay::Mob*>(world.logic(h))->mutable_brain();
}

// The probe's UUID on the real server, as `data get entity ovhand UUID` read it.
const net::Uuid kProbe{(static_cast<u64>(static_cast<u32>(289470649)) << 32) |
                           static_cast<u32>(-236178987),
                       (static_cast<u64>(static_cast<u32>(-1494576147)) << 32) |
                           static_cast<u32>(376949766)};

}  // namespace

TEST_CASE("brains: gossip, memories and pockets go to entities/ and come back",
          "[server][brains][anvil]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    ScratchDir          scratch;
    entity::EntityWorld world{*registries()};
    const auto          host = storage_host(world);
    MobRecords          records;
    EntityStorage       storage{*registries(), scratch.path / "entities"};

    const auto villager = spawn(world, "minecraft:villager", Vec3d{20.5, 64.0, 20.5});
    {
        gameplay::MobBrain& mind = brain(world, villager);
        auto&               v    = mind.villager;
        v.gossips.put(b::GossipEntry{kProbe, b::GossipType::MinorNegative, 30});
        v.gossips.put(b::GossipEntry{net::Uuid{1, 2}, b::GossipType::Trading, 7});
        v.last_gossip_decay = 1234;
        v.inventory[0]      = {"minecraft:bread", 3};
        v.food_level        = 4;
        mind.memories.set(b::MemoryType::Home, b::MemoryValue::of_pos(BlockPos{21, 64, 23}));
        mind.memories.set(b::MemoryType::JobSite, b::MemoryValue::of_pos(BlockPos{25, 64, 20}));
        mind.memories.set(b::MemoryType::LastSlept, b::MemoryValue::of_number(999));
        mind.memories.set(b::MemoryType::GolemDetectedRecently, b::MemoryValue::unit(), 300);
        mind.memories.set(b::MemoryType::WalkTarget, b::MemoryValue::of_pos(BlockPos{0, 0, 0}));
    }
    (void)storage.save_all(world, records, host);

    const auto file = nbt::RegionFile::open(scratch.path / "entities" / "r.0.0.mca");
    REQUIRE(file.has_value());
    const auto chunk = file->read_chunk(1, 1);
    REQUIRE(chunk.has_value());
    const nbt::Tag& saved = chunk->root.find("Entities")->list()->front();
    const nbt::Tag* gossips = saved.find("Gossips");
    REQUIRE(gossips != nullptr);
    REQUIRE(gossips->list()->size() == 2);
    const nbt::Tag& g = gossips->list()->front();
    CHECK(g.find("Type")->as_string() == "minor_negative");
    CHECK(g.find("Value")->as_i64() == 30);
    CHECK(g.find("Target")->get_if<nbt::Tag::IntArray>()->size() == 4);
    const nbt::Tag* memories = saved.find("Brain")->find("memories");
    REQUIRE(memories != nullptr);
    const nbt::Tag* home = memories->find("minecraft:home");
    REQUIRE(home != nullptr);
    CHECK(*home->find("value")->find("pos")->get_if<nbt::Tag::IntArray>() ==
          nbt::Tag::IntArray{21, 64, 23});
    CHECK(home->find("value")->find("dimension")->as_string() == "minecraft:overworld");
    CHECK(memories->find("minecraft:last_slept")->find("value")->as_i64() == 999);
    CHECK(memories->find("minecraft:golem_detected_recently")->find("ttl")->as_i64() == 300);
    CHECK_FALSE(memories->contains("minecraft:walk_target"));  // never saved
    CHECK(saved.find("Inventory")->list()->front().find("id")->as_string() == "minecraft:bread");
    CHECK(saved.find("Inventory")->list()->front().find("Count")->as_i64() == 3);
    CHECK(saved.find("FoodLevel")->as_i64() == 4);
    CHECK(saved.find("LastGossipDecay")->as_i64() == 1234);

    entity::EntityWorld again{*registries()};
    const auto          host2 = storage_host(again);
    MobRecords          records2;
    EntityStorage       storage2{*registries(), scratch.path / "entities"};
    CHECK(storage2.load_chunk(ChunkPos{1, 1}, again, records2, host2).entities == 1);
    for (const entity::EntityHandle h : again.handles()) {
        const gameplay::MobBrain& mind = brain(again, h);
        const auto&               v    = mind.villager;
        CHECK(v.gossips.value(kProbe, b::GossipType::MinorNegative) == 30);
        CHECK(v.gossips.value(net::Uuid{1, 2}, b::GossipType::Trading) == 7);
        CHECK(v.last_gossip_decay == 1234);
        CHECK(v.inventory[0].item == "minecraft:bread");
        CHECK(v.inventory[0].count == 3);
        CHECK(v.food_level == 4);
        CHECK(v.claims.home == BlockPos{21, 64, 23});
        CHECK(v.claims.job_site == BlockPos{25, 64, 20});
        CHECK(mind.memories.number(b::MemoryType::LastSlept) == 999);
        CHECK(mind.memories.ttl(b::MemoryType::GolemDetectedRecently) == 300);
        CHECK(v.typed);
    }
}

TEST_CASE("brains: a villager the real server wrote keeps its brain and its gossip",
          "[server][brains][parity]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    // Read off the real server: the schedule campaign's librarian and the
    // golem campaign's villager (measure_villager_life.py).
    nbt::Tag compound = nbt::Tag::make_compound();
    (void)compound.put("id", nbt::Tag{std::string{"minecraft:villager"}});
    nbt::Tag pos = nbt::Tag::make_list(nbt::TagType::Double);
    (void)pos.push(nbt::Tag{304.5});
    (void)pos.push(nbt::Tag{-60.0});
    (void)pos.push(nbt::Tag{4.5});
    (void)compound.put("Pos", std::move(pos));
    nbt::Tag data = nbt::Tag::make_compound();
    (void)data.put("type", nbt::Tag{std::string{"minecraft:plains"}});
    (void)data.put("profession", nbt::Tag{std::string{"minecraft:librarian"}});
    (void)data.put("level", nbt::Tag{i32{1}});
    (void)compound.put("VillagerData", std::move(data));
    nbt::Tag gossip = nbt::Tag::make_compound();
    (void)gossip.put("Type", nbt::Tag{std::string{"trading"}});
    (void)gossip.put("Target", nbt::Tag{nbt::Tag::IntArray{289470649, -236178987, -1494576147, 376949766}});
    (void)gossip.put("Value", nbt::Tag{i32{4}});
    nbt::Tag gossips = nbt::Tag::make_list(nbt::TagType::Compound);
    (void)gossips.push(std::move(gossip));
    (void)compound.put("Gossips", std::move(gossips));
    const auto place = [](BlockPos p) {
        nbt::Tag at = nbt::Tag::make_compound();
        (void)at.put("pos", nbt::Tag{nbt::Tag::IntArray{p.x, p.y, p.z}});
        (void)at.put("dimension", nbt::Tag{std::string{"minecraft:overworld"}});
        nbt::Tag v = nbt::Tag::make_compound();
        (void)v.put("value", std::move(at));
        return v;
    };
    nbt::Tag memories = nbt::Tag::make_compound();
    (void)memories.put("minecraft:meeting_point", place(BlockPos{307, -60, 1}));
    (void)memories.put("minecraft:home", place(BlockPos{301, -60, 2}));
    (void)memories.put("minecraft:job_site", place(BlockPos{307, -60, 7}));
    nbt::Tag worked = nbt::Tag::make_compound();
    (void)worked.put("value", nbt::Tag{i64{968}});
    (void)memories.put("minecraft:last_worked_at_poi", std::move(worked));
    nbt::Tag golem = nbt::Tag::make_compound();
    (void)golem.put("value", nbt::Tag::make_bool(true));
    (void)golem.put("ttl", nbt::Tag{i64{561}});
    (void)memories.put("minecraft:golem_detected_recently", std::move(golem));
    nbt::Tag brain_tag = nbt::Tag::make_compound();
    (void)brain_tag.put("memories", std::move(memories));
    (void)compound.put("Brain", std::move(brain_tag));

    entity::EntityWorld world{*registries()};
    const auto          host = storage_host(world);
    MobRecords          records;
    ScratchDir          scratch;
    EntityStorage       storage{*registries(), scratch.path / "entities"};
    const auto          handle = storage.decode(compound, world, records, host);
    REQUIRE(handle.has_value());
    const gameplay::MobBrain& mind = brain(world, *handle);
    CHECK(mind.villager.claims.home == BlockPos{301, -60, 2});
    CHECK(mind.villager.claims.job_site == BlockPos{307, -60, 7});
    CHECK(mind.villager.claims.meeting_point == BlockPos{307, -60, 1});
    CHECK(mind.memories.number(b::MemoryType::LastWorkedAtPoi) == 968);
    CHECK(mind.memories.ttl(b::MemoryType::GolemDetectedRecently) == 561);
    CHECK(mind.villager.gossips.value(kProbe, b::GossipType::Trading) == 4);
    // And back out the same way.
    const nbt::Tag out = storage.encode(world, *handle, records, host);
    CHECK(out.find("Brain")->find("memories")->find("minecraft:home") != nullptr);
    CHECK(out.find("Gossips")->list()->size() == 1);
}

TEST_CASE("brains: a trading screen prices every offer for its player", "[server][brains][parity]") {
    gameplay::VillagerState v;
    constexpr std::array<std::pair<i32, f32>, 5> kOffers{
        {{24, 0.05F}, {10, 0.2F}, {1, 0.05F}, {5, 0.05F}, {20, 0.2F}}};
    for (const auto& [base, multiplier] : kOffers) {
        gameplay::MerchantOffer o;
        o.cost_a           = gameplay::TradeItem{.item = "minecraft:emerald", .count = base};
        o.result           = gameplay::TradeItem{.item = "minecraft:glass", .count = 1};
        o.max_uses         = 12;
        o.price_multiplier = multiplier;
        v.offers.push_back(o);
    }
    v.gossips.put(b::GossipEntry{kProbe, b::GossipType::MinorPositive, 100});
    const auto specials = [&] {
        std::array<i32, 5> out{};
        for (usize i = 0; i < 5; ++i) {
            out[i] = v.offers[i].special_price;
        }
        return out;
    };
    // The real server's Merchant Offers, cell by cell.
    apply_special_prices(v, kProbe, -1);
    CHECK(specials() == std::array<i32, 5>{-5, -20, -5, -5, -20});
    CHECK(gameplay::cost_a_count(v.offers[0], 64) == 19);
    apply_special_prices(v, kProbe, 0);
    CHECK(specials() == std::array<i32, 5>{-12, -23, -6, -6, -26});
    apply_special_prices(v, net::Uuid{11, 22}, 4);  // someone else, with Hero IV
    CHECK(specials() == std::array<i32, 5>{-13, -5, -1, -2, -11});
    apply_special_prices(v, net::Uuid{11, 22}, -1);  // control: nothing at all
    CHECK(specials() == std::array<i32, 5>{0, 0, 0, 0, 0});
}

TEST_CASE("brains: the type a biome gives, 53 biomes measured", "[server][brains][parity]") {
    using T = gameplay::VillagerType;
    const std::array<std::pair<std::string_view, T>, 20> cells{{
        {"minecraft:plains", T::Plains},        {"minecraft:cherry_grove", T::Plains},
        {"minecraft:deep_dark", T::Plains},     {"minecraft:stony_peaks", T::Plains},
        {"minecraft:snowy_plains", T::Snow},    {"minecraft:grove", T::Snow},
        {"minecraft:frozen_river", T::Snow},    {"minecraft:deep_frozen_ocean", T::Snow},
        {"minecraft:desert", T::Desert},        {"minecraft:wooded_badlands", T::Desert},
        {"minecraft:swamp", T::Swamp},          {"minecraft:mangrove_swamp", T::Swamp},
        {"minecraft:taiga", T::Taiga},          {"minecraft:windswept_gravelly_hills", T::Taiga},
        {"minecraft:savanna_plateau", T::Savanna}, {"minecraft:windswept_savanna", T::Savanna},
        {"minecraft:bamboo_jungle", T::Jungle}, {"minecraft:sparse_jungle", T::Jungle},
        {"jungle", T::Jungle},                  {"minecraft:cold_ocean", T::Plains},
    }};
    for (const auto& [biome, type] : cells) {
        INFO(biome);
        CHECK(villager_type_for_biome(biome) == type);
    }
}

TEST_CASE("brains: births, golems and witnesses, finished on the server", "[server][brains]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    entity::EntityWorld     world{*registries()};
    VillagerLife            life{*registries(), 1};
    gameplay::VillagerWorld villagers;
    life.attach(villagers);
    usize            announced = 0;
    VillagerLifeHost host;
    host.create_mob = [&](std::string_view type, Vec3d at) { return spawn(world, type, at); };
    host.announce   = [&](const entity::EntityState&) { ++announced; };
    host.biome_at   = [](BlockPos) { return std::string_view{"minecraft:desert"}; };

    const auto a = spawn(world, "minecraft:villager", Vec3d{0.5, 64.0, 0.5});
    const auto c = spawn(world, "minecraft:villager", Vec3d{2.5, 64.0, 0.5});
    (void)life.before_entity_tick(world, 0, host);
    CHECK(brain(world, a).villager.type == gameplay::VillagerType::Desert);  // its biome's
    CHECK(brain(world, a).villager.typed);

    (void)life.before_entity_tick(world, 1, host);
    villagers.events->push_back(b::VillagerEvent{b::VillagerEventKind::Birth, a, c,
                                                 BlockPos{5, 64, 5}, Vec3d{1.5, 64.0, 0.5}, {}});
    villagers.events->push_back(b::VillagerEvent{b::VillagerEventKind::SummonGolem, a,
                                                 entity::kNoEntity, BlockPos{4, 64, 4},
                                                 Vec3d{4.5, 64.0, 4.5}, {}});
    const VillagerLifeStats done = life.after_entity_tick(world, host);
    CHECK(done.births == 1);
    CHECK(done.golems == 1);
    CHECK(announced == 2);  // the baby and the golem (the test's own spawns are not announced)
    bool baby = false, golem = false;
    for (const entity::EntityHandle h : world.handles()) {
        const entity::EntityState* s = world.state(h);
        if (s->type == type_id("minecraft:iron_golem")) {
            golem = true;
            CHECK(s->position.x == 4.5);
        } else if (h != a && h != c) {
            baby = true;
            CHECK(brain(world, h).animal.age == gameplay::kBabyAge);
            CHECK(brain(world, h).villager.claims.home == BlockPos{5, 64, 5});
        }
    }
    CHECK((baby && golem));

    // Witnesses: measured, villagers 4 and 12 blocks from a killing heard
    // major_negative 25, one at 20 did not.
    const auto near = spawn(world, "minecraft:villager", Vec3d{12.5, 64.0, 0.5});
    const auto far  = spawn(world, "minecraft:villager", Vec3d{20.5, 64.0, 0.5});
    life.on_player_killed(world, Vec3d{0.5, 64.0, 0.5}, world.state(a)->network_id, kProbe);
    CHECK(brain(world, c).villager.gossips.value(kProbe, b::GossipType::MajorNegative) == 25);
    CHECK(brain(world, near).villager.gossips.value(kProbe, b::GossipType::MajorNegative) == 25);
    CHECK(brain(world, far).villager.gossips.value(kProbe, b::GossipType::MajorNegative) == 0);
    life.on_player_hurt(world, world.state(far)->network_id, kProbe);
    life.on_player_hurt(world, world.state(far)->network_id, kProbe);
    CHECK(brain(world, far).villager.gossips.value(kProbe, b::GossipType::MinorNegative) == 50);
}
