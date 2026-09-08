#include "ov/render/environment.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

TEST_CASE("the brightness curve matches the published table", "[render][environment]") {
    // The sixteen rendered brightnesses of the overworld, published
    // independently of any code. A straight level/15 would give 0.4667 at
    // level 7 where the game gives 0.1795 — the difference between a cave that
    // is dark and one that looks like an overcast afternoon.
    constexpr std::array<f32, 16> kOverworld{
        0.0F,        0.017543862F, 0.037037041F, 0.058823530F, 0.083333343F, 0.111111112F,
        0.142857149F, 0.179487184F, 0.222222254F, 0.272727311F, 0.333333343F, 0.407407433F,
        0.500000060F, 0.619047582F, 0.777777731F, 1.0F};

    for (u32 level = 0; level < 16; ++level) {
        CAPTURE(level);
        CHECK(light_brightness(level, kOverworldAmbientLight) == Approx(kOverworld[level]).margin(1e-7));
    }

    // Two of those values are the fingerprint of the evaluation order. Written
    // as level/(60 - 3*level) the arithmetic is identical on paper and gives
    // exactly 0.5 and exactly 7/9; the shipped values are neither, because the
    // game divides by 15 first and rounds there. If this ever starts passing
    // with the closed form, the order has been "simplified".
    CHECK(light_brightness(12, 0.0F) > 0.5F);
    CHECK(light_brightness(14, 0.0F) < 7.0F / 9.0F);

    // The nether's floor lifts the whole curve without changing its top.
    CHECK(light_brightness(0, kNetherAmbientLight) == Approx(0.1F));
    CHECK(light_brightness(15, kNetherAmbientLight) == Approx(1.0F));
    CHECK(light_brightness(8, kNetherAmbientLight) == Approx(0.3F).margin(1e-6));
}

TEST_CASE("the day cycle hits its documented tick landmarks", "[render][environment]") {
    // The daylight cycle is documented by the ticks at which sky light changes
    // value. Reproducing those is what makes the angle function right rather
    // than merely plausible: the eased third is invisible at noon and moves
    // these landmarks by hundreds of ticks.
    CHECK(internal_sky_light(sky_darken(6000, 0.0F, 0.0F)) == 15);   // noon, clear
    CHECK(internal_sky_light(sky_darken(6000, 1.0F, 0.0F)) == 12);   // noon, rain
    CHECK(internal_sky_light(sky_darken(6000, 1.0F, 1.0F)) == 10);   // noon, thunder

    // The night minimum is reached at 13670 and not before.
    CHECK(sky_darken(13669, 0.0F, 0.0F) > 0.0F);
    CHECK(sky_darken(13670, 0.0F, 0.0F) == Approx(0.0F).margin(1e-6));
    // And it starts climbing again at 22331.
    CHECK(sky_darken(22330, 0.0F, 0.0F) == Approx(0.0F).margin(1e-6));
    CHECK(sky_darken(22331, 0.0F, 0.0F) > 0.0F);

    // Mobs spawn when internal sky light reaches 7, at 13188 clear.
    CHECK(internal_sky_light(sky_darken(13188, 0.0F, 0.0F)) == 7);
    CHECK(internal_sky_light(sky_darken(13187, 0.0F, 0.0F)) > 7);

    // Midnight is the floor, and it is 4 rather than 0: the moon is not
    // nothing.
    CHECK(internal_sky_light(sky_darken(18000, 0.0F, 0.0F)) == 4);

    // A full turn comes back to where it started.
    CHECK(celestial_angle(0) == Approx(celestial_angle(24000)).margin(1e-12));
    CHECK(sky_darken(-6000, 0.0F, 0.0F) == Approx(sky_darken(18000, 0.0F, 0.0F)));
}

TEST_CASE("fog and sky colours fade the documented way", "[render][environment]") {
    // 54 of the 64 biomes share this fog colour, so it is the one that matters.
    constexpr u32 kOverworldFog = 0xC0D8FF;
    constexpr u32 kPlainsSky    = 0x78A7FF;

    // Full daylight returns the biome's colour untouched. Anything else means
    // the constants are the wrong way round.
    CHECK(fog_colour(kOverworldFog, 1.0F) == kOverworldFog);
    CHECK(sky_colour(kPlainsSky, 1.0F) == kPlainsSky);

    // The sky goes exactly black; the fog does not, and keeps more blue than
    // red — that is what makes midnight blue rather than grey.
    CHECK(sky_colour(kPlainsSky, 0.0F) == 0x000000);
    const u32 night_fog = fog_colour(kOverworldFog, 0.0F);
    const u32 red       = (night_fog >> 16) & 0xFF;
    const u32 blue      = night_fog & 0xFF;
    CHECK(night_fog != 0x000000);
    CHECK(blue > red);
}

TEST_CASE("the lightmap is a real image, dark where it should be", "[render][environment]") {
    Lightmap lightmap;
    lightmap.update(1.0F, kOverworldAmbientLight, 0.5F, 0.0F);
    REQUIRE(lightmap.pixels().size() == Lightmap::kBytes);

    const auto at = [&](u32 block, u32 sky) {
        const auto  pixels = lightmap.pixels();
        const usize index  = (static_cast<usize>(sky) * Lightmap::kSize + block) * 4;
        return std::array<u8, 3>{pixels[index], pixels[index + 1], pixels[index + 2]};
    };

    // Full daylight is bright; the same cell at night is not.
    const auto noon_open = at(0, 15);
    CHECK(noon_open[0] > 200);

    Lightmap night;
    night.update(0.0F, kOverworldAmbientLight, 0.5F, 0.0F);
    const auto night_pixels = night.pixels();
    CHECK(night_pixels[(15 * Lightmap::kSize + 0) * 4] < noon_open[0]);

    // Torchlight is warm: more red than blue, at every level that has any.
    for (u32 block = 1; block < 16; ++block) {
        CAPTURE(block);
        const auto lit = at(block, 0);
        CHECK(lit[0] >= lit[1]);
        CHECK(lit[1] >= lit[2]);
    }

    // And nothing is ever fully black, which is why a cave floor is visible as
    // a shape rather than as nothing at all.
    const auto darkest = at(0, 0);
    CHECK(darkest[0] > 0);
    CHECK(darkest[0] < 60);
}
