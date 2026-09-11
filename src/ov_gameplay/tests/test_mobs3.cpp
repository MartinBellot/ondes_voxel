// ── mobs-3 ── A hostile mob's hit on a player: difficulty, armour, effects,
// and the goals that finally aim at a player (docs/provenance/mobs-3.md).
//
// Every `[parity]` number below was read off a real 1.20.1 server by
// scripts/measure_mobs3.py `melee`: a probe in survival that does not move, one
// mob summoned beside it, the health read back after each hit.
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/mob_attack.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/primed_tnt.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

/// What the player loses from one hit of `attack` points, through the same
/// `apply_damage` a server player is hurt by.
[[nodiscard]] f32 lost(f32 attack, Difficulty difficulty, f32 armour, f32 toughness = 0.0F,
                       u8 epf = 0) {
    HealthState      health{};
    DamageMitigation mitigation{};
    mitigation.armour    = armour;
    mitigation.toughness = toughness;
    mitigation.protection[static_cast<usize>(DamageKind::MobAttack)] = epf;
    const f32 before = health.health;
    (void)apply_damage(health, DamageKind::MobAttack, scale_for_difficulty(attack, difficulty),
                       DamageConstants{}, mitigation);
    return before - health.health;
}

}  // namespace

TEST_CASE("mobs-3: a mob's hit scales with difficulty", "[gameplay][mobs3][parity]") {
    // Zombie (attack_damage 3), no armour: 2.5, 3, 4.5 — measured, five hits
    // each. Spider (2): 2, 2, 3.
    CHECK(lost(3.0F, Difficulty::Easy, 0.0F) == Catch::Approx(2.5F));
    CHECK(lost(3.0F, Difficulty::Normal, 0.0F) == Catch::Approx(3.0F));
    CHECK(lost(3.0F, Difficulty::Hard, 0.0F) == Catch::Approx(4.5F));
    CHECK(lost(2.0F, Difficulty::Easy, 0.0F) == Catch::Approx(2.0F));
    CHECK(lost(2.0F, Difficulty::Hard, 0.0F) == Catch::Approx(3.0F));
    CHECK(scale_for_difficulty(3.0F, Difficulty::Peaceful) == 0.0F);
}

TEST_CASE("mobs-3: armour, toughness and Protection, as a real server takes them",
          "[gameplay][mobs3][parity]") {
    // Leather 7, iron 15, diamond 20 with toughness 8, iron with Protection IV
    // on the chestplate (EPF 4). Measured to the printed digit.
    CHECK(lost(3.0F, Difficulty::Normal, 7.0F) == Catch::Approx(2.34).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Normal, 15.0F) == Catch::Approx(1.38).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Normal, 20.0F, 8.0F) == Catch::Approx(0.69).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Normal, 15.0F, 0.0F, 4) == Catch::Approx(1.1592).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Easy, 7.0F) == Catch::Approx(1.925).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Easy, 15.0F) == Catch::Approx(1.125).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Easy, 20.0F, 8.0F) == Catch::Approx(0.5625).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Hard, 7.0F) == Catch::Approx(3.645).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Hard, 15.0F) == Catch::Approx(2.205).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Hard, 20.0F, 8.0F) == Catch::Approx(1.1025).margin(1e-5));
    CHECK(lost(3.0F, Difficulty::Hard, 15.0F, 0.0F, 4) == Catch::Approx(1.8522).margin(1e-5));
    // An enderman (7) and a zombified piglin (5), provoked, against iron.
    CHECK(lost(7.0F, Difficulty::Normal, 15.0F) == Catch::Approx(3.78).margin(1e-5));
    CHECK(lost(5.0F, Difficulty::Normal, 15.0F) == Catch::Approx(2.5).margin(1e-5));

    // A full set, summed from the table, is what the attribute reads.
    f32 iron = 0.0F;
    for (const char* piece : {"minecraft:iron_helmet", "minecraft:iron_chestplate",
                              "minecraft:iron_leggings", "minecraft:iron_boots"}) {
        iron += armour_piece(piece).value().defense;
    }
    CHECK(iron == 15.0F);
    CHECK_FALSE(armour_piece("minecraft:stick").has_value());

    // #bypasses_armor: starving ignores all of it.
    CHECK(after_armour(DamageKind::Starve, 3.0F, 20.0F, 8.0F) == 3.0F);
}

TEST_CASE("mobs-3: the effect a husk and a cave spider leave", "[gameplay][mobs3][parity]") {
    // A fresh world: regional difficulty 0.75 × id.
    const f32 easy   = effective_regional_difficulty(Difficulty::Easy, 0, 0, 1.0F);
    const f32 normal = effective_regional_difficulty(Difficulty::Normal, 0, 0, 1.0F);
    const f32 hard   = effective_regional_difficulty(Difficulty::Hard, 0, 0, 1.0F);
    CHECK(easy == Catch::Approx(0.75F));
    CHECK(normal == Catch::Approx(1.5F));
    CHECK(hard == Catch::Approx(2.25F));

    // Measured: no Hunger on Easy, 140 ticks on Normal, 280 on Hard (read 137
    // and 278 three and two ticks after the hit).
    CHECK_FALSE(melee_hit_effect("minecraft:husk", Difficulty::Easy, easy).has_value());
    CHECK(melee_hit_effect("minecraft:husk", Difficulty::Normal, normal)->duration == 140);
    CHECK(melee_hit_effect("minecraft:husk", Difficulty::Hard, hard)->duration == 280);
    CHECK(melee_hit_effect("minecraft:husk", Difficulty::Hard, hard)->effect == Effect::Hunger);
    // Poison: none, 140 (read 138), 300 (read 298).
    CHECK_FALSE(melee_hit_effect("minecraft:cave_spider", Difficulty::Easy, easy).has_value());
    CHECK(melee_hit_effect("minecraft:cave_spider", Difficulty::Normal, normal)->duration == 140);
    CHECK(melee_hit_effect("minecraft:cave_spider", Difficulty::Hard, hard)->duration == 300);
    CHECK_FALSE(melee_hit_effect("minecraft:zombie", Difficulty::Hard, hard).has_value());

    // An old world grows: 21 hours in, a full moon, a chunk lived in for 50 h.
    const f32 aged = effective_regional_difficulty(Difficulty::Hard, 72000 + 1440000, 3600000,
                                                   moon_brightness(0));
    CHECK(aged == Catch::Approx(3.0F * (0.75F + 0.25F + 1.0F + 0.25F)));
    CHECK(moon_brightness(4 * 24000) == 0.0F);
}

TEST_CASE("mobs-3: the melee reach", "[gameplay][mobs3]") {
    // A zombie on a player: 0.6·2·0.6·2 + 0.6 = 2.04, so √2.04 ≈ 1.43 blocks.
    CHECK(melee_reach_sq(0.6F, 0.6F) == Catch::Approx(2.04).margin(1e-6));
    // A spider (1.4 wide) reaches a player at √8.44 ≈ 2.9.
    CHECK(melee_reach_sq(1.4F, 0.6F) == Catch::Approx(8.44).margin(1e-5));
}

// ── The goals, against a flat floor ─────────────────────────────────────────

namespace {

class FlatLevel final : public world::LevelView {
public:
    explicit FlatLevel(const registry::BlockRegistry& registry) : registry_{&registry} {
        air_   = registry.default_state(registry.find_block("minecraft:air").value());
        stone_ = registry.default_state(registry.find_block("minecraft:stone").value());
    }
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        return pos.y < 0 ? stone_ : air_;
    }
    [[nodiscard]] bool is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override {
        return world::WorldShape::overworld();
    }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

    static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
        return static_cast<const FlatLevel*>(context)->block_at(BlockPos{x, y, z});
    }

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         air_{};
    registry::BlockStateId         stone_{};
};

struct Rig {
    FlatLevel           level;
    CollisionWorld      collisions;
    entity::EntityWorld world;
    i32                 player_type{-1};
    i32                 villager_type{-1};

    Rig()
        : level{*blocks()},
          collisions{*blocks(), &FlatLevel::look_up, &level},
          world{*registries()} {
        const auto types = registries()->find("minecraft:entity_type");
        player_type   = registries()->protocol_id(*types, "minecraft:player").value();
        villager_type = registries()->protocol_id(*types, "minecraft:villager").value();
    }

    entity::EntityHandle mob(std::string_view type, Vec3d at) {
        const auto handle = world.spawn(type, at, net::Uuid{}).value();
        const entity::EntityState* state = world.state(handle);
        world.set_logic(handle, std::make_unique<Mob>(*mob_kind(type), state->width, state->height,
                                                      state->network_id, player_type));
        return handle;
    }

    /// Tick `count` times; returns the tick of every attack, with its target.
    std::vector<std::pair<i64, MobAttack>> run(i64 count, std::span<const Quarry> quarries) {
        std::vector<std::pair<i64, MobAttack>> out;
        std::vector<MobAttack>                 attacks;
        MobContext                             context{&collisions, &level, false};
        context.quarries      = quarries;
        context.attacks       = &attacks;
        context.villager_type = villager_type;
        for (i64 tick = 0; tick < count; ++tick) {
            attacks.clear();
            world.tick(entity::TickContext{tick, &context});
            for (const MobAttack& attack : attacks) {
                out.emplace_back(tick, attack);
            }
        }
        return out;
    }
};

}  // namespace

TEST_CASE("mobs-3: a zombie hunts a player it can see and hits every 20 ticks",
          "[gameplay][mobs3]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig;
    const auto zombie = rig.mob("minecraft:zombie", Vec3d{0.5, 0.0, 0.5});
    const std::array<Quarry, 1> players{Quarry{.network_id = 9001, .type = rig.player_type,
                                               .feet = Vec3d{6.5, 0.0, 0.5}}};
    const auto hits = rig.run(400, players);
    REQUIRE(hits.size() >= 5);
    for (const auto& [tick, attack] : hits) {
        CHECK(attack.target == 9001);
        CHECK(attack.target_is_player);
        CHECK(attack.attacker == zombie);
    }
    for (usize i = 1; i < hits.size(); ++i) {
        CHECK(hits[i].first - hits[i - 1].first == kMeleeCooldownTicks);
    }
    // It walked up to the player and stopped inside the game's reach.
    const Vec3d at  = rig.world.state(zombie)->position;
    const f64   gap = (at - players[0].feet).length();
    CHECK(gap * gap <= melee_reach_sq(0.6F, 0.6F) + 1e-9);
}

TEST_CASE("mobs-3: with no quarry listed there is no target and no hit", "[gameplay][mobs3]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Rig        rig;
    const auto zombie = rig.mob("minecraft:zombie", Vec3d{0.5, 0.0, 0.5});
    const auto hits   = rig.run(200, {});
    CHECK(hits.empty());
    const auto* mob = dynamic_cast<Mob*>(rig.world.logic(zombie));
    REQUIRE(mob != nullptr);
    CHECK(mob->brain().target_player == 0);
}

TEST_CASE("mobs-3: a creeper closes to its swell's three blocks and never swings",
          "[gameplay][mobs3]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Rig        rig;
    const auto creeper = rig.mob("minecraft:creeper", Vec3d{0.5, 0.0, 0.5});
    const std::array<Quarry, 1> players{Quarry{.network_id = 7, .type = rig.player_type,
                                               .feet = Vec3d{8.5, 0.0, 0.5}}};
    const auto hits = rig.run(600, players);
    CHECK(hits.empty());
    const f64 gap = (rig.world.state(creeper)->position - players[0].feet).length();
    CHECK(gap <= kCreeperSwellStart);
    CHECK(gap > 2.0);
}

TEST_CASE("mobs-3: a creeper seeks only within its follow range of 16", "[gameplay][mobs3]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Rig        rig;
    const auto creeper = rig.mob("minecraft:creeper", Vec3d{0.5, 0.0, 0.5});
    const std::array<Quarry, 1> players{Quarry{.network_id = 7, .type = rig.player_type,
                                               .feet = Vec3d{24.5, 0.0, 0.5}}};
    (void)rig.run(100, players);
    const auto* mob = dynamic_cast<Mob*>(rig.world.logic(creeper));
    REQUIRE(mob != nullptr);
    CHECK(mob->brain().target_player == 0);
}

TEST_CASE("mobs-3: a zombie hunts a villager, and a player it sees first", "[gameplay][mobs3]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Rig        rig;
    const auto zombie   = rig.mob("minecraft:zombie", Vec3d{0.5, 0.0, 0.5});
    const auto villager = rig.world.spawn("minecraft:villager", Vec3d{5.5, 0.0, 0.5}, net::Uuid{});
    REQUIRE(villager.has_value());
    const auto hits = rig.run(200, {});
    REQUIRE_FALSE(hits.empty());
    CHECK_FALSE(hits.front().second.target_is_player);
    CHECK(hits.front().second.target == rig.world.state(*villager)->network_id);
    (void)zombie;
}
