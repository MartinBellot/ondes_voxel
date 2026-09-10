// The engine on the null backend: the same mixer the device runs, pulled by
// hand, fed waveforms whose every sample is known.
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <vector>

using namespace ov;
using namespace ov::audio;
using Catch::Matchers::WithinAbs;

namespace {

constexpr u32 kRate = 48000;

constexpr std::string_view kDocument = R"json({
  "test.flat": {"sounds": ["test/flat"]},
  "test.short": {"sounds": ["test/short"]},
  "test.near": {"sounds": [{"name": "test/flat", "attenuation_distance": 8}]},
  "test.quiet": {"sounds": [{"name": "test/flat", "volume": 0.5}]},
  "test.missing": {"sounds": ["test/not_there"]},
  "block.stone.place": {"sounds": ["dig/stone1", "dig/stone2", "dig/stone3", "dig/stone4"]}
})json";

struct Fixture {
    SoundCatalog                 catalog;
    std::unique_ptr<SoundEngine> engine;

    explicit Fixture(u32 voices = 8) {
        auto parsed = SoundCatalog::parse(kDocument);
        REQUIRE(parsed.has_value());
        catalog = std::move(*parsed);
        EngineDesc desc;
        desc.catalog     = &catalog;
        desc.read        = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
        desc.backend     = Backend::Null;
        desc.sample_rate = kRate;
        desc.max_voices  = voices;
        desc.keep_log    = true;
        auto created     = SoundEngine::create(desc);
        REQUIRE(created.has_value());
        engine = std::move(*created);

        // A flat mono waveform at half scale, 10 000 frames: whatever comes out
        // is the gain, read directly.
        const std::vector<i16> flat(10000, 16384);
        engine->add_pcm("assets/minecraft/sounds/test/flat.ogg", flat, 1, kRate);
        const std::vector<i16> brief(1000, 16384);
        engine->add_pcm("assets/minecraft/sounds/test/short.ogg", brief, 1, kRate);
    }

    std::vector<f32> render(u32 frames) {
        engine->update();
        std::vector<f32> out(static_cast<usize>(frames) * 2, -1.0F);
        engine->render(out);
        return out;
    }
};

PlayRequest at(std::string_view event, Vec3d position, SoundCategory category = SoundCategory::Block) {
    PlayRequest request;
    request.event    = event;
    request.position = position;
    request.category = category;
    return request;
}

}  // namespace

TEST_CASE("attenuation: vanilla's linear model, 16 blocks times the volume", "[audio][engine]") {
    CHECK(SoundEngine::attenuation(0.0, 1.0F, 16) == 1.0F);
    CHECK_THAT(SoundEngine::attenuation(8.0, 1.0F, 16), WithinAbs(0.5, 1e-6));
    CHECK(SoundEngine::attenuation(16.0, 1.0F, 16) == 0.0F);
    CHECK(SoundEngine::attenuation(40.0, 1.0F, 16) == 0.0F);
    // Above 1 it is not louder, it carries further: silent at 32, not 16.
    CHECK(SoundEngine::attenuation(0.0, 2.0F, 16) == 1.0F);
    CHECK_THAT(SoundEngine::attenuation(16.0, 2.0F, 16), WithinAbs(0.5, 1e-6));
    // Below 1 it is quieter over the same 16 blocks.
    CHECK_THAT(SoundEngine::attenuation(0.0, 0.5F, 16), WithinAbs(0.5, 1e-6));
    CHECK_THAT(SoundEngine::attenuation(8.0, 0.5F, 16), WithinAbs(0.25, 1e-6));
    // attenuation_distance replaces the 16.
    CHECK(SoundEngine::attenuation(8.0, 1.0F, 8) == 0.0F);
    CHECK(SoundEngine::attenuation(1.0, 0.0F, 16) == 0.0F);
}

TEST_CASE("categories are Mojang's, in wire order", "[audio][engine]") {
    CHECK(static_cast<int>(SoundCategory::Master) == 0);
    CHECK(static_cast<int>(SoundCategory::Block) == 4);
    CHECK(static_cast<int>(SoundCategory::Hostile) == 5);
    CHECK(static_cast<int>(SoundCategory::Voice) == 9);
    CHECK(to_string(SoundCategory::Neutral) == "neutral");
    CHECK(category_from_name("record") == SoundCategory::Record);
    CHECK_FALSE(category_from_name("sfx").has_value());
}

TEST_CASE("a sound at the listener is heard, centred", "[audio][engine]") {
    Fixture f;
    REQUIRE(f.engine->play(at("test.flat", Vec3d{0, 64, 0})).has_value());
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    const auto out = f.render(256);
    // 0.5 of full scale, gain 1, equal-power centre: 0.5 * cos(pi/4) per ear.
    const double expected = 0.5 * std::cos(std::numbers::pi / 4.0);
    CHECK_THAT(out[200], WithinAbs(expected, 1e-4));
    CHECK_THAT(out[201], WithinAbs(expected, 1e-4));
    CHECK(f.engine->stats().active_voices == 1);
    CHECK(f.engine->is_playing("test.flat"));

    const auto log = f.engine->take_log();
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "minecraft:test.flat");
    CHECK(log[0].file == "assets/minecraft/sounds/test/flat.ogg");
    CHECK(log[0].category == SoundCategory::Block);
}

TEST_CASE("distance and direction: 8 blocks to the right is half, in the right ear",
          "[audio][engine]") {
    Fixture f;
    // Yaw 0 faces +Z; the right hand points to -X.
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    REQUIRE(f.engine->play(at("test.flat", Vec3d{-8, 64, 0})).has_value());
    const auto out = f.render(256);
    CHECK_THAT(out[200], WithinAbs(0.0, 1e-4));         // left
    CHECK_THAT(out[201], WithinAbs(0.5 * 0.5, 1e-4));   // right: sample 0.5, gain 0.5

    // Turned around (yaw 180 faces -Z), the same sound is in the left ear.
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 180.0F});
    (void)f.render(256);  // one callback to ramp
    const auto turned = f.render(256);
    CHECK_THAT(turned[200], WithinAbs(0.25, 1e-3));
    CHECK_THAT(turned[201], WithinAbs(0.0, 1e-3));
}

TEST_CASE("out of range is silent, and attenuation_distance is per entry", "[audio][engine]") {
    Fixture f;
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    REQUIRE(f.engine->play(at("test.near", Vec3d{0, 64, 10})).has_value());
    const auto out = f.render(128);
    CHECK(out[100] == 0.0F);
    CHECK(out[101] == 0.0F);
}

TEST_CASE("category and master volumes scale, master scales everything", "[audio][engine]") {
    Fixture f;
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    f.engine->set_volume(SoundCategory::Block, 0.5F);
    REQUIRE(f.engine->play(at("test.flat", Vec3d{0, 64, 0})).has_value());
    const double centre = std::cos(std::numbers::pi / 4.0);
    CHECK_THAT(f.render(256)[200], WithinAbs(0.5 * 0.5 * centre, 1e-4));

    f.engine->set_volume(SoundCategory::Master, 0.0F);
    (void)f.render(256);
    CHECK(f.render(256)[200] == 0.0F);
    CHECK(f.engine->volume(SoundCategory::Master) == 0.0F);
}

TEST_CASE("the entry's own volume multiplies the request's", "[audio][engine]") {
    Fixture f;
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    REQUIRE(f.engine->play(at("test.quiet", Vec3d{0, 64, 0})).has_value());
    const double centre = std::cos(std::numbers::pi / 4.0);
    CHECK_THAT(f.render(256)[200], WithinAbs(0.5 * 0.5 * centre, 1e-4));
}

TEST_CASE("pitch changes the length and is clamped to 0.5 .. 2", "[audio][engine]") {
    Fixture     f;
    PlayRequest fast = at("test.short", Vec3d{0, 64, 0});
    fast.pitch       = 2.0F;
    fast.relative    = true;
    REQUIRE(f.engine->play(fast).has_value());
    const auto out = f.render(600);
    // 1000 source frames at twice the speed last 500 output frames.
    CHECK(out[2 * 490] != 0.0F);
    CHECK(out[2 * 510] == 0.0F);
    CHECK(f.engine->stats().active_voices == 0);
    CHECK_FALSE(f.engine->is_playing("test.short"));

    PlayRequest wild = fast;
    wild.pitch       = 7.0F;
    REQUIRE(f.engine->play(wild).has_value());
    wild.pitch = 0.01F;
    REQUIRE(f.engine->play(wild).has_value());
    const auto log = f.engine->take_log();
    REQUIRE(log.size() == 3);
    CHECK(log[1].pitch == 2.0F);
    CHECK(log[2].pitch == 0.5F);
}

TEST_CASE("Stop Sound: by category, by event, and everything", "[audio][engine]") {
    Fixture f;
    f.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    REQUIRE(f.engine->play(at("test.flat", Vec3d{0, 64, 0}, SoundCategory::Block)).has_value());
    REQUIRE(f.engine->play(at("test.flat", Vec3d{0, 64, 0}, SoundCategory::Hostile)).has_value());
    (void)f.render(64);
    CHECK(f.engine->stats().active_voices == 2);

    f.engine->stop(SoundCategory::Block);
    (void)f.render(64);
    CHECK(f.engine->stats().active_voices == 1);

    f.engine->stop(std::nullopt, "test.short");  // matches nothing playing
    (void)f.render(64);
    CHECK(f.engine->stats().active_voices == 1);

    f.engine->stop(std::nullopt);
    const auto out = f.render(64);
    CHECK(f.engine->stats().active_voices == 0);
    CHECK(out[10] == 0.0F);
}

TEST_CASE("the voice limit drops the newcomer and counts it", "[audio][engine]") {
    Fixture f{2};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(f.engine->play(at("test.flat", Vec3d{0, 64, 0})).has_value());
    }
    (void)f.render(64);
    const EngineStats stats = f.engine->stats();
    CHECK(stats.active_voices == 2);
    CHECK(stats.dropped_no_voice == 1);
    CHECK(stats.started == 3);
}

TEST_CASE("refusals are named: unknown event, missing file", "[audio][engine]") {
    Fixture f;
    const auto unknown = f.engine->play(at("minecraft:no.such.sound", Vec3d{}));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error() == PlayRefusal::UnknownEvent);
    const auto missing = f.engine->play(at("test.missing", Vec3d{}));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == PlayRefusal::FileMissing);
    CHECK(f.engine->stats().refused == 2);
    CHECK(to_string(PlayRefusal::FileMissing).find("asset") != std::string_view::npos);
}

// The one test that goes through stb_vorbis with a real file. Mojang's sounds
// may not be committed, so it runs where ov-assetimport --sounds has been run
// and skips — saying so — everywhere else.
TEST_CASE("a real Ogg Vorbis file decodes and plays", "[audio][engine][assets]") {
    const std::filesystem::path root = std::filesystem::path{OV_SOURCE_DIR} / "run" / "assets";
    const std::filesystem::path file = root / "assets/minecraft/sounds/dig/stone1.ogg";
    if (!std::filesystem::exists(file)) {
        SKIP("run/assets has no sounds: ov-assetimport --sounds");
    }
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    EngineDesc desc;
    desc.catalog = &*catalog;
    desc.read    = [root](std::string_view path) -> std::optional<std::vector<u8>> {
        std::ifstream in(root / path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
    };
    desc.backend  = Backend::Null;
    desc.keep_log = true;
    auto engine   = SoundEngine::create(desc);
    REQUIRE(engine.has_value());
    (*engine)->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});

    PlayRequest request;
    request.event    = "block.stone.place";
    request.category = SoundCategory::Block;
    request.position = Vec3d{0, 64, 0};
    request.seed     = 42;
    REQUIRE((*engine)->play(request).has_value());
    (*engine)->update();
    std::vector<f32> out(4800 * 2);
    (*engine)->render(out);
    f32 peak = 0.0F;
    for (const f32 sample : out) {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.01F);
    CHECK((*engine)->stats().cached_bytes > 0);
}
