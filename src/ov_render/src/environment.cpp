#include "ov/render/environment.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::render {

namespace {

constexpr f64 kTicksPerDay = 24000.0;

[[nodiscard]] constexpr u32 channel(u32 colour, u32 shift) noexcept {
    return (colour >> shift) & 0xFFU;
}

[[nodiscard]] u32 to_byte(f32 value) noexcept {
    return static_cast<u32>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

/// The easing the brightness slider brightens with.
[[nodiscard]] f32 ease_out_quart(f32 x) noexcept {
    const f32 inverted = 1.0F - x;
    return 1.0F - inverted * inverted * inverted * inverted;
}

}  // namespace

f32 light_brightness(u32 level, f32 ambient_light) noexcept {
    const f32 f = static_cast<f32>(std::min(level, 15U)) / 15.0F;
    const f32 v = f / (4.0F - 3.0F * f);
    return ambient_light + (1.0F - ambient_light) * v;
}

f64 celestial_angle(i64 time_of_day) noexcept {
    // Noon is 6000, and the angle is measured from there, so the quarter-day
    // offset is not decoration.
    const i64 day  = ((time_of_day % 24000) + 24000) % 24000;
    const f64 x    = static_cast<f64>(day) / kTicksPerDay - 0.25;
    const f64 wrapped = x < 0.0 ? x + 1.0 : x;

    // A third of the way from the linear fraction towards a cosine. This is
    // what stretches dawn and dusk: without it the sun would cross the horizon
    // at the same rate it crosses the sky.
    const f64 eased = 1.0 - (std::cos(wrapped * std::numbers::pi) + 1.0) / 2.0;
    return wrapped + (eased - wrapped) / 3.0;
}

f32 sky_darken(i64 time_of_day, f32 rain, f32 thunder) noexcept {
    const f64 angle  = celestial_angle(time_of_day);
    const f64 factor = std::clamp(2.0 * std::cos(angle * 2.0 * std::numbers::pi) + 0.5, 0.0, 1.0);

    // Rain takes five sixteenths of the light, and a thunderstorm takes five
    // sixteenths of what is left — which is why a storm at noon reads 10 and
    // not 9.
    f64 result = factor;
    result *= 1.0 - static_cast<f64>(std::clamp(rain, 0.0F, 1.0F)) * 5.0 / 16.0;
    result *= 1.0 - static_cast<f64>(std::clamp(thunder, 0.0F, 1.0F)) * 5.0 / 16.0;
    return static_cast<f32>(result);
}

u8 internal_sky_light(f32 darken) noexcept {
    const f32 level = 15.0F - std::floor((1.0F - std::clamp(darken, 0.0F, 1.0F)) * 11.0F);
    return static_cast<u8>(std::clamp(level, 0.0F, 15.0F));
}

u32 fog_colour(u32 biome_fog, f32 darken) noexcept {
    const f32 a = std::clamp(darken, 0.0F, 1.0F);
    // Blue keeps a larger floor than red and green, which is the whole reason
    // midnight fog is dark blue and not dark grey.
    const f32 rg = 0.94F * a + 0.06F;
    const f32 b  = 0.91F * a + 0.09F;
    return (to_byte(static_cast<f32>(channel(biome_fog, 16)) / 255.0F * rg) << 16U) |
           (to_byte(static_cast<f32>(channel(biome_fog, 8)) / 255.0F * rg) << 8U) |
           to_byte(static_cast<f32>(channel(biome_fog, 0)) / 255.0F * b);
}

u32 sky_colour(u32 biome_sky, f32 darken) noexcept {
    const f32 a = std::clamp(darken, 0.0F, 1.0F);
    return (to_byte(static_cast<f32>(channel(biome_sky, 16)) / 255.0F * a) << 16U) |
           (to_byte(static_cast<f32>(channel(biome_sky, 8)) / 255.0F * a) << 8U) |
           to_byte(static_cast<f32>(channel(biome_sky, 0)) / 255.0F * a);
}

void Lightmap::update(f32 darken, f32 ambient_light, f32 gamma, f32 flicker) noexcept {
    for (u32 sky = 0; sky < kSize; ++sky) {
        for (u32 block = 0; block < kSize; ++block) {
            const f32 sky_term = light_brightness(sky, ambient_light) * darken;

            // Block light is brighter than its level alone: a torch at 14 has
            // to hold its own against a sky at 15, and the flicker rides on
            // top of it.
            const f32 block_level = light_brightness(block, ambient_light);
            const f32 block_term  = block_level * (flicker * 0.1F + 1.5F);

            // Sky light is white; block light is warm, and warms as it
            // strengthens: nearly grey at the bottom of the range, orange in
            // the middle, white at the top. The shape of that ramp is
            // documented; the exact polynomial 1.20.1 uses is NOT, so this
            // reproduces the description rather than the code, and stays here
            // until it is measured against the real client.
            const f32 warmth = std::clamp(block_level, 0.0F, 1.0F);
            const f32 tilt   = warmth * (1.0F - warmth) * 4.0F;
            const f32 red    = block_term;
            const f32 green  = block_term * (1.0F - tilt * (1.0F - 216.0F / 255.0F));
            const f32 blue   = block_term * (1.0F - tilt * (1.0F - 140.0F / 255.0F));

            // Added rather than maximised. The sources disagree — a 2011
            // write-up says the game takes the brighter of the two, the modern
            // dimension documentation says they add — and adding is what makes
            // a torch read against daylight, which is the case a max cannot
            // produce. Undecided by documentation; a candidate for measurement.
            f32 r = sky_term + red;
            f32 g = sky_term + green;
            f32 b = sky_term + blue;

            // Never quite black. The strength is documented as 0.04; the grey
            // it moves towards is not, and 0.75 is the value everyone repeats.
            constexpr f32 kBaseLight  = 0.04F;
            constexpr f32 kBaseTarget = 0.75F;
            r += (kBaseTarget - r) * kBaseLight;
            g += (kBaseTarget - g) * kBaseLight;
            b += (kBaseTarget - b) * kBaseLight;

            // The brightness slider: lerp towards an eased-up version of the
            // colour. 0 is Moody, 1 is Bright, and 1.20.1 defaults to 0.5.
            const f32 amount = std::clamp(gamma, 0.0F, 1.0F);
            r = r + (ease_out_quart(std::clamp(r, 0.0F, 1.0F)) - r) * amount;
            g = g + (ease_out_quart(std::clamp(g, 0.0F, 1.0F)) - g) * amount;
            b = b + (ease_out_quart(std::clamp(b, 0.0F, 1.0F)) - b) * amount;

            const usize index    = (static_cast<usize>(sky) * kSize + block) * 4;
            pixels_[index]       = static_cast<u8>(to_byte(r));
            pixels_[index + 1]   = static_cast<u8>(to_byte(g));
            pixels_[index + 2]   = static_cast<u8>(to_byte(b));
            pixels_[index + 3]   = 255;
        }
    }
}

}  // namespace ov::render
