// ── tame ── The server half of taming: the keys a tame animal takes into
// entities/, our own round trip, the cat's wire order, and a zoo the real
// 1.20.1 server wrote read back by this one (docs/provenance/apprivoisement.md).
#include "../src/entity_storage.hpp"
#include "../src/mob_despawn.hpp"
#include "../src/taming.hpp"

#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/tame.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

struct ScratchDir {
    std::filesystem::path path;
    ScratchDir() {
        std::random_device device;
        path = std::filesystem::temp_directory_path() /
               ("ov_tame_" + std::to_string(device()) + "_" + std::to_string(device()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    ScratchDir(const ScratchDir&)            = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

/// The server's side of a load: brains, and taming's own keys.
struct Host {
    entity::EntityWorld* world{nullptr};
    Taming*              taming{nullptr};

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
        host.write_extra = [this](const entity::EntityState& state, nbt::Tag& out) {
            taming->write_nbt(*world, state, out);
        };
        host.read_extra = [this](entity::EntityState& state, const nbt::Tag& compound) {
            taming->read_nbt(*world, state, compound);
        };
        return host;
    }
};

entity::EntityHandle spawn_mob(entity::EntityWorld& world, const EntityStorageHost& host,
                               std::string_view type, Vec3d at) {
    const auto handle = world.spawn(type, at, net::Uuid{0x7A3E'0000'0000'0000ULL,
                                                        static_cast<u64>(at.x * 1000.0)});
    REQUIRE(handle.has_value());
    host.attach(*handle, type);
    return *handle;
}

gameplay::Mob& mob_of(entity::EntityWorld& world, entity::EntityHandle handle) {
    return *dynamic_cast<gameplay::Mob*>(world.logic(handle));
}

[[nodiscard]] entity::EntityHandle find_type(entity::EntityWorld& world, std::string_view type) {
    for (const entity::EntityHandle handle : world.handles()) {
        if (world.state(handle)->type == type_id(type)) {
            return handle;
        }
    }
    return entity::kNoEntity;
}

}  // namespace

TEST_CASE("tame: a cat's variants are in the wire's order", "[server][tame]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    // The protocol ids come from the registries report, through the pack.
    const auto variants = registries()->find("minecraft:cat_variant");
    REQUIRE(variants.has_value());
    const auto names = Taming::cat_variants();
    REQUIRE(names.size() == 11);
    for (usize i = 0; i < names.size(); ++i) {
        const std::string name = "minecraft:" + std::string{names[i]};
        INFO(name);
        CHECK(registries()->protocol_id(*variants, name) == std::optional<i32>{static_cast<i32>(i)});
    }
    // The measured pair (apprivoisement.md § 2): tabby on the wire is 0, and
    // black, which a spawn does not send, is the default 1.
    CHECK(registries()->protocol_id(*variants, "minecraft:tabby") == std::optional<i32>{0});
    CHECK(registries()->protocol_id(*variants, "minecraft:black") ==
          std::optional<i32>{net::metadata::kCatDefaultVariant});
}

TEST_CASE("tame: a tame zoo goes to entities/ and comes back", "[server][tame][anvil]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    ScratchDir          scratch;
    Taming              taming{*registries()};
    entity::EntityWorld world{*registries()};
    Host                holder{&world, &taming};
    const EntityStorageHost host = holder.make();
    MobRecords              records;
    EntityStorage           storage{*registries(), scratch.path / "entities"};
    (void)storage.load_chunk(ChunkPos{0, 0}, world, records, host);

    const net::Uuid me{0x1111, 0x2222};
    const auto wolf = spawn_mob(world, host, "minecraft:wolf", Vec3d{1.5, 64.0, 1.5});
    {
        gameplay::TameState& t = mob_of(world, wolf).mutable_brain().tame;
        t.tame    = true;
        t.owner   = me;
        t.sitting = true;
        t.collar  = 11;
        gameplay::apply_tame_body(t, *world.mutable_state(wolf));
        world.mutable_state(wolf)->health = 17.0F;
    }
    const auto horse = spawn_mob(world, host, "minecraft:horse", Vec3d{4.5, 64.0, 1.5});
    {
        gameplay::TameState& t = mob_of(world, horse).mutable_brain().tame;
        t.tame    = true;
        t.owner   = me;
        t.saddled = true;
        t.armour  = "minecraft:iron_horse_armor";
        t.temper  = 30;
        t.variant = 515;
        t.stats   = gameplay::HorseStats{22.0, 0.25, 0.8};
        world.mutable_state(horse)->max_health = 0.0F;
        gameplay::apply_tame_body(t, *world.mutable_state(horse));
    }
    const auto llama = spawn_mob(world, host, "minecraft:llama", Vec3d{8.5, 64.0, 1.5});
    {
        gameplay::TameState& t = mob_of(world, llama).mutable_brain().tame;
        t.tame     = true;
        t.chested  = true;
        t.strength = 4;
        t.variant  = 2;
        t.armour   = "minecraft:blue_carpet";
    }
    const auto cat = spawn_mob(world, host, "minecraft:cat", Vec3d{11.5, 64.0, 1.5});
    {
        gameplay::TameState& t = mob_of(world, cat).mutable_brain().tame;
        t.tame    = true;
        t.owner   = me;
        t.variant = 5;
        t.collar  = 3;
    }
    const auto goat = spawn_mob(world, host, "minecraft:goat", Vec3d{13.5, 64.0, 1.5});
    mob_of(world, goat).mutable_brain().tame.left_horn = false;

    // The keys, at the types vanilla writes (after_tame dump, measure_tame.py).
    const nbt::Tag wolf_nbt = storage.encode(world, wolf, records, host);
    REQUIRE(wolf_nbt.find("Owner") != nullptr);
    CHECK(wolf_nbt.find("Owner")->get_if<nbt::Tag::IntArray>() != nullptr);
    CHECK(wolf_nbt.find("Sitting")->type() == nbt::TagType::Byte);
    CHECK(wolf_nbt.find("CollarColor")->type() == nbt::TagType::Byte);
    CHECK(wolf_nbt.find("AngerTime")->type() == nbt::TagType::Int);
    const nbt::Tag horse_nbt = storage.encode(world, horse, records, host);
    CHECK(horse_nbt.find("Temper")->type() == nbt::TagType::Int);
    CHECK(horse_nbt.find("Tame")->type() == nbt::TagType::Byte);
    CHECK(horse_nbt.find("SaddleItem")->find("id")->as_string() == "minecraft:saddle");
    CHECK(horse_nbt.find("ArmorItem")->find("id")->as_string() == "minecraft:iron_horse_armor");
    const nbt::Tag* attributes = horse_nbt.find("Attributes");
    REQUIRE(attributes != nullptr);
    bool jump = false;
    for (const nbt::Tag& entry : *attributes->list()) {
        if (entry.find("Name")->as_string() == "minecraft:horse.jump_strength") {
            CHECK(entry.find("Base")->as_f64() == 0.8);
            jump = true;
        }
    }
    CHECK(jump);

    (void)storage.save_all(world, records, host);

    // A second world reads them back.
    Taming              taming2{*registries()};
    entity::EntityWorld world2{*registries()};
    Host                holder2{&world2, &taming2};
    const EntityStorageHost host2 = holder2.make();
    MobRecords              records2;
    EntityStorage           storage2{*registries(), scratch.path / "entities"};
    (void)storage2.load_chunk(ChunkPos{0, 0}, world2, records2, host2);

    const auto wolf2 = find_type(world2, "minecraft:wolf");
    REQUIRE(wolf2 != entity::kNoEntity);
    {
        const gameplay::TameState& t = mob_of(world2, wolf2).brain().tame;
        CHECK(t.tame);
        CHECK(t.owned_by(me));
        CHECK(t.sitting);
        CHECK(t.collar == 11);
        // Health 17 of 20 — not clamped to a wild wolf's 8 on the way in.
        CHECK(world2.state(wolf2)->max_health == 20.0F);
        CHECK(world2.state(wolf2)->health == 17.0F);
    }
    const auto horse2 = find_type(world2, "minecraft:horse");
    REQUIRE(horse2 != entity::kNoEntity);
    {
        const gameplay::TameState& t = mob_of(world2, horse2).brain().tame;
        CHECK(t.tame);
        CHECK(t.saddled);
        CHECK(t.armour == "minecraft:iron_horse_armor");
        CHECK(t.temper == 30);
        CHECK(t.variant == 515);
        CHECK(t.stats.max_health == 22.0);
        CHECK(t.stats.speed == 0.25);
        CHECK(t.stats.jump == 0.8);
        CHECK(world2.state(horse2)->max_health == 22.0F);
    }
    const auto llama2 = find_type(world2, "minecraft:llama");
    REQUIRE(llama2 != entity::kNoEntity);
    {
        const gameplay::TameState& t = mob_of(world2, llama2).brain().tame;
        CHECK(t.chested);
        CHECK(t.strength == 4);
        CHECK(t.variant == 2);
        CHECK(t.armour == "minecraft:blue_carpet");
    }
    const auto cat2 = find_type(world2, "minecraft:cat");
    REQUIRE(cat2 != entity::kNoEntity);
    CHECK(mob_of(world2, cat2).brain().tame.variant == 5);
    CHECK(mob_of(world2, cat2).brain().tame.collar == 3);
    const auto goat2 = find_type(world2, "minecraft:goat");
    REQUIRE(goat2 != entity::kNoEntity);
    CHECK_FALSE(mob_of(world2, goat2).brain().tame.left_horn);
    CHECK(mob_of(world2, goat2).brain().tame.right_horn);
}

TEST_CASE("tame: a zoo the real server wrote, read by this one", "[server][tame][parity]") {
    if (registries() == nullptr) {
        return;
    }
    // Written by scripts/measure_tame.py `zoo` (16 NoAI mobs tagged "tamezoo")
    // and kept, untracked, under .scratch/. Absent on a fresh checkout.
    const auto entities =
        std::filesystem::path{OV_SOURCE_DIR} / ".scratch" / "tame-zoo-vanilla" / "entities";
    if (!std::filesystem::exists(entities)) {
        WARN("no vanilla tame zoo at " << entities.string() << " — run measure_tame.py zoo");
        return;
    }
    Taming              taming{*registries()};
    entity::EntityWorld world{*registries()};
    Host                holder{&world, &taming};
    const EntityStorageHost host = holder.make();
    MobRecords              records;
    EntityStorage           storage{*registries(), entities};
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -3; z <= 1; ++z) {
            (void)storage.load_chunk(ChunkPos{x, z}, world, records, host);
        }
    }
    const net::Uuid probe = net::Uuid::offline_player("ovhand");
    usize zoo = 0, checked = 0;
    std::vector<std::string> seen;       // every zoo mob's type
    std::vector<std::string> unmatched;  // the ones no check below took
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        const nbt::Tag             out   = storage.encode(world, handle, records, host);
        const nbt::Tag*            tags  = out.find("Tags");
        const bool in_zoo = tags != nullptr && tags->list() != nullptr &&
                            std::ranges::any_of(*tags->list(), [](const nbt::Tag& tag) {
                                return tag.as_string() == "tamezoo";
                            });
        if (!in_zoo) {
            continue;
        }
        ++zoo;
        const gameplay::MobBrain& brain = mob_of(world, handle).brain();
        const gameplay::TameState& t    = brain.tame;
        const auto is = [&](std::string_view type) { return state->type == type_id(type); };
        const usize before = checked;
        const auto  types  = registries()->find("minecraft:entity_type");
        seen.emplace_back(registries()->entry_of(*types, state->type));
        if (is("minecraft:wolf") && t.owner) {
            CHECK(t.tame);
            CHECK(t.owned_by(probe));
            CHECK(t.sitting);
            CHECK(t.collar == 14);
            CHECK(state->max_health == 20.0F);
            CHECK(state->health == 20.0F);
            ++checked;
        } else if (is("minecraft:cat")) {
            CHECK(t.variant == 5);  // calico
            CHECK(t.owned_by(probe));
            CHECK(t.sitting);
            CHECK(t.collar == 3);
            ++checked;
        } else if (is("minecraft:ocelot")) {
            CHECK(t.trusting);
            ++checked;
        } else if (is("minecraft:horse")) {
            CHECK(t.tame);
            CHECK(t.owned_by(probe));
            CHECK(t.variant == 515);
            CHECK(t.temper == 15);
            CHECK(t.saddled);
            CHECK(t.armour == "minecraft:golden_horse_armor");
            CHECK(t.bred);
            // Its stats are the ones its `Attributes` list carries: read in,
            // and written back out unchanged.
            const nbt::Tag* list = out.find("Attributes");
            REQUIRE(list != nullptr);
            for (const nbt::Tag& entry : *list->list()) {
                if (entry.find("Name")->as_string() == "minecraft:generic.max_health") {
                    CHECK(t.stats.max_health == entry.find("Base")->as_f64());
                    CHECK(state->max_health == static_cast<f32>(t.stats.max_health));
                }
            }
            CHECK(t.stats.drawn());
            ++checked;
        } else if (is("minecraft:donkey")) {
            CHECK(t.tame);
            CHECK(t.chested);
            REQUIRE(out.find("Items") != nullptr);  // carried through untouched
            CHECK(out.find("Items")->list()->size() == 1);
            ++checked;
        } else if (is("minecraft:mule")) {
            CHECK(t.tame);
            ++checked;
        } else if (is("minecraft:llama")) {
            CHECK(t.tame);
            CHECK(t.variant == 3);
            CHECK(t.strength == 4);
            CHECK(t.armour == "minecraft:blue_carpet");
            ++checked;
        } else if (is("minecraft:trader_llama")) {
            CHECK(t.variant == 1);
            CHECK(t.strength == 2);
            ++checked;
        } else if (is("minecraft:rabbit")) {
            CHECK(t.variant == 3);
            ++checked;
        } else if (is("minecraft:fox")) {
            CHECK(t.variant == 1);
            CHECK(t.flag);
            ++checked;
        } else if (is("minecraft:parrot")) {
            CHECK(t.variant == 2);
            CHECK(t.owned_by(probe));
            CHECK(t.sitting);
            ++checked;
        } else if (is("minecraft:turtle") || is("minecraft:bee")) {
            CHECK(t.flag);
            ++checked;
        } else if (is("minecraft:goat")) {
            CHECK(t.flag);
            CHECK(t.left_horn);
            CHECK_FALSE(t.right_horn);
            ++checked;
        } else if (is("minecraft:camel")) {
            CHECK(brain.animal.saddled);
            ++checked;
        }
        if (checked == before) {
            unmatched.push_back(seen.back());
        }
    }
    // The zoo, by type: a missing or unread mob is named in the failure.
    // Sixteen were summoned; the real server saved fifteen — no trader llama
    // is in its entities/, and its `data get` at the trader llama's spot
    // answered with the cat (apprivoisement.md § 8). What it saved, we read.
    std::ranges::sort(seen);
    std::vector<std::string> expected{
        "minecraft:bee",    "minecraft:camel",  "minecraft:cat",    "minecraft:donkey",
        "minecraft:fox",    "minecraft:goat",   "minecraft:horse",  "minecraft:llama",
        "minecraft:mule",   "minecraft:ocelot", "minecraft:parrot", "minecraft:rabbit",
        "minecraft:turtle", "minecraft:wolf",   "minecraft:wolf"};
    std::ranges::sort(expected);
    CHECK(seen == expected);
    // Only the wild wolf is left to no check: it carries nothing tame.
    CHECK(unmatched == std::vector<std::string>{"minecraft:wolf"});
    CHECK(zoo == 15);
}
