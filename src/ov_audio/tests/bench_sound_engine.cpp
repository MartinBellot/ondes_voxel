// What 200 sounds in one frame cost the frame thread and the mixer.
//
// The frame thread's share — play() for every sound, then update() — is what
// a burst (an explosion's blocks, a mob farm) adds to the frame record; the
// mixer's share runs on the device's own thread and is measured per callback
// of 512 frames (10.7 ms at 48 kHz). Both are measured with the voice cap of
// the shipped client (64, priority on) and without one (256: every sound gets
// a voice), so the cap's effect is a number and not a belief.
//
// Printed, not asserted beyond a loose ceiling: a timing that fails on a busy
// machine is noise. The numbers go in docs/provenance/son.md.
#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::audio;

namespace {

constexpr std::string_view kDocument = R"json({
  "test.a": {"sounds": ["test/a", "test/b", "test/c", "test/d"]}
})json";

struct Result {
    f64 play_p50{0}, play_p99{0};
    f64 mix_p50{0}, mix_p99{0};
    u64 dropped{0}, stolen{0};
};

f64 percentile(std::vector<f64> values, f64 p) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<usize>(p * static_cast<f64>(values.size() - 1));
    return values[index];
}

Result run(u32 voices, u32 per_frame, u32 frames) {
    auto catalog = SoundCatalog::parse(kDocument);
    REQUIRE(catalog.has_value());
    EngineDesc desc;
    desc.catalog    = &*catalog;
    desc.read       = [](std::string_view) { return std::optional<std::vector<u8>>{}; };
    desc.backend    = Backend::Null;
    desc.max_voices = voices;
    auto engine     = SoundEngine::create(desc);
    REQUIRE(engine.has_value());
    SoundEngine& e = **engine;
    // Four one-second waveforms, so sounds overlap across frames.
    std::vector<i16> wave(48000);
    for (usize i = 0; i < wave.size(); ++i) {
        wave[i] = static_cast<i16>((i * 37) % 20000) - 10000;
    }
    for (const char* name : {"a", "b", "c", "d"}) {
        e.add_pcm(std::string{"assets/minecraft/sounds/test/"} + name + ".ogg", wave, 1, 48000);
    }
    e.set_listener(Listener{Vec3d{0, 64, 0}, 30.0F, 10.0F});

    std::vector<f64> play_ms;
    std::vector<f64> mix_ms;
    std::vector<f32> out(512 * 2);
    u64              seed = 1;
    for (u32 frame = 0; frame < frames; ++frame) {
        const auto started = std::chrono::steady_clock::now();
        for (u32 k = 0; k < per_frame; ++k) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            PlayRequest request;
            request.event    = "test.a";
            request.category = SoundCategory::Block;
            // A ring of sources 1 to 30 blocks out: some loud, some inaudible.
            const f64 r      = 1.0 + static_cast<f64>((seed >> 33) % 30);
            const f64 a      = static_cast<f64>((seed >> 17) % 628) / 100.0;
            request.position = Vec3d{r * std::cos(a), 64.0, r * std::sin(a)};
            request.seed     = static_cast<i64>(seed);
            (void)e.play(request);
        }
        e.update();
        play_ms.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count());
        const auto mixed = std::chrono::steady_clock::now();
        e.render(out);
        mix_ms.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - mixed).count());
    }
    Result result;
    result.play_p50 = percentile(play_ms, 0.50);
    result.play_p99 = percentile(play_ms, 0.99);
    result.mix_p50  = percentile(mix_ms, 0.50);
    result.mix_p99  = percentile(mix_ms, 0.99);
    result.dropped  = e.stats().dropped_no_voice;
    result.stolen   = e.stats().stolen;
    return result;
}

}  // namespace

TEST_CASE("bench: 200 sounds a frame, with and without the voice cap", "[audio][bench]") {
    // Every frame starts 200 sounds — a worst case that never lets up — and a
    // control run starts none, so the burst's cost stands out from the floor.
    for (const u32 voices : {64U, 256U}) {
        const Result burst = run(voices, 200, 200);
        const Result idle  = run(voices, 0, 200);
        std::printf("voices %3u  200/frame: play+update p50 %.4f p99 %.4f ms  "
                    "mix/512 p50 %.4f p99 %.4f ms  dropped %llu stolen %llu\n",
                    voices, burst.play_p50, burst.play_p99, burst.mix_p50, burst.mix_p99,
                    static_cast<unsigned long long>(burst.dropped),
                    static_cast<unsigned long long>(burst.stolen));
        std::printf("voices %3u    0/frame: play+update p50 %.4f p99 %.4f ms  "
                    "mix/512 p50 %.4f p99 %.4f ms\n",
                    voices, idle.play_p50, idle.play_p99, idle.mix_p50, idle.mix_p99);
        // A callback of 512 frames is 10.7 ms of sound: past it the device
        // underruns — a stutter. Held for the shipped cap only: without one the
        // burst does not fit in a Debug build (19 ms measured), which is the
        // point of the cap.
        if (voices == 64) {
            CHECK(burst.mix_p99 < 10.7);
        }
    }
}
