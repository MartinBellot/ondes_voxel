// The director's music, records and subtitles, on the null backend, through
// the same events the netclient hands out. Like test_sound_director.cpp it
// needs the registry pack and sounds.json, neither committed, and skips —
// saying so — without them.
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"
#include "ov/client/sound_director.hpp"
#include "ov/client/subtitles.hpp"
#include "ov/netclient/client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

using namespace ov;

namespace {

struct Rig {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<audio::SoundCatalog>     catalog;
    std::unique_ptr<audio::SoundEngine>    engine;
    std::unique_ptr<client::SoundDirector> director;
    client::SubtitleOverlay                subtitles;
    std::string                            missing;

    Rig() {
        const std::filesystem::path root{OV_SOURCE_DIR};
        const auto pack          = root / "data/vanilla/1.20.1/registry.ovpack";
        auto       loaded_blocks = registry::BlockRegistry::load(pack);
        auto       loaded_regs   = registry::Registries::load(pack);
        if (!loaded_blocks || !loaded_regs) {
            missing = "no format-15 registry.ovpack (tools/ov_datagen/ovpack.py)";
            return;
        }
        blocks.emplace(std::move(*loaded_blocks));
        registries.emplace(std::move(*loaded_regs));
        std::ifstream in(root / "run/assets/assets/minecraft/sounds.json", std::ios::binary);
        if (!in) {
            missing = "no sounds.json (ov-assetimport --sounds)";
            return;
        }
        const std::string json((std::istreambuf_iterator<char>(in)), {});
        auto              parsed = audio::SoundCatalog::parse(json);
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
        // Four seconds of every file these tests reach: long enough to still
        // be playing when a stop or a tick looks.
        const std::vector<i16> tone(48000 * 4, 8000);
        for (const char* event :
             {"minecraft:music_disc.cat", "minecraft:music_disc.stal", "minecraft:music.dragon",
              "minecraft:music.end", "minecraft:music.game", "minecraft:music.menu",
              "minecraft:music.nether.basalt_deltas", "minecraft:ui.button.click",
              "minecraft:block.stone.break"}) {
            std::vector<const audio::SoundEntry*> files;
            catalog->files_of(event, files);
            for (const audio::SoundEntry* file : files) {
                engine->add_pcm(file->name, tone, 1, 48000);
            }
        }
        director = std::make_unique<client::SoundDirector>(*engine, *blocks, *registries, 11);
        director->set_subtitles(&subtitles);
        director->listen(Vec3d{0.5, -59.0, 0.5}, 0.0F, 0.0F);
    }

    [[nodiscard]] i32 item(std::string_view name) const {
        const auto id = registries->find("minecraft:item");
        REQUIRE(id.has_value());
        const auto index = registries->protocol_id(*id, name);
        REQUIRE(index.has_value());
        return static_cast<i32>(*index);
    }

    void run(u32 frames = 512) {
        engine->update();
        std::vector<f32> out(static_cast<usize>(frames) * 2);
        engine->render(out);
    }
};

}  // namespace

TEST_CASE("director: a disc in a jukebox plays its record at volume 4, and says so",
          "[sound][director][music]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    // Measured on the real server: 1010 with the disc's item id (cat is 1123).
    CHECK(rig.item("minecraft:music_disc_cat") == 1123);
    netclient::ClientEvents events;
    events.world_events.push_back(net::WorldEvent{1010, 1, -60, 2, rig.item("minecraft:music_disc_cat"), false});
    rig.director->on_events(events, {});
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "minecraft:music_disc.cat");
    CHECK(log[0].category == audio::SoundCategory::Record);
    CHECK(log[0].volume == 4.0F);
    CHECK(log[0].position.x == 1.5);
    const auto now = rig.director->take_now_playing();
    REQUIRE(now.has_value());
    CHECK(*now == R"({"translate":"record.nowPlaying","with":[{"translate":"item.minecraft.music_disc_cat.desc"}]})");
    CHECK_FALSE(rig.director->take_now_playing().has_value());

    // The music fades under it, a tick at a time, and comes back after.
    rig.run();
    // One tick more than the fade: forty float steps of 1/40 may stop a hair
    // above zero, and the clamp only catches the one that crosses it.
    for (int t = 0; t <= client::SoundDirector::kFadeTicks; ++t) {
        rig.director->tick(false);
    }
    CHECK(rig.director->music_fade() == 0.0F);
    CHECK(rig.engine->fade(audio::SoundCategory::Music) == 0.0F);

    // 1011, measured on eject and on breaking the jukebox: the record stops.
    events.clear();
    events.world_events.push_back(net::WorldEvent{1011, 1, -60, 2, 0, false});
    rig.director->on_events(events, {});
    rig.run();
    CHECK_FALSE(rig.engine->is_playing("minecraft:music_disc.cat"));
    rig.director->tick(false);
    CHECK(rig.director->music_fade() > 0.0F);

    // An item that is not a disc: refused, named, silent.
    events.clear();
    events.world_events.push_back(net::WorldEvent{1010, 1, -60, 2, rig.item("minecraft:stone"), false});
    const u64 refused = rig.director->refused();
    rig.director->on_events(events, {});
    CHECK(rig.director->refused() == refused + 1);
    CHECK(rig.engine->take_log().empty());
}

TEST_CASE("director: the dragon's bar in the End plays the boss music at once",
          "[sound][director][music]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    netclient::ClientEvents events;
    events.dimension = "minecraft:the_end";
    events.boss_bars.push_back(netclient::ClientEvents::BossBarChange{1, 2, 0, 0x02 | 0x04});
    rig.director->on_events(events, {});
    CHECK(rig.director->dimension() == "minecraft:the_end");
    CHECK(rig.director->boss_music());
    rig.director->tick(client::MusicContext{});
    CHECK(rig.director->music().playing());
    CHECK(rig.director->music().current() == "minecraft:music.dragon");

    // The bar goes: the dragon's music stops with it.
    events.clear();
    events.boss_bars.push_back(netclient::ClientEvents::BossBarChange{1, 2, 1, 0});
    rig.director->on_events(events, {});
    CHECK_FALSE(rig.director->boss_music());
    rig.run();
    rig.director->tick(client::MusicContext{});
    CHECK_FALSE(rig.director->music().playing());
}

TEST_CASE("director: the biome's music comes from the codec the server sent",
          "[sound][director][music]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    netclient::ClientEvents events;
    events.dimension   = "minecraft:the_nether";
    events.biome_music = std::vector<net::BiomeMusic>{
        net::BiomeMusic{"minecraft:basalt_deltas", "minecraft:music.nether.basalt_deltas", 0, 0,
                        false}};
    rig.director->on_events(events, {});
    client::MusicContext context;
    context.biome = "minecraft:basalt_deltas";
    // The manager's first silence is 100 ticks, capped by the choice's 0.
    rig.director->tick(context);
    rig.director->tick(context);
    CHECK(rig.director->music().current() == "minecraft:music.nether.basalt_deltas");
}

TEST_CASE("director: the menu's click, and a subtitle for a sound heard", "[sound][director]") {
    Rig rig;
    if (!rig.missing.empty()) {
        SKIP(rig.missing);
    }
    rig.director->clicked();
    const auto log = rig.engine->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "minecraft:ui.button.click");
    CHECK(log[0].relative);
    CHECK(log[0].category == audio::SoundCategory::Master);

    const auto stone = rig.blocks->default_state(*rig.blocks->find_block("minecraft:stone"));
    // Yaw 0 faces +Z, so -X is the right hand: a block 4 blocks toward -X is
    // on the right, and its line points right.
    rig.director->broke(stone, BlockPos{-4, -60, 0});
    rig.director->broke(stone, BlockPos{0, -60, 100});  // out of range: no line
    const auto lines = rig.subtitles.lines(audio::Listener{Vec3d{0.5, -59.0, 0.5}, 0.0F, 0.0F});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "subtitles.block.generic.break");
    CHECK(lines[0].arrow == 1);
}
