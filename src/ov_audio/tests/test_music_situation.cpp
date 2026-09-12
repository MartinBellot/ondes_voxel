// Which music, in which situation: the wiki's Music page, rule by rule.
#include "ov/audio/music.hpp"
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::audio;

TEST_CASE("situational music, in the wiki's order", "[audio][music]") {
    MusicSituation s;
    CHECK(situational_music(s).event == "minecraft:music.game");
    CHECK(situational_music(s).min_delay == 12000);
    CHECK(situational_music(s).max_delay == 24000);

    s.creative = true;
    CHECK(situational_music(s).event == "minecraft:music.creative");

    // The biome's own music, when it has one and the player is not creative.
    const MusicChoice forest{"minecraft:music.overworld.forest", 12000, 24000, false};
    s.creative = false;
    s.biome    = forest;
    CHECK(situational_music(s).event == "minecraft:music.overworld.forest");

    // Underwater, and in an ocean or a river: both, or neither.
    s.underwater = true;
    CHECK(situational_music(s).event == "minecraft:music.overworld.forest");
    s.underwater_biome = true;
    CHECK(situational_music(s).event == "minecraft:music.under_water");

    // The End: its own, or the dragon's, which replaces and starts at once.
    s.dimension = "minecraft:the_end";
    CHECK(situational_music(s).event == "minecraft:music.end");
    s.boss_music = true;
    const MusicChoice dragon = situational_music(s);
    CHECK(dragon.event == "minecraft:music.dragon");
    CHECK(dragon.replace_current);
    CHECK(dragon.max_delay == 0);

    // A boss bar outside the End (the wither's) is not the dragon's music.
    s.dimension = "minecraft:overworld";
    CHECK(situational_music(s).event == "minecraft:music.under_water");

    // The Nether: the biome's, as every Nether biome names one.
    MusicSituation nether;
    nether.dimension = "minecraft:the_nether";
    nether.biome     = MusicChoice{"minecraft:music.nether.basalt_deltas", 12000, 24000, false};
    CHECK(situational_music(nether).event == "minecraft:music.nether.basalt_deltas");

    // The menu, "after 1 to 30 seconds"; the credits before everything.
    s.menu                 = true;
    const MusicChoice menu = situational_music(s);
    CHECK(menu.event == "minecraft:music.menu");
    CHECK(menu.min_delay == 20);
    CHECK(menu.max_delay == 600);
    s.credits = true;
    CHECK(situational_music(s).event == "minecraft:music.credits");
}

TEST_CASE("music: the menu's track stops with the menu, and a long silence is cut short",
          "[audio][music]") {
    constexpr std::string_view kDocument = R"json({
      "music.menu": {"sounds": [{"name": "music/menu/m", "stream": true}]},
      "music.game": {"sounds": [{"name": "music/game/g", "stream": true}]},
      "music.dragon": {"sounds": [{"name": "music/game/end/boss", "stream": true}]}
    })json";
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    EngineDesc desc;
    desc.catalog = &*catalog;
    desc.read    = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
    desc.backend = Backend::Null;
    auto engine  = SoundEngine::create(desc);
    REQUIRE(engine.has_value());
    SoundEngine&           e = **engine;
    const std::vector<i16> track(48000 * 4, 1000);
    e.add_pcm("assets/minecraft/sounds/music/menu/m.ogg", track, 1, 48000);
    e.add_pcm("assets/minecraft/sounds/music/game/g.ogg", track, 1, 48000);
    e.add_pcm("assets/minecraft/sounds/music/game/end/boss.ogg", track, 1, 48000);

    MusicSituation menu_situation;
    menu_situation.menu = true;
    MusicManager music(7, 5000);
    // A first silence of 5000 is cut to the menu's 600.
    music.tick(e, situational_music(menu_situation));
    CHECK(music.ticks_until_next() < 600);
    for (int t = 0; t < 601 && !music.playing(); ++t) {
        music.tick(e, situational_music(menu_situation));
        e.update();
    }
    REQUIRE(music.playing());
    CHECK(music.current() == "minecraft:music.menu");

    // Into a world: the menu's track stops, and a game silence is drawn.
    const MusicSituation game;
    music.tick(e, situational_music(game));
    CHECK_FALSE(music.playing());
    CHECK(music.ticks_until_next() >= 12000);
    CHECK(music.ticks_until_next() <= 24000);

    // The dragon: at once, whatever the silence.
    MusicSituation end;
    end.dimension  = "minecraft:the_end";
    end.boss_music = true;
    music.tick(e, situational_music(end));
    CHECK(music.playing());
    CHECK(music.current() == "minecraft:music.dragon");

    // The dragon dies: its track stops with it, the End's silence follows.
    end.boss_music = false;
    music.tick(e, situational_music(end));
    CHECK_FALSE(music.playing());
    CHECK(music.ticks_until_next() >= 12000);

    // Back to the menu: the silence is cut to 600 at most.
    music.tick(e, situational_music(menu_situation));
    CHECK(music.ticks_until_next() < 600);
}

TEST_CASE("music: an event with no track (warped forest) waits a full silence",
          "[audio][music]") {
    constexpr std::string_view kDocument = R"json({
      "music.nether.warped_forest": {"sounds": []}
    })json";
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    EngineDesc desc;
    desc.catalog = &*catalog;
    desc.read    = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
    desc.backend = Backend::Null;
    auto engine  = SoundEngine::create(desc);
    REQUIRE(engine.has_value());
    MusicManager      music(1, 0);
    const MusicChoice warped{"minecraft:music.nether.warped_forest", 12000, 24000, false};
    music.tick(**engine, warped);
    CHECK_FALSE(music.playing());
    CHECK(music.ticks_until_next() == 24000);
}
