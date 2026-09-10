// Where mobs come from, and where they go.
//
// Every rule here is one that fails silently if it is wrong: a light threshold
// off by one gives a world that is merely a bit too dark or a bit too empty, a
// cap that does not scale gives a server that either has nothing in it or falls
// over. So each is exercised on its own, against a level and a light field the
// test writes by hand.
//
// The light thresholds and the caps come from scripts/measure_mobs.py; see
// docs/provenance/mobs.md for how, and for which of the numbers below are
// measured and which are ours.
#include "ov/gameplay/spawning.hpp"

#include "ov/registry/registries.hpp"

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

class GroundLevel final : public world::LevelView {
public:
    explicit GroundLevel(const registry::BlockRegistry& registry, std::string_view floor)
        : registry_{&registry} {
        air_   = registry.default_state(registry.find_block("minecraft:air").value());
        floor_ = registry.default_state(registry.find_block(floor).value());
    }

    /// Everything below y = 0 is the floor block; above it, air.
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        return pos.y < 0 ? floor_ : air_;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return loaded; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

    bool loaded{true};

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         air_{};
    registry::BlockStateId         floor_{};
};

class FixedLight final : public LightSource {
public:
    u8 block{0};
    u8 sky{0};
    u8 darken{11};

    [[nodiscard]] u8 block_light(BlockPos) const override { return block; }
    [[nodiscard]] u8 sky_light(BlockPos) const override { return sky; }
    [[nodiscard]] u8 sky_darken() const override { return darken; }
};

}  // namespace

TEST_CASE("a type's category is looked up, and an unknown one is Misc rather than a guess",
          "[gameplay][spawn]") {
    CHECK(category_of("minecraft:zombie") == MobCategory::Monster);
    CHECK(category_of("minecraft:cow") == MobCategory::Creature);
    CHECK(category_of("minecraft:bat") == MobCategory::Ambient);
    CHECK(category_of("minecraft:squid") == MobCategory::WaterCreature);
    CHECK(category_of("minecraft:cod") == MobCategory::WaterAmbient);
    // Not in the table: `Misc`, which never spawns naturally. The safe
    // direction — a missing mob rather than a chicken in a cave at midnight.
    CHECK(category_of("minecraft:item") == MobCategory::Misc);
    CHECK(category_of("minecraft:not_a_mob") == MobCategory::Misc);
}

TEST_CASE("the category cap scales with how much world is ticked", "[gameplay][spawn]") {
    // A full 17x17 spawn square is the denominator, so a server ticking exactly
    // that many chunks gets the whole cap.
    CHECK(NaturalSpawner::effective_cap(MobCategory::Monster, 17 * 17) == 70);
    CHECK(NaturalSpawner::effective_cap(MobCategory::Creature, 17 * 17) == 10);

    // Half the world ticked, half the mobs.
    CHECK(NaturalSpawner::effective_cap(MobCategory::Monster, 17 * 17 / 2) == 34);

    // The multiply happens before the divide. Dividing first would round a
    // small server's cap to zero and it would hold no mobs at all — a bug that
    // looks exactly like "spawning is broken".
    CHECK(NaturalSpawner::effective_cap(MobCategory::Monster, 100) == 24);
    CHECK(NaturalSpawner::effective_cap(MobCategory::Creature, 100) == 3);

    // Misc has no cap and never spawns.
    CHECK(NaturalSpawner::effective_cap(MobCategory::Misc, 17 * 17) == 0);
}

TEST_CASE("effective light takes the larger of block light and dimmed sky light",
          "[gameplay][spawn]") {
    FixedLight light;
    // Midnight: the sky contributes 15 - 11 = 4.
    light.sky    = 15;
    light.block  = 0;
    light.darken = 11;
    CHECK(light.effective_light({0, 0, 0}) == 4);

    // Noon: the sky contributes all of it.
    light.darken = 0;
    CHECK(light.effective_light({0, 0, 0}) == 15);

    // A torch under a sealed roof: block light alone.
    light.sky    = 0;
    light.block  = 7;
    light.darken = 11;
    CHECK(light.effective_light({0, 0, 0}) == 7);
}

TEST_CASE("a monster needs darkness, a floor, headroom and distance", "[gameplay][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    GroundLevel level{*blocks(), "minecraft:stone"};
    FixedLight  light;
    light.sky   = 0;
    light.block = 0;

    const std::vector<Vec3d>    players{Vec3d{0.5, 0.0, 0.5}};
    const std::vector<ChunkPos> ticking{ChunkPos{0, 0}};
    const std::vector<i32>      live(8, 0);

    SpawnEnvironment environment;
    environment.level             = &level;
    environment.light             = &light;
    environment.players           = players;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();

    NaturalSpawner spawner{1};

    // Far from the player, dark, on stone: yes.
    CHECK(spawner.can_spawn_at(environment, MobCategory::Monster, {60, 0, 0}, 0.6F, 1.95F));

    // Too close to the player: no, whatever else is true.
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {10, 0, 0}, 0.6F, 1.95F));

    // Any light at all: no. The threshold is zero, measured.
    light.block = 1;
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {60, 0, 0}, 0.6F, 1.95F));
    light.block = 0;

    // No floor under it: no.
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {60, 8, 0}, 0.6F, 1.95F));

    // Inside the ground: no.
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {60, -8, 0}, 0.6F, 1.95F));

    // An unloaded chunk is refused rather than treated as air: spawning into
    // one would make the world depend on which chunks happened to be resident.
    level.loaded = false;
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {60, 0, 0}, 0.6F, 1.95F));
    level.loaded = true;

    // No light source at all: refused, not assumed dark.
    environment.light = nullptr;
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {60, 0, 0}, 0.6F, 1.95F));
}

TEST_CASE("an animal needs grass and sky, and does not care about darkness",
          "[gameplay][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    FixedLight light;
    light.sky   = 15;
    light.block = 0;

    const std::vector<Vec3d>    players{Vec3d{0.5, 0.0, 0.5}};
    const std::vector<ChunkPos> ticking{ChunkPos{0, 0}};
    const std::vector<i32>      live(8, 0);

    SpawnEnvironment environment;
    environment.light             = &light;
    environment.players           = players;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();

    NaturalSpawner spawner{1};

    GroundLevel grass{*blocks(), "minecraft:grass_block"};
    environment.level = &grass;
    CHECK(spawner.can_spawn_at(environment, MobCategory::Creature, {60, 0, 0}, 0.9F, 1.4F));

    // Midnight is irrelevant to a cow — sky *light* is what it needs, not
    // daylight, and the stored sky light does not change with the clock.
    light.darken = 11;
    CHECK(spawner.can_spawn_at(environment, MobCategory::Creature, {60, 0, 0}, 0.9F, 1.4F));

    // Under a roof: no sky light, no cow.
    light.sky = 3;
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Creature, {60, 0, 0}, 0.9F, 1.4F));
    light.sky = 15;

    // Stone rather than grass: no cow, and a monster in the same place is fine.
    GroundLevel stone{*blocks(), "minecraft:stone"};
    environment.level = &stone;
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Creature, {60, 0, 0}, 0.9F, 1.4F));
}

TEST_CASE("nothing spawns without players, and the cap is respected",
          "[gameplay][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    GroundLevel level{*blocks(), "minecraft:stone"};
    FixedLight  light;

    std::vector<ChunkPos> ticking;
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            ticking.emplace_back(x, z);
        }
    }
    const std::vector<Vec3d> players{Vec3d{0.5, 0.0, 0.5}};
    std::vector<i32>         live(8, 0);

    const SpawnerEntry monsters[] = {
        SpawnerEntry{"minecraft:zombie", 100, 4, 4},
        SpawnerEntry{"minecraft:skeleton", 100, 4, 4},
    };

    SpawnEnvironment environment;
    environment.level             = &level;
    environment.light             = &light;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();

    NaturalSpawner            spawner{4242};
    spawner.set_entries(MobCategory::Monster, monsters);
    std::vector<SpawnRequest> out;

    SECTION("no players, nothing at all") {
        // The game's rule, not an optimisation: the vanilla spawner runs over
        // the chunks a player ticket reaches. Learned the hard way — a whole
        // run of scripts/measure_mobs.py read zero everywhere before the rig
        // knew to keep a probe client connected.
        environment.players = {};
        spawner.spawn_tick(environment, out);
        CHECK(out.empty());
    }

    SECTION("with a player, monsters appear, and never more than the cap") {
        environment.players = players;
        for (int pass = 0; pass < 20; ++pass) {
            spawner.spawn_tick(environment, out);
        }
        REQUIRE_FALSE(out.empty());
        CHECK(static_cast<i32>(out.size()) <=
              NaturalSpawner::effective_cap(MobCategory::Monster, ticking.size()) * 20);
        for (const SpawnRequest& request : out) {
            CHECK(request.category == MobCategory::Monster);
            const f64 dx = request.position.x - players.front().x;
            const f64 dz = request.position.z - players.front().z;
            CHECK(dx * dx + dz * dz >=
                  NaturalSpawner::kMinimumPlayerDistance *
                          NaturalSpawner::kMinimumPlayerDistance -
                      1.0);
        }
    }

    SECTION("already at the cap, nothing more") {
        environment.players   = players;
        live[static_cast<usize>(MobCategory::Monster)] =
            NaturalSpawner::effective_cap(MobCategory::Monster, ticking.size());
        environment.live_per_category = live;
        spawner.spawn_tick(environment, out);
        CHECK(out.empty());
    }
}

TEST_CASE("a pack is a pack", "[gameplay][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    GroundLevel level{*blocks(), "minecraft:grass_block"};
    FixedLight  light;
    light.sky = 15;

    std::vector<ChunkPos> ticking;
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            ticking.emplace_back(x, z);
        }
    }
    const std::vector<Vec3d> players{Vec3d{0.5, 0.0, 0.5}};
    const std::vector<i32>   live(8, 0);
    const SpawnerEntry       creatures[] = {SpawnerEntry{"minecraft:cow", 100, 4, 4}};

    SpawnEnvironment environment;
    environment.level             = &level;
    environment.light             = &light;
    environment.players           = players;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();

    NaturalSpawner spawner{7};
    spawner.set_entries(MobCategory::Creature, creatures);
    std::vector<SpawnRequest> out;
    spawner.spawn_tick(environment, out);
    REQUIRE_FALSE(out.empty());

    // Members of one pack land near each other, which is the whole point of a
    // pack: four unrelated cows scattered over a chunk is not a herd.
    const i32 pack = out.front().pack;
    Vec3d     first = out.front().position;
    i32       members = 0;
    for (const SpawnRequest& request : out) {
        if (request.pack != pack) {
            continue;
        }
        ++members;
        CHECK(std::abs(request.position.x - first.x) <= 5.5);
        CHECK(std::abs(request.position.z - first.z) <= 5.5);
    }
    CHECK(members >= 1);
}

TEST_CASE("despawn: immediate far away, never when persistent, random in between",
          "[gameplay][spawn]") {
    math::LegacyRandomSource random{99};

    // Past the category's distance: gone, now.
    CHECK(decide_despawn(MobCategory::Monster, 200.0, false, 10000, random) ==
          DespawnDecision::Immediate);

    // A named or command-spawned mob never goes, however far.
    CHECK(decide_despawn(MobCategory::Monster, 200.0, true, 10000, random) ==
          DespawnDecision::Keep);

    // Close in: kept.
    CHECK(decide_despawn(MobCategory::Monster, 10.0, false, 10000, random) ==
          DespawnDecision::Keep);

    // In between, but not idle long enough yet: kept.
    CHECK(decide_despawn(MobCategory::Monster, 64.0, false, 10, random) == DespawnDecision::Keep);

    // In between and idle: the random branch fires eventually, and the rate is
    // roughly one in kRandomDespawnOdds.
    i32 removed = 0;
    for (i32 attempt = 0; attempt < 40000; ++attempt) {
        if (decide_despawn(MobCategory::Monster, 64.0, false, 10000, random) ==
            DespawnDecision::Random) {
            ++removed;
        }
    }
    CHECK(removed > 20);
    CHECK(removed < 80);

    // An item is Misc: never removed by distance, or every dropped stack would
    // vanish the moment a player walked away.
    CHECK(decide_despawn(MobCategory::Misc, 5000.0, false, 100000, random) ==
          DespawnDecision::Keep);
}

TEST_CASE("water categories want water, not a floor", "[gameplay][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    GroundLevel stone{*blocks(), "minecraft:stone"};
    GroundLevel ocean{*blocks(), "minecraft:water"};
    FixedLight  light;

    const std::vector<Vec3d>    players{Vec3d{0.5, 0.0, 0.5}};
    const std::vector<ChunkPos> ticking{ChunkPos{0, 0}};
    const std::vector<i32>      live(8, 0);

    SpawnEnvironment environment;
    environment.light             = &light;
    environment.players           = players;
    environment.ticking_chunks    = ticking;
    environment.live_per_category = live;
    environment.registries        = registries();

    NaturalSpawner spawner{3};

    environment.level = &stone;
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::WaterCreature, {60, -4, 0}, 0.8F,
                                     0.8F));
    environment.level = &ocean;
    CHECK(spawner.can_spawn_at(environment, MobCategory::WaterCreature, {60, -4, 0}, 0.8F, 0.8F));
}

TEST_CASE("a position inside a solid block is refused before a mob is chosen",
          "[gameplay][spawn]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    // The cheap half of the position test, and the only part of it that
    // rejects anything in bulk. Everything else `position_plausible` asks —
    // loaded, in range, far from a player, dark enough — is true nearly
    // everywhere; a block that stops movement is what actually turns attempts
    // away, and it needs no hitbox to decide.
    GroundLevel level{*blocks(), "minecraft:stone"};
    FixedLight  light;
    light.block = 0;
    light.sky   = 0;

    SpawnEnvironment environment;
    environment.level      = &level;
    environment.light      = &light;
    environment.registries = registries();

    NaturalSpawner spawner{1};
    // Inside the stone: refused without asking what kind of mob it would be.
    CHECK_FALSE(spawner.position_plausible(environment, MobCategory::Monster, {40, -8, 40}));
    // In the air above it: the cheap half has no objection. Whether a mob fits
    // there is `can_spawn_at`'s question, and it needs a size.
    CHECK(spawner.position_plausible(environment, MobCategory::Monster, {40, 8, 40}));
    // And the same air cell fails the full test, because there is no floor
    // under it — which is the rejection that still costs a type draw.
    CHECK_FALSE(spawner.can_spawn_at(environment, MobCategory::Monster, {40, 8, 40}, 0.6F, 1.95F));
}
