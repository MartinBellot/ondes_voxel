// The head, the priority of voices, and the fades — on the null backend, fed
// a flat waveform so that every sample out is the gain, read directly.
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace ov;
using namespace ov::audio;
using Catch::Matchers::WithinAbs;

namespace {

constexpr u32 kRate = 48000;

constexpr std::string_view kDocument = R"json({
  "test.flat": {"sounds": ["test/flat"]}
})json";

struct Rig {
    SoundCatalog                 catalog;
    std::unique_ptr<SoundEngine> engine;

    explicit Rig(u32 voices = 8) {
        auto parsed = SoundCatalog::parse(kDocument);
        REQUIRE(parsed.has_value());
        catalog = std::move(*parsed);
        EngineDesc desc;
        desc.catalog     = &catalog;
        desc.read        = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
        desc.backend     = Backend::Null;
        desc.sample_rate = kRate;
        desc.max_voices  = voices;
        auto created     = SoundEngine::create(desc);
        REQUIRE(created.has_value());
        engine = std::move(*created);
        const std::vector<i16> flat(20000, 16384);  // 0.5 of full scale
        engine->add_pcm("assets/minecraft/sounds/test/flat.ogg", flat, 1, kRate);
    }

    std::vector<f32> render(u32 frames) {
        engine->update();
        std::vector<f32> out(static_cast<usize>(frames) * 2, -1.0F);
        engine->render(out);
        return out;
    }
};

PlayRequest at(Vec3d position, SoundCategory category = SoundCategory::Block) {
    PlayRequest request;
    request.event    = "test.flat";
    request.position = position;
    request.category = category;
    return request;
}

const double kCentre = std::cos(std::numbers::pi / 4.0);

}  // namespace

// ── The head ────────────────────────────────────────────────────────────────

TEST_CASE("pan: the right hand from look x up, whatever the pitch", "[audio][pan]") {
    const Vec3d ears{0, 64, 0};
    // Yaw 0 faces +Z: -X is the right hand, +X the left.
    CHECK_THAT(SoundEngine::pan(Listener{ears, 0.0F, 0.0F}, Vec3d{-5, 64, 0}), WithinAbs(1.0, 1e-6));
    CHECK_THAT(SoundEngine::pan(Listener{ears, 0.0F, 0.0F}, Vec3d{5, 64, 0}), WithinAbs(-1.0, 1e-6));
    // Yaw 90 faces -X: the right hand is -Z.
    CHECK_THAT(SoundEngine::pan(Listener{ears, 90.0F, 0.0F}, Vec3d{0, 64, -5}), WithinAbs(1.0, 1e-6));
    // Ahead and behind: centred.
    CHECK_THAT(SoundEngine::pan(Listener{ears, 0.0F, 0.0F}, Vec3d{0, 64, 5}), WithinAbs(0.0, 1e-6));
    CHECK_THAT(SoundEngine::pan(Listener{ears, 0.0F, 0.0F}, Vec3d{0, 64, -5}), WithinAbs(0.0, 1e-6));
    // Overhead and underfoot: centred, at any yaw and pitch.
    for (const f32 pitch : {-90.0F, -45.0F, 0.0F, 30.0F, 90.0F}) {
        for (const f32 yaw : {0.0F, 37.0F, 90.0F, 211.0F}) {
            const Listener head{ears, yaw, pitch};
            CHECK_THAT(SoundEngine::pan(head, Vec3d{0, 70, 0}), WithinAbs(0.0, 1e-6));
            CHECK_THAT(SoundEngine::pan(head, Vec3d{0, 60, 0}), WithinAbs(0.0, 1e-6));
        }
    }
    // Nodding never swaps ears: with no roll the right axis is horizontal, so
    // a source anywhere pans the same at every pitch.
    for (const Vec3d source : {Vec3d{-3, 66, 2}, Vec3d{4, 61, -7}, Vec3d{1, 64, 9}}) {
        const f32 level = SoundEngine::pan(Listener{ears, 25.0F, 0.0F}, source);
        for (const f32 pitch : {-89.0F, -30.0F, 45.0F, 89.0F}) {
            CHECK_THAT(SoundEngine::pan(Listener{ears, 25.0F, pitch}, source),
                       WithinAbs(static_cast<f64>(level), 1e-5));
        }
    }
}

TEST_CASE("spatialise: the formula, and the samples the mixer writes", "[audio][pan]") {
    // The model written from the documentation — linear to
    // attenuation_distance x max(1, volume), gain min(1, volume), a
    // constant-power pan on the head's right axis — compared first with the
    // formula computed here independently, then with the null device's output.
    struct Case {
        Vec3d    source;
        Listener head;
        f32      volume;
        f64      pan;  // expected, worked out by hand
    };
    const Vec3d ears{10, 64, -3};
    const Case  cases[] = {
        {Vec3d{10, 64, -3}, Listener{ears, 0.0F, 0.0F}, 1.0F, 0.0},     // at the ears
        {Vec3d{4, 64, -3}, Listener{ears, 0.0F, 0.0F}, 1.0F, 1.0},      // 6 to the right (-X)
        {Vec3d{16, 64, -3}, Listener{ears, 0.0F, 0.0F}, 1.0F, -1.0},    // 6 to the left (+X)
        {Vec3d{10, 64, 9}, Listener{ears, 0.0F, 0.0F}, 1.0F, 0.0},      // 12 ahead
        {Vec3d{13, 68, 0}, Listener{ears, -45.0F, 20.0F}, 1.0F, 0.0},   // ahead along the yaw
        {Vec3d{-10, 64, -3}, Listener{ears, 90.0F, -60.0F}, 2.0F, 0.0}, // 20 ahead, volume 2
        {Vec3d{10, 64, -6}, Listener{ears, 90.0F, 0.0F}, 0.3F, 1.0},    // 3 to the right, 0.3
        {Vec3d{7, 64, 0}, Listener{ears, 0.0F, 70.0F}, 1.0F, std::sqrt(0.5)},  // 45 deg right, head down
    };
    for (const Case& c : cases) {
        const f64 dx = c.source.x - ears.x, dy = c.source.y - ears.y, dz = c.source.z - ears.z;
        const f64 d  = std::sqrt(dx * dx + dy * dy + dz * dz);
        const f64 v  = static_cast<f64>(c.volume);
        const f64 g  = std::min(1.0, v) * std::clamp(1.0 - d / (16.0 * std::max(1.0, v)), 0.0, 1.0);
        const f64 angle = (c.pan + 1.0) * std::numbers::pi / 4.0;

        CHECK_THAT(SoundEngine::pan(c.head, c.source), WithinAbs(c.pan, 1e-5));
        const StereoGain model = SoundEngine::spatialise(c.head, c.source, c.volume, 16, false);
        CHECK_THAT(model.left, WithinAbs(g * std::cos(angle), 1e-5));
        CHECK_THAT(model.right, WithinAbs(g * std::sin(angle), 1e-5));

        Rig rig;
        rig.engine->set_listener(c.head);
        PlayRequest request = at(c.source);
        request.volume      = c.volume;
        REQUIRE(rig.engine->play(request).has_value());
        const auto out = rig.render(256);
        CHECK_THAT(out[200], WithinAbs(0.5 * static_cast<f64>(model.left), 1e-4));
        CHECK_THAT(out[201], WithinAbs(0.5 * static_cast<f64>(model.right), 1e-4));
    }
}

TEST_CASE("a relative sound is unpanned and unattenuated", "[audio][pan]") {
    const StereoGain g = SoundEngine::spatialise(Listener{Vec3d{0, 0, 0}, 0.0F, 0.0F},
                                                 Vec3d{100, 0, 0}, 0.25F, 16, true);
    CHECK_THAT(g.left, WithinAbs(0.25 * kCentre, 1e-6));
    CHECK(g.left == g.right);
}

// ── Priority ────────────────────────────────────────────────────────────────

TEST_CASE("full: a louder newcomer takes the quietest voice", "[audio][priority]") {
    Rig rig{2};
    rig.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    REQUIRE(rig.engine->play(at(Vec3d{0, 64, 12})).has_value());  // gain 0.25
    REQUIRE(rig.engine->play(at(Vec3d{0, 64, 4})).has_value());   // gain 0.75
    (void)rig.render(64);
    // At the ears: louder than both, and it takes the 0.25 one's voice.
    REQUIRE(rig.engine->play(at(Vec3d{0, 64, 0})).has_value());
    const auto out = rig.render(256);
    CHECK(rig.engine->stats().stolen == 1);
    CHECK(rig.engine->stats().dropped_no_voice == 0);
    CHECK_THAT(out[200], WithinAbs(0.5 * (1.0 + 0.75) * kCentre, 1e-4));

    // Out of range: quieter than everything playing, so it is the one dropped.
    REQUIRE(rig.engine->play(at(Vec3d{0, 64, 40})).has_value());
    (void)rig.render(64);
    CHECK(rig.engine->stats().stolen == 1);
    CHECK(rig.engine->stats().dropped_no_voice == 1);
    CHECK(rig.engine->stats().active_voices == 2);
}

TEST_CASE("full: equals do not displace each other", "[audio][priority]") {
    Rig rig{2};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(rig.engine->play(at(Vec3d{0, 64, 0})).has_value());
    }
    (void)rig.render(64);
    CHECK(rig.engine->stats().active_voices == 2);
    CHECK(rig.engine->stats().dropped_no_voice == 1);
    CHECK(rig.engine->stats().stolen == 0);
}

TEST_CASE("full: a muted category never takes a voice from a heard one", "[audio][priority]") {
    Rig rig{1};
    rig.engine->set_listener(Listener{Vec3d{0, 64, 0}, 0.0F});
    rig.engine->set_volume(SoundCategory::Hostile, 0.0F);
    REQUIRE(rig.engine->play(at(Vec3d{0, 64, 10})).has_value());
    (void)rig.render(64);
    REQUIRE(rig.engine->play(at(Vec3d{0, 64, 0}, SoundCategory::Hostile)).has_value());
    (void)rig.render(64);
    CHECK(rig.engine->stats().stolen == 0);
    CHECK(rig.engine->stats().dropped_no_voice == 1);
}

// ── Fades ───────────────────────────────────────────────────────────────────

TEST_CASE("a fade multiplies a category without touching its volume", "[audio][fade]") {
    Rig rig;
    rig.engine->set_volume(SoundCategory::Music, 0.8F);
    rig.engine->set_fade(SoundCategory::Music, 0.5F);
    PlayRequest request = at(Vec3d{}, SoundCategory::Music);
    request.relative    = true;
    REQUIRE(rig.engine->play(request).has_value());
    CHECK_THAT(rig.render(256)[200], WithinAbs(0.5 * 0.8 * 0.5 * kCentre, 1e-4));
    CHECK(rig.engine->volume(SoundCategory::Music) == 0.8F);
    CHECK(rig.engine->fade(SoundCategory::Music) == 0.5F);
    CHECK(rig.engine->catalog().find("test.flat") != nullptr);

    rig.engine->set_fade(SoundCategory::Music, 1.0F);
    (void)rig.render(256);  // one callback to ramp
    CHECK_THAT(rig.render(256)[200], WithinAbs(0.5 * 0.8 * kCentre, 1e-4));
}
