// ── nether-2 ── The Nether's mob session, driven through its host.
//
// What a player sees end to end is checked by scripts/check_nether_mobs_e2e.py
// against a running server; this is the same session with a fake Nether — a
// flat netherrack floor, one player — so that the barter, the blaze's volley
// and the wire-id routing are pinned without a socket.

#include "../src/mob_combat.hpp"
#include "../src/nether_mobs.hpp"

#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] std::filesystem::path generated_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated";
}

[[nodiscard]] bool have_data() {
    return std::filesystem::is_regular_file(pack_path()) &&
           std::filesystem::is_directory(generated_root() / "data" / "minecraft" / "loot_tables");
}

constexpr std::array<std::string_view, 5> kNetherBiomes{
    "minecraft:basalt_deltas", "minecraft:crimson_forest", "minecraft:nether_wastes",
    "minecraft:soul_sand_valley", "minecraft:warped_forest"};

/// A floor of netherrack at y 64, air above, one survival player standing on
/// it at the origin; every packet, drop and hurt recorded.
struct FakeNether {
    registry::BlockStateId              netherrack{};
    std::vector<i32>                    packets;
    std::vector<net::ItemStack>         drops;
    std::vector<f32>                    hurts;
    i32                                 ingots{8};
    NetherMobHost                       host;

    explicit FakeNether(const registry::BlockRegistry& blocks) {
        netherrack       = blocks.default_state(*blocks.find_block("minecraft:netherrack"));
        host.block_at    = [this](BlockPos pos) {
            return pos.y <= 64 ? netherrack : registry::kAirState;
        };
        host.loaded      = [](BlockPos) { return true; };
        host.ticking     = [](ChunkPos) { return false; };  // nothing spawns by itself
        host.biome_at    = [](BlockPos) -> u16 { return 2; };
        host.block_light = [](BlockPos) -> u8 { return 0; };
        host.players     = [](std::vector<NetherPlayer>& out) {
            out.push_back(NetherPlayer{7, Vec3d{0.5, 65.0, 0.5}, false, true, false});
        };
        host.broadcast   = [this](i32 id, std::span<const u8>) { packets.push_back(id); };
        host.drop_item   = [this](Vec3d, const net::ItemStack& stack) { drops.push_back(stack); };
        host.hurt_player = [this](i32, f32 damage, gameplay::DamageKind) {
            hurts.push_back(damage);
            return true;
        };
        host.take_held   = [this](i32, std::string_view item) {
            if (item != "minecraft:gold_ingot" || ingots == 0) {
                return false;
            }
            --ingots;
            return true;
        };
    }
};

}  // namespace

TEST_CASE("the Nether's entities are routed by their wire ids", "[server][nether]") {
    CHECK_FALSE(NetherMobs::owns(1));
    CHECK_FALSE(NetherMobs::owns(1'000'005));  // an overworld mob
    CHECK(NetherMobs::owns(NetherMobs::kFirstId));
    CHECK(NetherMobs::owns(NetherMobs::kFirstId + 12'345));
}

TEST_CASE("a piglin handed a gold ingot pays one barter 120 ticks later",
          "[server][nether]") {
    if (!have_data()) {
        SKIP("registry pack or generated data absent");
    }
    auto registries = registry::Registries::load(pack_path());
    auto blocks     = registry::BlockRegistry::load(pack_path());
    REQUIRE(registries.has_value());
    REQUIRE(blocks.has_value());
    MobCombat  combat{*registries, nullptr};
    NetherMobs mobs{*registries, *blocks, &combat, nullptr, kNetherBiomes, generated_root(), 42};
    FakeNether nether{*blocks};

    const auto piglin = mobs.summon("minecraft:piglin", Vec3d{2.5, 65.0, 0.5}, nether.host);
    REQUIRE(piglin.has_value());
    CHECK(NetherMobs::owns(*piglin));
    CHECK(mobs.type_of(*piglin) == "minecraft:piglin");

    mobs.queue_interact(7, *piglin);
    i64 tick = 0;
    for (; tick < 119; ++tick) {
        (void)mobs.tick(tick, nether.host);
    }
    CHECK(nether.ingots == 7);       // taken on the first tick
    CHECK(nether.drops.empty());     // still admiring
    for (; tick < 125; ++tick) {
        (void)mobs.tick(tick, nether.host);
    }
    REQUIRE(nether.drops.size() == 1);
    const auto items = registries->find("minecraft:item");
    REQUIRE(items.has_value());
    const std::string_view paid = registries->entry_of(*items, nether.drops.front().item_id);
    // One of the table's items (an enchanted book stands for the book).
    static constexpr std::array<std::string_view, 17> kTable{
        "minecraft:enchanted_book", "minecraft:iron_boots", "minecraft:potion",
        "minecraft:splash_potion",  "minecraft:iron_nugget", "minecraft:ender_pearl",
        "minecraft:string",         "minecraft:quartz",      "minecraft:obsidian",
        "minecraft:crying_obsidian", "minecraft:fire_charge", "minecraft:leather",
        "minecraft:soul_sand",      "minecraft:nether_brick", "minecraft:spectral_arrow",
        "minecraft:gravel",         "minecraft:blackstone"};
    CHECK(std::ranges::find(kTable, paid) != kTable.end());

    // A second click while nothing is held: nothing happens.
    nether.ingots = 0;
    mobs.queue_interact(7, *piglin);
    for (i32 i = 0; i < 130; ++i) {
        (void)mobs.tick(tick++, nether.host);
    }
    CHECK(nether.drops.size() == 1);
}

TEST_CASE("a blaze that sees a player shoots, and its small fireball hurts for 5",
          "[server][nether]") {
    if (!have_data()) {
        SKIP("registry pack or generated data absent");
    }
    auto registries = registry::Registries::load(pack_path());
    auto blocks     = registry::BlockRegistry::load(pack_path());
    REQUIRE(registries.has_value());
    REQUIRE(blocks.has_value());
    MobCombat  combat{*registries, nullptr};
    NetherMobs mobs{*registries, *blocks, &combat, nullptr, kNetherBiomes, generated_root(), 42};
    FakeNether nether{*blocks};

    REQUIRE(mobs.summon("minecraft:blaze", Vec3d{6.5, 65.0, 0.5}, nether.host).has_value());
    usize shots = 0;
    for (i64 tick = 0; tick < 200 && nether.hurts.empty(); ++tick) {
        shots += mobs.tick(tick, nether.host).shots;
    }
    CHECK(shots >= 1);
    REQUIRE_FALSE(nether.hurts.empty());
    CHECK(nether.hurts.front() == 5.0F);
}

TEST_CASE("a hit on a Nether mob lands there, and a kill removes it", "[server][nether]") {
    if (!have_data()) {
        SKIP("registry pack or generated data absent");
    }
    auto registries = registry::Registries::load(pack_path());
    auto blocks     = registry::BlockRegistry::load(pack_path());
    REQUIRE(registries.has_value());
    REQUIRE(blocks.has_value());
    MobCombat  combat{*registries, nullptr};
    NetherMobs mobs{*registries, *blocks, &combat, nullptr, kNetherBiomes, generated_root(), 42};
    FakeNether nether{*blocks};

    const auto strider = mobs.summon("minecraft:strider", Vec3d{3.5, 65.0, 3.5}, nether.host);
    REQUIRE(strider.has_value());
    CHECK_FALSE(mobs.hurt(1'000'001, 4.0F, 7, 0, nether.host));  // not one of ours
    CHECK(mobs.hurt(*strider, 100.0F, 7, 0, nether.host));
    (void)mobs.tick(0, nether.host);
    CHECK(mobs.type_of(*strider).empty());  // gone after the tick
}
