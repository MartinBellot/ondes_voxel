// ── mobs-4 ── A mob's status effects on the server: the table beside the
// entity world (mob_effects.hpp). The rules are effects.hpp's and tested
// there against the real server; what is tested here is that a mob gets them —
// its body, its window, its walk, its metadata and its Anvil record.
#include "../src/mob_combat.hpp"
#include "../src/mob_effects.hpp"

#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <memory>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

struct Packet {
    i32             id;
    std::vector<u8> payload;
};

/// A world with one mob of each species asked for, each with its brain.
struct Scene {
    entity::EntityWorld world{*registries(), 1000};
    MobCombat           combat{*registries(), nullptr};
    MobEffects          effects{*registries(), &combat};
    std::vector<Packet> sent;
    MobEffectSink       sink = [this](i32 id, std::span<const u8> payload) {
        sent.push_back(Packet{id, {payload.begin(), payload.end()}});
    };

    [[nodiscard]] i32 spawn(std::string_view type) {
        const auto handle = world.spawn(type, Vec3d{0.5, 64.0, 0.5}, net::Uuid{});
        REQUIRE(handle.has_value());
        const entity::EntityState* state = world.state(*handle);
        const gameplay::MobKind*   kind  = gameplay::mob_kind(type);
        REQUIRE(kind != nullptr);
        world.set_logic(*handle, std::make_unique<gameplay::Mob>(*kind, state->width, state->height,
                                                                 state->network_id));
        return state->network_id;
    }
    [[nodiscard]] entity::EntityState& state(i32 id) { return *world.mutable_state(world.find(id)); }
    [[nodiscard]] const gameplay::MobBrain& brain(i32 id) {
        return dynamic_cast<gameplay::Mob*>(world.logic(world.find(id)))->brain();
    }
};

[[nodiscard]] gameplay::EffectInstance of(gameplay::Effect effect, i32 duration, u8 amplifier = 0) {
    gameplay::EffectInstance instance;
    instance.effect    = effect;
    instance.duration  = duration;
    instance.amplifier = amplifier;
    return instance;
}

}  // namespace

TEST_CASE("a mob's body refuses what its species refuses", "[server][mobs4]") {
    REQUIRE(registries() != nullptr);
    Scene     scene;
    const i32 spider = scene.spawn("minecraft:spider");
    const i32 zombie = scene.spawn("minecraft:zombie");
    const i32 cow    = scene.spawn("minecraft:cow");

    CHECK(scene.effects.apply(scene.world, spider, of(gameplay::Effect::Poison, 100), scene.sink) ==
          gameplay::AddResult::Immune);
    CHECK(scene.effects.apply(scene.world, zombie, of(gameplay::Effect::Regeneration, 100),
                              scene.sink) == gameplay::AddResult::Immune);
    CHECK(scene.effects.apply(scene.world, cow, of(gameplay::Effect::Poison, 100), scene.sink) ==
          gameplay::AddResult::Added);
    // A refusal leaves nothing behind.
    CHECK(scene.effects.effects_of(spider) == nullptr);
    CHECK(scene.effects.carriers() == 1);
    // Not a mob of this world.
    CHECK_FALSE(scene.effects.apply(scene.world, 424242, of(gameplay::Effect::Speed, 100), scene.sink));
}

TEST_CASE("poison hurts a cow through its window, and stops at one", "[server][mobs4]") {
    Scene     scene;
    const i32 cow = scene.spawn("minecraft:cow");
    REQUIRE(scene.state(cow).health == 10.0F);
    (void)scene.effects.apply(scene.world, cow, of(gameplay::Effect::Poison, 1001), scene.sink);
    // Poison I acts when what is left is a multiple of 25: at 1000, then 975.
    // One point each, and never the last.
    for (i32 i = 0; i < 26; ++i) {
        scene.combat.tick(gameplay::DamageConstants{});
        scene.effects.tick(scene.world, scene.sink);
    }
    CHECK(scene.state(cow).health == 9.0F);
    REQUIRE(scene.effects.hurts().size() == 1);
    CHECK(scene.effects.hurts()[0].id == cow);
    CHECK(scene.effects.hurts()[0].kind == gameplay::DamageKind::Magic);
    CHECK_FALSE(scene.effects.hurts()[0].killed);
    scene.effects.clear_hurts();
    for (i32 i = 0; i < 400; ++i) {
        scene.combat.tick(gameplay::DamageConstants{});
        scene.effects.tick(scene.world, scene.sink);
    }
    CHECK(scene.state(cow).health == 1.0F);
    // Every change of health went to the watchers.
    bool health_sent = false;
    for (const Packet& packet : scene.sent) {
        health_sent = health_sent || packet.id == net::clientbound::kEntityMetadata;
    }
    CHECK(health_sent);
}

TEST_CASE("instant health kills what is undead, and the kill is the server's", "[server][mobs4]") {
    Scene     scene;
    const i32 zombie = scene.spawn("minecraft:zombie");
    scene.state(zombie).health = 5.0F;
    // Instant health II on the undead: 12 of harm.
    CHECK(scene.effects.apply(scene.world, zombie, of(gameplay::Effect::InstantHealth, 1, 1),
                              scene.sink) == gameplay::AddResult::Applied);
    REQUIRE(scene.effects.hurts().size() == 1);
    CHECK(scene.effects.hurts()[0].killed);
    CHECK(scene.state(zombie).health == 0.0F);
    CHECK_FALSE(scene.state(zombie).removed);  // the server plays the death

    // Instant damage heals it instead, up to its maximum.
    const i32 other = scene.spawn("minecraft:zombie");
    scene.state(other).health = 5.0F;
    (void)scene.effects.apply(scene.world, other, of(gameplay::Effect::InstantDamage, 1), scene.sink);
    CHECK(scene.state(other).health == 9.0F);
}

TEST_CASE("speed and slowness are the square of the attribute's ratio", "[server][mobs4]") {
    using Catch::Matchers::WithinRel;
    // The modifier amounts are the game's floats widened to double — measured
    // on the wire, Speed II's is 0.4000000059604645 (effets.md § 15) — so the
    // factor is (1 + 0.2f)², not a rounder 1.44.
    const auto squared = [](f64 k) { return k * k; };
    Scene     scene;
    const i32 zombie = scene.spawn("minecraft:zombie");
    CHECK(scene.brain(zombie).effect_walk == 1.0);
    (void)scene.effects.apply(scene.world, zombie, of(gameplay::Effect::Speed, 100), scene.sink);
    CHECK_THAT(scene.brain(zombie).effect_walk,
               WithinRel(squared(1.0 + static_cast<f64>(0.2F)), 1e-12));
    (void)scene.effects.apply(scene.world, zombie, of(gameplay::Effect::Speed, 100, 1), scene.sink);
    CHECK_THAT(scene.brain(zombie).effect_walk,
               WithinRel(squared(1.0 + static_cast<f64>(0.4F)), 1e-12));
    CHECK(scene.effects.clear(scene.world, zombie, scene.sink) == usize{1});
    CHECK(scene.brain(zombie).effect_walk == 1.0);
    CHECK(scene.effects.carriers() == 0);

    (void)scene.effects.apply(scene.world, zombie, of(gameplay::Effect::Slowness, 3), scene.sink);
    CHECK_THAT(scene.brain(zombie).effect_walk,
               WithinRel(squared(1.0 - static_cast<f64>(0.15F)), 1e-12));
    for (i32 i = 0; i < 4; ++i) {
        scene.effects.tick(scene.world, scene.sink);
    }
    CHECK(scene.brain(zombie).effect_walk == 1.0);
    CHECK(scene.effects.carriers() == 0);
}

TEST_CASE("health boost moves the maximum and clamps on its way out", "[server][mobs4]") {
    Scene     scene;
    const i32 cow = scene.spawn("minecraft:cow");
    (void)scene.effects.apply(scene.world, cow, of(gameplay::Effect::HealthBoost, 2, 1), scene.sink);
    CHECK(scene.state(cow).max_health == 18.0F);
    scene.state(cow).health = 18.0F;
    for (i32 i = 0; i < 3; ++i) {
        scene.effects.tick(scene.world, scene.sink);
    }
    CHECK(scene.state(cow).max_health == 10.0F);
    CHECK(scene.state(cow).health == 10.0F);
}

TEST_CASE("the colour goes to the watchers, and comes back to a late one", "[server][mobs4]") {
    Scene     scene;
    const i32 cow = scene.spawn("minecraft:cow");
    // Measured (`measure_hostile.py packets`): Speed on a cow told its
    // watchers the colour and the speed attribute — never an Entity Effect.
    (void)scene.effects.apply(scene.world, cow, of(gameplay::Effect::Speed, 100), scene.sink);
    REQUIRE(scene.sent.size() == 2);
    CHECK(scene.sent[0].id == net::clientbound::kEntityMetadata);
    CHECK(scene.sent[1].id == net::clientbound::kUpdateAttributes);
    for (const Packet& packet : scene.sent) {
        CHECK(packet.id != net::clientbound::kEntityEffect);
        CHECK(packet.id != net::clientbound::kRemoveEntityEffect);
    }
    // A late joiner: the same two.
    std::vector<Packet> late;
    scene.effects.pairing(scene.state(cow), [&](i32 id, std::span<const u8> payload) {
        late.push_back(Packet{id, {payload.begin(), payload.end()}});
    });
    REQUIRE(late.size() == 2);
    CHECK(late[0].payload == scene.sent[0].payload);
    CHECK(late[1].payload == scene.sent[1].payload);

    // A poison alone moves no attribute: no Update Attributes (measured).
    Scene     other;
    const i32 poisoned = other.spawn("minecraft:cow");
    gameplay::EffectInstance hidden = of(gameplay::Effect::Poison, 600);
    hidden.visible   = false;
    hidden.show_icon = false;
    (void)other.effects.apply(other.world, poisoned, hidden, other.sink);
    for (const Packet& packet : other.sent) {
        CHECK(packet.id != net::clientbound::kUpdateAttributes);
    }
    // Clearing the speed sends the attribute back without its modifier.
    scene.sent.clear();
    (void)scene.effects.clear(scene.world, cow, scene.sink);
    bool attributes_sent = false;
    for (const Packet& packet : scene.sent) {
        attributes_sent = attributes_sent || packet.id == net::clientbound::kUpdateAttributes;
    }
    CHECK(attributes_sent);
}

TEST_CASE("ActiveEffects in the Anvil record, and back", "[server][mobs4]") {
    Scene     scene;
    const i32 cow = scene.spawn("minecraft:cow");
    gameplay::EffectInstance boost = of(gameplay::Effect::HealthBoost, 40000, 1);
    (void)scene.effects.apply(scene.world, cow, boost, scene.sink);
    (void)scene.effects.apply(scene.world, cow, of(gameplay::Effect::Speed, 30000, 2), scene.sink);
    scene.state(cow).health = 17.0F;

    nbt::Tag out = nbt::Tag::make_compound();
    (void)out.put("ActiveEffects", nbt::Tag{i32{7}});  // a stale one from disk
    (void)out.put("Health", nbt::Tag{scene.state(cow).health});
    scene.effects.write(scene.state(cow), out);
    REQUIRE(out.find("ActiveEffects") != nullptr);
    REQUIRE(out.find("ActiveEffects")->list() != nullptr);
    CHECK(out.find("ActiveEffects")->list()->size() == 2);

    // A fresh world reads it back: the effects, the maximum, the health.
    Scene     back;
    const i32 again = back.spawn("minecraft:cow");
    back.state(again).health = 10.0F;  // what the storage's clamp left
    back.effects.read(back.world, back.state(again), out);
    REQUIRE(back.effects.effects_of(again) != nullptr);
    CHECK(back.effects.effects_of(again)->amplifier(gameplay::Effect::HealthBoost) == 1);
    CHECK(back.effects.effects_of(again)->get(gameplay::Effect::Speed)->duration == 30000);
    CHECK(back.state(again).max_health == 18.0F);
    CHECK(back.state(again).health == 17.0F);
    CHECK(back.brain(again).effect_walk > 1.9);

    // A mob without effects writes none, and takes a stale list away.
    nbt::Tag bare = nbt::Tag::make_compound();
    (void)bare.put("ActiveEffects", nbt::Tag{i32{7}});
    const i32 plain = back.spawn("minecraft:cow");
    back.effects.write(back.state(plain), bare);
    CHECK(bare.find("ActiveEffects") == nullptr);
}
