#include "ov/audio/music.hpp"
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::audio;

namespace {

constexpr std::string_view kDocument = R"json({
  "music.game": {"sounds": [{"name": "music/game/calm1", "stream": true}]},
  "music.creative": {"sounds": [{"name": "music/game/creative/aria_math", "stream": true}]},
  "music.overworld.forest": {"sounds": [{"name": "music/game/forest", "stream": true}]}
})json";

}  // namespace

TEST_CASE("default music: game, or creative in creative", "[audio][music]") {
    CHECK(default_music(false).event == "minecraft:music.game");
    CHECK(default_music(true).event == "minecraft:music.creative");
    CHECK(default_music(false).min_delay == 12000);
    CHECK(default_music(false).max_delay == 24000);
    CHECK_FALSE(default_music(false).replace_current);
}

TEST_CASE("music: a silence, a track, then a new silence in range", "[audio][music]") {
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    EngineDesc desc;
    desc.catalog  = &*catalog;
    desc.read     = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
    desc.backend  = Backend::Null;
    desc.keep_log = true;
    auto engine   = SoundEngine::create(desc);
    REQUIRE(engine.has_value());
    // A one-second "track": the manager only cares that it ends.
    const std::vector<i16> track(48000, 1000);
    (*engine)->add_pcm("assets/minecraft/sounds/music/game/calm1.ogg", track, 1, 48000);

    MusicManager      music{99, 3};
    const MusicChoice choice = default_music(false);
    for (int tick = 0; tick < 3; ++tick) {
        music.tick(**engine, choice);
        CHECK_FALSE(music.playing());
    }
    music.tick(**engine, choice);
    REQUIRE(music.playing());
    CHECK(music.current() == "minecraft:music.game");
    const auto log = (*engine)->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].category == SoundCategory::Music);
    CHECK(log[0].relative);

    // While it plays, nothing else starts.
    music.tick(**engine, choice);
    CHECK((*engine)->take_log().empty());

    // Play it to the end; the next tick draws a silence within the choice's range.
    std::vector<f32> out(2 * 4800);
    for (int block = 0; block < 12; ++block) {
        (*engine)->update();
        (*engine)->render(out);
    }
    music.tick(**engine, choice);
    CHECK_FALSE(music.playing());
    CHECK(music.ticks_until_next() >= 12000);
    CHECK(music.ticks_until_next() <= 24000);
}

TEST_CASE("music: a missing track waits a full silence instead of retrying every tick",
          "[audio][music]") {
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    EngineDesc desc;
    desc.catalog = &*catalog;
    desc.read    = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
    desc.backend = Backend::Null;
    auto engine  = SoundEngine::create(desc);
    REQUIRE(engine.has_value());

    MusicManager music{1, 0};
    music.tick(**engine, default_music(true));
    CHECK_FALSE(music.playing());
    // The choice's longest silence, not its shortest: the music is not
    // imported by default, and the menu's 20-tick minimum would warn every
    // second.
    CHECK(music.ticks_until_next() == 24000);
    CHECK((*engine)->stats().refused == 1);
}
