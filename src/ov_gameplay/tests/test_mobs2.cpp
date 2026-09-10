// ── mobs-2 ── The walk law, the species' measured modifiers, and what a type
// asks of the place it spawns. Every `[parity]` number below was read off a
// real 1.20.1 server by scripts/measure_mobs2.py; docs/provenance/mobs-2.md
// has the campaigns.
#include "ov/gameplay/conversion.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/slime.hpp"
#include "ov/gameplay/spawn_rules.hpp"
#include "ov/gameplay/spawning.hpp"
#include "ov/gameplay/walk_speed.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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

/// Everything below y = 0 is `floor`; above, air.
class Ground final : public world::LevelView {
public:
    Ground(const registry::BlockRegistry& registry, std::string_view floor) : registry_{&registry} {
        air_   = registry.default_state(registry.find_block("minecraft:air").value());
        floor_ = registry.default_state(registry.find_block(floor).value());
    }

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        return pos.y < 0 ? floor_ : air_;
    }

    [[nodiscard]] bool is_loaded(BlockPos) const override { return true; }

    [[nodiscard]] world::WorldShape shape() const override {
        return world::WorldShape::overworld();
    }

    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }

    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         air_{};
    registry::BlockStateId         floor_{};
};

class Light final : public LightSource {
public:
    u8 block{0};
    u8 sky{0};
    u8 darken{11};

    [[nodiscard]] u8 block_light(BlockPos) const override { return block; }

    [[nodiscard]] u8 sky_light(BlockPos) const override { return sky; }

    [[nodiscard]] u8 sky_darken() const override { return darken; }
};

/// West of x = 0 is biome 1, east of it biome 2.
class Halves final : public BiomeLookup {
public:
    [[nodiscard]] u16 biome_at(BlockPos pos) const override { return pos.x < 0 ? 1 : 2; }
};

struct Measured {
    std::string_view type;
    f64              stroll;
};

}  // namespace

TEST_CASE("the walk law: grass, packed ice and blue ice", "[mobs2][parity]") {
    // A chasing zombie, s = 0.23, measured on the three floors. The law's
    // dependence on the floor is what tells it from "the attribute halved",
    // which predicts one number for all three.
    CHECK(walk_blocks_per_tick_on(0.23, 0.6) == Catch::Approx(0.1141894).epsilon(0.0005));
    CHECK(walk_blocks_per_tick_on(0.23, 0.98) == Catch::Approx(0.1099586).epsilon(0.0005));
    CHECK(walk_blocks_per_tick_on(0.23, 0.989) == Catch::Approx(0.1157442).epsilon(0.0005));
    CHECK(kWalkLaw == Catch::Approx(2.1586).epsilon(0.0005));
}

TEST_CASE("every species strolls at its measured cruise speed", "[mobs2][parity]") {
    // Blocks per tick on bare grass, vanilla 1.20.1, the top plateau of each
    // species' steps (scripts/measure_mobs2.py stroll).
    constexpr Measured kStroll[] = {
        {"minecraft:zombie", 0.11417},  {"minecraft:skeleton", 0.13488},
        {"minecraft:creeper", 0.08633}, {"minecraft:spider", 0.12429},
        {"minecraft:cow", 0.08632},     {"minecraft:pig", 0.13482},
        {"minecraft:sheep", 0.11415},   {"minecraft:chicken", 0.13485},
        {"minecraft:husk", 0.11417},    {"minecraft:stray", 0.13485},
        {"minecraft:drowned", 0.11416}, {"minecraft:cave_spider", 0.12426},
        {"minecraft:witch", 0.13484},   {"minecraft:enderman", 0.19412},
        {"minecraft:rabbit", 0.13175},  {"minecraft:wolf", 0.19416},
        {"minecraft:fox", 0.19392},     {"minecraft:cat", 0.12432},
        {"minecraft:horse", 0.05354},
    };
    for (const Measured& row : kStroll) {
        const MobKind* kind = mob_kind(row.type);
        REQUIRE(kind != nullptr);
        INFO(row.type);
        CHECK(kind->speed(kind->stroll) == Catch::Approx(row.stroll).epsilon(0.005));
    }
}

TEST_CASE("panic and chase speeds, where the rig could provoke them", "[mobs2][parity]") {
    // Panic: hurt by the probe every four seconds. Knockback contaminates the
    // steps, so the tolerance is 2 %.
    constexpr Measured kPanic[] = {
        {"minecraft:cow", 0.34150},     {"minecraft:pig", 0.20937},   {"minecraft:sheep", 0.17771},
        {"minecraft:chicken", 0.26172}, {"minecraft:horse", 0.15696}, {"minecraft:cat", 0.42933},
    };
    for (const Measured& row : kPanic) {
        const MobKind* kind = mob_kind(row.type);
        REQUIRE(kind != nullptr);
        INFO(row.type);
        CHECK(kind->speed(kind->panic) == Catch::Approx(row.stroll).epsilon(0.02));
    }
    // Chase, four of each species thirty blocks from a survival probe.
    constexpr Measured kChase[] = {
        {"minecraft:zombie", 0.11419},
        {"minecraft:husk", 0.11419},
        {"minecraft:drowned", 0.11419},
    };
    for (const Measured& row : kChase) {
        const MobKind* kind = mob_kind(row.type);
        REQUIRE(kind != nullptr);
        INFO(row.type);
        CHECK(kind->speed(kind->chase) == Catch::Approx(row.stroll).epsilon(0.002));
    }
    // A provoked enderman: 0.43708 b/t, which is s = 0.45 — its 0.3 plus the
    // +0.15 of its attack boost, at modifier 1.0.
    CHECK(walk_blocks_per_tick(0.45) == Catch::Approx(0.43708).epsilon(0.002));
}

TEST_CASE("a monster's darkness is a draw, and the sky takes part", "[mobs2][spawn]") {
    Light                    light;
    math::LegacyRandomSource random{2024};
    const auto               rate = [&] {
        i32 yes = 0;
        for (i32 i = 0; i < 20000; ++i) {
            yes += monster_dark_enough(light, {0, 0, 0}, random) ? 1 : 0;
        }
        return static_cast<f64>(yes) / 20000.0;
    };

    // Sealed and dark: always.
    light.sky   = 0;
    light.block = 0;
    CHECK(rate() == 1.0);
    CHECK(monster_light_possible(light, {0, 0, 0}));

    // Any block light: never — the dimension's limit is 0 (what mobs.md measured).
    light.block = 1;
    CHECK(rate() == 0.0);
    CHECK_FALSE(monster_light_possible(light, {0, 0, 0}));
    light.block = 0;

    // Open sky at midnight: sky 15 must not beat a draw in 0..31 (17/32), and
    // the light level 15 - 11 = 4 must not beat a draw in 0..7 (4/8).
    light.sky    = 15;
    light.darken = 11;
    CHECK(rate() == Catch::Approx(17.0 / 32.0 * 4.0 / 8.0).margin(0.015));
    CHECK(monster_light_possible(light, {0, 0, 0}));

    // Open sky at noon: light 15, above anything the provider draws.
    light.darken = 0;
    CHECK(rate() == 0.0);
    CHECK_FALSE(monster_light_possible(light, {0, 0, 0}));
}

TEST_CASE("each type asks for its own floor and sky", "[mobs2][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Light light;
    light.sky    = 15;
    light.darken = 11;
    const std::vector<Vec3d>    players{Vec3d{0.5, 0.0, 0.5}};
    const std::vector<ChunkPos> ticking{ChunkPos{0, 0}};
    const std::vector<i32>      live(8, 0);
    SpawnEnvironment            environment;
    environment.light             = &light;
    environment.players           = players;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();
    NaturalSpawner spawner{5};

    Ground sand{*blocks(), "minecraft:sand"};
    environment.level = &sand;
    // A rabbit stands on sand (#rabbits_spawnable_on), a cow does not.
    CHECK(spawner.can_spawn_type_at(environment, MobCategory::Creature, "minecraft:rabbit",
                                    {60, 0, 0}, 0.4F, 0.5F));
    CHECK_FALSE(spawner.can_spawn_type_at(environment, MobCategory::Creature, "minecraft:cow",
                                          {60, 0, 0}, 0.9F, 1.4F));
    Ground snow{*blocks(), "minecraft:snow_block"};
    environment.level = &snow;
    CHECK(spawner.can_spawn_type_at(environment, MobCategory::Creature, "minecraft:wolf",
                                    {60, 0, 0}, 0.6F, 0.85F));

    // A husk needs the open sky: under a roof (sky light 0, dark) never,
    // while a zombie in the same dark spot always.
    Ground stone{*blocks(), "minecraft:stone"};
    environment.level = &stone;
    light.sky         = 0;
    i32 husks         = 0;
    i32 zombies       = 0;
    for (i32 i = 0; i < 400; ++i) {
        husks += spawner.can_spawn_type_at(environment, MobCategory::Monster, "minecraft:husk",
                                           {60, 0, 0}, 0.6F, 1.95F)
                     ? 1
                     : 0;
        zombies += spawner.can_spawn_type_at(environment, MobCategory::Monster, "minecraft:zombie",
                                             {60, 0, 0}, 0.6F, 1.95F)
                       ? 1
                       : 0;
    }
    CHECK(husks == 0);
    CHECK(zombies == 400);
    // Under the midnight sky, sometimes — the darkness draw.
    light.sky = 15;
    husks     = 0;
    for (i32 i = 0; i < 400; ++i) {
        husks += spawner.can_spawn_type_at(environment, MobCategory::Monster, "minecraft:husk",
                                           {60, 0, 0}, 0.6F, 1.95F)
                     ? 1
                     : 0;
    }
    CHECK(husks > 60);
    CHECK(husks < 160);
}

TEST_CASE("the type is drawn from the biome of the position", "[mobs2][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Ground                stone{*blocks(), "minecraft:stone"};
    Light                 light;
    Halves                halves;
    std::vector<ChunkPos> ticking;
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            ticking.emplace_back(x, z);
        }
    }
    const std::vector<Vec3d> players{Vec3d{0.5, 0.0, 0.5}};
    const std::vector<i32>   live(8, 0);
    SpawnEnvironment         environment;
    environment.level             = &stone;
    environment.light             = &light;
    environment.players           = players;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();
    environment.biomes            = &halves;

    const SpawnerEntry west[] = {SpawnerEntry{"minecraft:zombie", 100, 1, 1}};
    const SpawnerEntry east[] = {SpawnerEntry{"minecraft:skeleton", 100, 1, 1}};
    NaturalSpawner     spawner{11};
    spawner.set_biome_entries(1, MobCategory::Monster, west);
    spawner.set_biome_entries(2, MobCategory::Monster, east);

    std::vector<SpawnRequest> out;
    for (i32 pass = 0; pass < 10; ++pass) {
        spawner.spawn_tick(environment, out);
    }
    REQUIRE(out.size() > 5);
    for (const SpawnRequest& request : out) {
        INFO(request.position.x);
        CHECK(request.type_name ==
              (request.position.x < 0.0 ? "minecraft:zombie" : "minecraft:skeleton"));
    }
}

TEST_CASE("a slime is 1, 2 or 4, and splits into 2 to 4 of half its size", "[mobs2][slime]") {
    math::LegacyRandomSource random{77};
    i32                      seen[5] = {0, 0, 0, 0, 0};
    for (i32 i = 0; i < 3000; ++i) {
        const i32 size = draw_slime_size(random, 0.0F);
        REQUIRE((size == 1 || size == 2 || size == 4));
        ++seen[size];
    }
    // No special multiplier (easy, normal): one exponent draw in three each.
    CHECK(seen[1] > 850);
    CHECK(seen[2] > 850);
    CHECK(seen[4] > 850);

    // The measured box of size 1, and proportional; health size².
    CHECK(slime_side(1) == Catch::Approx(0.5202F));
    CHECK(slime_side(4) == Catch::Approx(2.0808F));
    CHECK(slime_health(4) == 16.0F);
    CHECK(slime_speed(1) == Catch::Approx(0.3));

    std::vector<SlimeChild> children;
    slime_children(1, random, children);
    CHECK(children.empty());
    i32 counts[5] = {0, 0, 0, 0, 0};
    for (i32 i = 0; i < 600; ++i) {
        slime_children(4, random, children);
        REQUIRE(children.size() >= 2);
        REQUIRE(children.size() <= 4);
        ++counts[children.size()];
        for (const SlimeChild& child : children) {
            CHECK(child.size == 2);
            CHECK(std::abs(child.offset.x) <= 0.5 + 1e-9);
            CHECK(std::abs(child.offset.z) <= 0.5 + 1e-9);
        }
    }
    CHECK(counts[2] > 150);
    CHECK(counts[3] > 150);
    CHECK(counts[4] > 150);
}

TEST_CASE("a zombie drowns in 30 seconds and converts in 15 more", "[mobs2][conversion]") {
    DrowningState state;
    i32           started   = -1;
    i32           converted = -1;
    for (i32 tick = 1; tick <= 1000 && converted < 0; ++tick) {
        const DrowningEvent event = drowning_tick(state, true);
        if (event == DrowningEvent::Started) {
            started = tick;
        } else if (event == DrowningEvent::Converted) {
            converted = tick;
        }
    }
    CHECK(started == 601);
    CHECK(converted == 902);

    // Surfacing before the 30 seconds resets the count; after, it does not.
    DrowningState bobbing;
    for (i32 tick = 0; tick < 599; ++tick) {
        (void)drowning_tick(bobbing, true);
    }
    (void)drowning_tick(bobbing, false);
    CHECK(bobbing.in_water == -1);
    DrowningState committed;
    for (i32 tick = 0; tick < 601; ++tick) {
        (void)drowning_tick(committed, true);
    }
    CHECK(committed.converting >= 0);
    (void)drowning_tick(committed, false);
    CHECK(committed.converting >= 0);
}

TEST_CASE("slime chunks are one in ten, and the moon has eight phases", "[mobs2][spawn]") {
    i32 slimy = 0;
    for (i32 x = -50; x < 50; ++x) {
        for (i32 z = -50; z < 50; ++z) {
            slimy += is_slime_chunk(1234567890, x, z) ? 1 : 0;
        }
    }
    // 10 000 chunks, p = 0.1: 1000 ± 30 (σ); three sigma either way.
    CHECK(slimy > 910);
    CHECK(slimy < 1090);
    CHECK(is_slime_chunk(42, 3, -7) == is_slime_chunk(42, 3, -7));
    CHECK(moon_brightness(0) == 1.0F);
    CHECK(moon_brightness(4 * 24000 + 100) == 0.0F);
    CHECK(moon_brightness(9 * 24000) == 0.75F);
}
