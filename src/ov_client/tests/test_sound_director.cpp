// The whole chain a sound travels on the client, on the null backend: a
// packet or a gesture, the measured block sounds in the registry pack, the
// sounds.json catalogue, the engine's mixer. What comes out is read from the
// engine's log and from the mixed samples.
//
// It needs the pack (tools/ov_datagen/ovpack.py, format 15, with the measured
// sound sections) and sounds.json (ov-assetimport --sounds). Neither may be
// committed, so without them it says so and skips.
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"
#include "ov/client/sound_director.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/chat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

using namespace ov;
using Catch::Matchers::WithinAbs;

namespace {

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

struct Rig {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<audio::SoundCatalog>     catalog;
    std::unique_ptr<audio::SoundEngine>    engine;
    std::unique_ptr<client::SoundDirector> director;
    std::string                            missing;

    Rig() {
        const std::filesystem::path root{OV_SOURCE_DIR};
        const auto pack = root / "data/vanilla/1.20.1/registry.ovpack";
        auto       loaded_blocks = registry::BlockRegistry::load(pack);
        auto       loaded_regs   = registry::Registries::load(pack);
        if (!loaded_blocks || !loaded_regs) {
            missing = "no format-15 registry.ovpack (tools/ov_datagen/ovpack.py)";
            return;
        }
        blocks.emplace(std::move(*loaded_blocks));
        registries.emplace(std::move(*loaded_regs));
        const auto json = read_text(root / "run/assets/assets/minecraft/sounds.json");
        if (!json) {
            missing = "no sounds.json (ov-assetimport --sounds)";
            return;
        }
        auto parsed = audio::SoundCatalog::parse(*json);
        REQUIRE(parsed.has_value());
        catalog.emplace(std::move(*parsed));

        audio::EngineDesc desc;
        desc.catalog  = &*catalog;
        desc.read     = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
        desc.backend  = audio::Backend::Null;
        desc.keep_log = true;
        auto created  = audio::SoundEngine::create(desc);
        REQUIRE(created.has_value());
        engine = std::move(*created);
        // A short click for every file the tests can reach: which variant a
        // seed picks is the catalogue's business, not this test's.
        const std::vector<i16> click(2400, 12000);
        for (const char* event :
             {"minecraft:block.stone.break", "minecraft:block.stone.place",
              "minecraft:block.stone.step", "minecraft:block.stone.fall",
              "minecraft:entity.generic.explode", "minecraft:entity.experience_orb.pickup",
              "minecraft:entity.item.pickup", "minecraft:entity.player.small_fall",
              "minecraft:entity.player.hurt"}) {
            std::vector<const audio::SoundEntry*> files;
            catalog->files_of(event, files);
            for (const audio::SoundEntry* file : files) {
                engine->add_pcm(file->name, click, 1, 48000);
            }
        }
        director = std::make_unique<client::SoundDirector>(*engine, *blocks, *registries, 7);
        director->listen(Vec3d{0.5, 64.0, 0.5}, 0.0F);
    }

    [[nodiscard]] registry::BlockStateId stone() const {
        return blocks->default_state(*blocks->find_block("minecraft:stone"));
    }
};

}  // namespace

TEST_CASE("director: another player's broken block is heard from World Event 2001",
          "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    netclient::ClientEvents events;
    events.world_events.push_back(net::WorldEvent{net::kWorldEventBlockBreak, 1, 64, 2,
                                                  static_cast<i32>(rig.stone().value()), false});
    rig.director->on_events(events, {});
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "minecraft:block.stone.break");
    // Stone's set is 1.0 / 1.0: broken at (1+1)/2 and 1*0.8, the wiki's row.
    CHECK_THAT(log[0].volume, WithinAbs(1.0, 1e-6));
    CHECK_THAT(log[0].pitch, WithinAbs(0.8, 1e-6));
    CHECK(log[0].position.x == 1.5);
    CHECK(log[0].category == audio::SoundCategory::Block);

    // And it is heard: the mixer produces samples.
    std::vector<f32> out(2 * 512);
    rig.engine->update();
    rig.engine->render(out);
    f32 peak = 0.0F;
    for (const f32 sample : out) {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.05F);
}

TEST_CASE("director: a Sound Effect by registry id plays that event", "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    netclient::ClientEvents events;
    net::SoundEffect        sound;
    sound.sound.sound_id = 1269;  // captured: block.stone.place
    sound.category       = 4;
    sound.x              = 12;
    sound.y              = 512;
    sound.z              = 20;
    sound.pitch          = 0.8F;
    events.sounds.push_back(sound);
    net::SoundEffect bad = sound;
    bad.category         = 12;  // no such category: refused, not folded into one
    events.sounds.push_back(bad);
    rig.director->on_events(events, {});
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "minecraft:block.stone.place");
    CHECK(rig.director->refused() == 1);
}

TEST_CASE("director: the sounds no packet carries — explosion and pickups", "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    netclient::ClientEvents events;
    net::Explosion          blast;
    blast.x = 3.0;
    blast.y = 64.0;
    blast.z = 3.0;
    events.explosions.push_back(blast);
    events.pickups.push_back(netclient::ClientEvents::Pickup{42, 1, 1});
    events.pickups.push_back(netclient::ClientEvents::Pickup{43, 1, 1});
    events.pickups.push_back(netclient::ClientEvents::Pickup{99, 1, 1});  // never seen
    const client::EntityLookup lookup = [](i32 id) -> std::optional<client::HeardEntity> {
        if (id == 42) {
            return client::HeardEntity{Vec3d{1, 64, 1},
                                       netclient::ClientEvents::kSpawnedAsExperienceOrb};
        }
        if (id == 43) {
            return client::HeardEntity{Vec3d{2, 64, 1}, 55};
        }
        return std::nullopt;
    };
    rig.director->on_events(events, lookup);
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 3);
    CHECK(log[0].event == "minecraft:entity.generic.explode");
    CHECK(log[0].volume == 4.0F);
    CHECK(log[0].pitch >= 0.56F);
    CHECK(log[0].pitch <= 0.84F);
    CHECK(log[1].event == "minecraft:entity.experience_orb.pickup");
    CHECK(log[1].volume == 0.1F);
    CHECK(log[2].event == "minecraft:entity.item.pickup");
    CHECK(log[2].pitch >= 1.6F);
    CHECK(rig.director->refused() == 1);
}

TEST_CASE("director: footsteps come one per 1/0.6 blocks, at 0.15", "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    for (int tick = 0; tick < 10; ++tick) {  // 10 * 0.2158 = 2.158 blocks
        rig.director->walked(0.2158, true, rig.stone(), Vec3d{0.5, 64.0, 0.5});
    }
    rig.director->walked(5.0, false, rig.stone(), Vec3d{0.5, 64.0, 0.5});  // airborne: silent
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "minecraft:block.stone.step");
    CHECK_THAT(log[0].volume, WithinAbs(0.15, 1e-6));
    CHECK(log[0].category == audio::SoundCategory::Player);
}

TEST_CASE("director: this player's own place and break, at (v+1)/2 and p*0.8",
          "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    rig.director->placed(rig.stone(), BlockPos{2, -60, 5});
    rig.director->broke(rig.stone(), BlockPos{0, -61, 5});
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 2);
    CHECK(log[0].event == "minecraft:block.stone.place");
    CHECK_THAT(log[0].volume, WithinAbs(1.0, 1e-6));
    CHECK_THAT(log[0].pitch, WithinAbs(0.8, 1e-6));
    CHECK(log[0].position.y == -59.5);
    CHECK(log[1].event == "minecraft:block.stone.break");
}

TEST_CASE("director: a landing is the fall, the block, the hurt — in that order",
          "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    rig.director->landed(2.0F, rig.stone(), Vec3d{0.5, 64.0, 0.5});
    rig.director->landed(0.0F, rig.stone(), Vec3d{0.5, 64.0, 0.5});  // no damage, no sound
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 3);
    CHECK(log[0].event == "minecraft:entity.player.small_fall");
    CHECK(log[1].event == "minecraft:block.stone.fall");
    CHECK_THAT(log[1].volume, WithinAbs(0.5, 1e-6));
    CHECK_THAT(log[1].pitch, WithinAbs(0.75, 1e-6));
    CHECK(log[2].event == "minecraft:entity.player.hurt");
}
