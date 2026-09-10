// What the server sends, to whom, and how far — against a recording host.
//
// Every rule under test was read off a capture of the real 1.20.1 server
// (docs/provenance/son.md). The host records who was left out and the radius,
// which is the half of the rule a packet dump alone does not show.
#include "../src/sounds.hpp"

#include "ov/protocol/chat.hpp"
#include "ov/protocol/sound.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

using namespace ov;
using namespace ov::server;
using Catch::Matchers::WithinAbs;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

const Loaded& loaded() {
    static const Loaded pack = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        if (auto b = registry::BlockRegistry::load(path)) {
            out.blocks.emplace(std::move(*b));
        }
        if (auto r = registry::Registries::load(path)) {
            out.registries.emplace(std::move(*r));
        }
        return out;
    }();
    REQUIRE(pack.blocks.has_value());
    REQUIRE(pack.registries.has_value());
    return pack;
}

struct Sent {
    const void*     except{nullptr};
    Vec3d           at{};
    f64             radius{0.0};
    i32             id{0};
    std::vector<u8> payload;
};

struct Recorder {
    std::vector<Sent> sent;
    SoundHost         host;
    Recorder() {
        host.send_near = [this](const void* except, Vec3d at, f64 radius, i32 id,
                                std::span<const u8> payload) {
            sent.push_back(Sent{except, at, radius, id, {payload.begin(), payload.end()}});
        };
    }
};

i32 sound_id(std::string_view name) {
    const auto& regs     = *loaded().registries;
    const auto  registry = regs.find("minecraft:sound_event");
    REQUIRE(registry.has_value());
    const auto id = regs.protocol_id(*registry, name);
    REQUIRE(id.has_value());
    return static_cast<i32>(*id);
}

registry::BlockStateId state_of(std::string_view block, std::string_view property = {},
                                std::string_view value = {}) {
    const auto& blocks = *loaded().blocks;
    const auto  id     = blocks.find_block(block);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks.default_state(*id);
    if (!property.empty()) {
        const auto view = blocks.find_property(*id, property);
        REQUIRE(view.has_value());
        const auto it = std::ranges::find(view->values, value);
        REQUIRE(it != view->values.end());
        state = blocks.with_property(state, *view,
                                     static_cast<u16>(std::distance(view->values.begin(), it)));
    }
    return state;
}

i32 entity_type(std::string_view name) {
    const auto& regs     = *loaded().registries;
    const auto  registry = regs.find("minecraft:entity_type");
    REQUIRE(registry.has_value());
    return static_cast<i32>(*regs.protocol_id(*registry, name));
}

net::SoundEffect effect(const Sent& sent) {
    REQUIRE(sent.id == net::clientbound::kSoundEffect);
    const auto parsed = net::parse_sound_effect(sent.payload);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("server sounds: a placed block, heard by all but its placer", "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    int      placer = 0;
    sounds.block_placed(rec.host, &placer, BlockPos{1, -60, 2}, state_of("minecraft:stone"));
    REQUIRE(rec.sent.size() == 1);
    CHECK(rec.sent[0].except == &placer);
    CHECK(rec.sent[0].radius == 16.0);
    const auto sound = effect(rec.sent[0]);
    // The capture's own packet: 1269, block, (12, -476, 20), 1.0, 0.8.
    CHECK(sound.sound.sound_id == sound_id("minecraft:block.stone.place"));
    CHECK(sound.category == net::sound_category::kBlock);
    CHECK(sound.x == 12);
    CHECK(sound.y == -476);
    CHECK(sound.z == 20);
    CHECK(sound.volume == 1.0F);
    CHECK(sound.pitch == 0.8F);
}

TEST_CASE("server sounds: a break is World Event 2001, the breaker left out", "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    int      breaker = 0;
    const auto stone = state_of("minecraft:stone");
    sounds.block_broken(rec.host, &breaker, BlockPos{1, -60, 2}, stone);
    sounds.block_broken(rec.host, &breaker, BlockPos{1, -60, 2}, registry::kAirState);
    REQUIRE(rec.sent.size() == 1);
    CHECK(rec.sent[0].except == &breaker);
    CHECK(rec.sent[0].id == net::clientbound::kWorldEvent);
    const auto event = net::parse_world_event(rec.sent[0].payload);
    REQUIRE(event.has_value());
    CHECK(event->event == net::kWorldEventBlockBreak);
    CHECK(event->data == static_cast<i32>(stone.value()));
    CHECK(event->y == -60);
}

TEST_CASE("server sounds: a clicked door leaves out the clicker, a ticked one nobody",
          "[server][sound]") {
    Sounds     sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder   rec;
    int        actor  = 0;
    const auto closed = state_of("minecraft:oak_door", "open", "false");
    const auto open   = state_of("minecraft:oak_door", "open", "true");
    sounds.block_changed(rec.host, &actor, BlockPos{2, -60, -2}, closed, open);
    REQUIRE(rec.sent.size() == 1);
    CHECK(rec.sent[0].except == &actor);
    CHECK(effect(rec.sent[0]).sound.sound_id == sound_id("minecraft:block.wooden_door.open"));

    // Moved by power: queued on the tick, sent to everyone on flush.
    sounds.queue_changed(BlockPos{2, -60, -2}, open, closed);
    sounds.flush(rec.host);
    REQUIRE(rec.sent.size() == 2);
    CHECK(rec.sent[1].except == nullptr);
    CHECK(effect(rec.sent[1]).sound.sound_id == sound_id("minecraft:block.wooden_door.close"));

    // The upper half of a door moved by a tick is the same door: silent.
    const auto& blocks     = *loaded().blocks;
    const auto  door       = *blocks.find_block("minecraft:oak_door");
    const auto  half       = *blocks.find_property(door, "half");
    const auto  upper_of   = [&](registry::BlockStateId state) {
        return blocks.with_property(
            state, half,
            static_cast<u16>(std::distance(half.values.begin(),
                                           std::ranges::find(half.values, "upper"))));
    };
    sounds.queue_changed(BlockPos{2, -59, -2}, upper_of(closed), upper_of(open));
    sounds.flush(rec.host);
    CHECK(rec.sent.size() == 2);

    // A change that is not a toggle — a door's `powered` alone — plays nothing.
    sounds.block_changed(rec.host, &actor, BlockPos{2, -60, -2}, closed,
                         state_of("minecraft:oak_door", "powered", "true"));
    CHECK(rec.sent.size() == 2);
}

TEST_CASE("server sounds: a lever is heard by everyone, at 0.3", "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    int      actor = 0;
    sounds.block_changed(rec.host, &actor, BlockPos{0, -60, -2},
                         state_of("minecraft:lever", "powered", "false"),
                         state_of("minecraft:lever", "powered", "true"));
    REQUIRE(rec.sent.size() == 1);
    CHECK(rec.sent[0].except == nullptr);
    const auto sound = effect(rec.sent[0]);
    CHECK(sound.sound.sound_id == sound_id("minecraft:block.lever.click"));
    CHECK_THAT(sound.volume, WithinAbs(0.3, 1e-6));
    CHECK_THAT(sound.pitch, WithinAbs(0.6, 1e-6));
}

TEST_CASE("server sounds: level 5, 10, 15 — and nothing between", "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    for (const i32 level : {1, 2, 4, 5, 30, 40}) {
        sounds.level_up(rec.host, Vec3d{0.5, -60.0, 0.5}, level);
    }
    REQUIRE(rec.sent.size() == 3);
    CHECK_THAT(effect(rec.sent[0]).volume, WithinAbs(0.125, 1e-6));  // captured at level 5
    CHECK_THAT(effect(rec.sent[1]).volume, WithinAbs(0.75, 1e-6));
    CHECK_THAT(effect(rec.sent[2]).volume, WithinAbs(0.75, 1e-6));
}

TEST_CASE("server sounds: a stride is six tenths of the way, one step a unit", "[server][sound]") {
    Sounds::Stride stride;
    int            steps = 0;
    for (int tick = 0; tick < 10; ++tick) {  // 2.158 blocks at walking speed
        steps += Sounds::advance(stride, 0.2158) ? 1 : 0;
    }
    CHECK(steps == 1);
    for (int tick = 0; tick < 90; ++tick) {  // 21.58 blocks in all
        steps += Sounds::advance(stride, 0.2158) ? 1 : 0;
    }
    CHECK(steps == 12);  // 21.58 * 0.6 = 12.9, first at 1: twelve crossings
}

TEST_CASE("server sounds: the swing — strong when charged, crit and weak otherwise",
          "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    const Vec3d at{0.5, -60.0, 0.5};
    sounds.player_attack(rec.host, at, false, false, false, 1.0F);  // the captured case
    sounds.player_attack(rec.host, at, true, false, false, 1.0F);
    sounds.player_attack(rec.host, at, false, false, false, 0.3F);
    REQUIRE(rec.sent.size() == 3);
    CHECK(rec.sent[0].except == nullptr);
    const auto strong = effect(rec.sent[0]);
    CHECK(strong.sound.sound_id == sound_id("minecraft:entity.player.attack.strong"));
    CHECK(strong.category == net::sound_category::kPlayer);
    CHECK(strong.volume == 1.0F);
    CHECK(strong.pitch == 1.0F);
    CHECK(effect(rec.sent[1]).sound.sound_id == sound_id("minecraft:entity.player.attack.crit"));
    CHECK(effect(rec.sent[2]).sound.sound_id == sound_id("minecraft:entity.player.attack.weak"));
}

TEST_CASE("server sounds: eating — mouthfuls for the others, the burp for all",
          "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    int      eater = 0;
    const Vec3d at{0.5, -60.0, 0.5};
    sounds.eating(rec.host, &eater, at);
    sounds.ate(rec.host, at);
    REQUIRE(rec.sent.size() == 3);
    CHECK(rec.sent[0].except == &eater);
    const auto bite = effect(rec.sent[0]);
    CHECK(bite.sound.sound_id == sound_id("minecraft:entity.generic.eat"));
    CHECK(bite.category == net::sound_category::kPlayer);
    CHECK((bite.volume == 0.5F || bite.volume == 1.0F));
    CHECK(rec.sent[1].except == nullptr);
    const auto burp = effect(rec.sent[1]);
    CHECK(burp.sound.sound_id == sound_id("minecraft:entity.player.burp"));
    CHECK(burp.volume == 0.5F);
    const auto last = effect(rec.sent[2]);
    CHECK(last.sound.sound_id == sound_id("minecraft:entity.generic.eat"));
    CHECK(last.category == net::sound_category::kNeutral);  // captured so
}

TEST_CASE("server sounds: a cow's hurt and the TNT's fuse", "[server][sound]") {
    Sounds   sounds{*loaded().blocks, *loaded().registries, 1};
    Recorder rec;
    sounds.mob_hurt(rec.host, entity_type("minecraft:cow"), Vec3d{2.5, -60.0, -5.5});
    sounds.tnt_primed(rec.host, Vec3d{0.5, -60.0, 4.5});
    REQUIRE(rec.sent.size() == 2);
    const auto cow = effect(rec.sent[0]);
    CHECK(cow.sound.sound_id == sound_id("minecraft:entity.cow.hurt"));
    CHECK(cow.category == net::sound_category::kNeutral);
    CHECK_THAT(cow.volume, WithinAbs(0.4, 1e-6));
    CHECK(rec.sent[0].except == nullptr);
    const auto tnt = effect(rec.sent[1]);
    CHECK(tnt.sound.sound_id == sound_id("minecraft:entity.tnt.primed"));
    CHECK(tnt.x == 4);
    CHECK(tnt.y == -480);
    CHECK(tnt.z == 36);
    // Every sound draws a seed of its own: the clients' variant choice.
    CHECK(cow.seed != tnt.seed);
}
