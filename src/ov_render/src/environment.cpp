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

f32 fog_sky_blend(f32 render_distance_chunks) noexcept {
    const f32 base = 0.25F + 0.75F * render_distance_chunks / 32.0F;
    return 1.0F - std::pow(base, 0.25F);
}

u32 blend_fog_towards_sky(u32 fog, u32 sky, f32 render_distance_chunks) noexcept {
    const f32 amount = fog_sky_blend(render_distance_chunks);
    const auto mix   = [amount](u32 a, u32 b) {
        const f32 fa = static_cast<f32>(a) / 255.0F;
        const f32 fb = static_cast<f32>(b) / 255.0F;
        return to_byte(fa + (fb - fa) * amount);
    };
    return (mix(channel(fog, 16), channel(sky, 16)) << 16) |
           (mix(channel(fog, 8), channel(sky, 8)) << 8) | mix(channel(fog, 0), channel(sky, 0));
}

f32 terrain_fog_start(f32 render_distance_blocks) noexcept {
    return render_distance_blocks - std::clamp(render_distance_blocks / 10.0F, 4.0F, 64.0F);
}

std::vector<f32> sky_disc(f32 height) {
    // Rim vertices at -180, -135, ... 180 degrees: nine, the last closing the
    // fan on the first. Wound by the sign of the height, so that both discs
    // face the eye.
    std::vector<f32> out;
    out.reserve(8 * 9);
    const f32 radius = height >= 0.0F ? kSkyDiscRadius : -kSkyDiscRadius;
    for (i32 step = 0; step < 8; ++step) {
        const f64 a0 = static_cast<f64>(-180 + step * 45) * std::numbers::pi / 180.0;
        const f64 a1 = static_cast<f64>(-180 + (step + 1) * 45) * std::numbers::pi / 180.0;
        const f32 p[9] = {0.0F,
                          height,
                          0.0F,
                          radius * static_cast<f32>(std::cos(a0)),
                          height,
                          kSkyDiscRadius * static_cast<f32>(std::sin(a0)),
                          radius * static_cast<f32>(std::cos(a1)),
                          height,
                          kSkyDiscRadius * static_cast<f32>(std::sin(a1))};
        out.insert(out.end(), p, p + 9);
    }
    return out;
}

void Lightmap::update(f32 darken, f32 ambient_light, f32 gamma, f32 flicker) noexcept {
    // Fitted against the real client's own lightmap texels, dumped by
    // scripts/render_parity_oracle.java (docs/provenance/rendu-parite.md):
    // every texel of the noon frames reproduces exactly with this structure.
    //
    // ⚠️ Two constants only act away from full daylight — the floor of the
    // sky's brightness and the lift of its colour towards white — and are
    // held here as named hypotheses until the oracle's night sweep confirms
    // or replaces them.
    constexpr f32 kSkyFloor    = 0.05F;
    constexpr f32 kSkyBlueLift = 0.35F;
    constexpr f32 kBaseLight   = 0.04F;
    constexpr f32 kBaseTarget  = 0.75F;

    const f32 d         = std::clamp(darken, 0.0F, 1.0F);
    const f32 sky_scale = d * (1.0F - kSkyFloor) + kSkyFloor;
    // Sky light is blue at night: its colour runs from (d, d, 1) lifted
    // towards white, so a moonlit field is blue-grey and not grey.
    const f32 sky_red  = d + (1.0F - d) * kSkyBlueLift;
    const f32 sky_blue = 1.0F;
    const f32 amount   = std::clamp(gamma, 0.0F, 1.0F);

    const auto base = [](f32 x) { return x + (kBaseTarget - x) * kBaseLight; };

    for (u32 sky = 0; sky < kSize; ++sky) {
        for (u32 block = 0; block < kSize; ++block) {
            const f32 sky_term = light_brightness(sky, ambient_light) * sky_scale;

            // Block light, warm: red at full, green and blue falling away
            // below full on two polynomials — orange in the middle of the
            // range, white at the top. The flicker rides on the 1.5.
            const f32 b     = light_brightness(block, ambient_light) * (flicker + 1.5F);
            const f32 red   = b;
            const f32 green = b * ((b * 0.6F + 0.4F) * 0.6F + 0.4F);
            const f32 blue  = b * (b * b * 0.6F + 0.4F);

            // Added: a torch reads against daylight.
            f32 c[3] = {red + sky_red * sky_term, green + sky_red * sky_term,
                        blue + sky_blue * sky_term};
            for (f32& x : c) {
                // Never quite black, clamped, eased by the brightness slider
                // (0 Moody, 1 Bright, 1.20.1's default 0.5), clamped, and
                // pulled towards 0.75 a second time — why a sunlit texel is
                // 252 and not 255.
                x = std::clamp(base(x), 0.0F, 1.0F);
                x = x + (ease_out_quart(x) - x) * amount;
                x = std::clamp(base(x), 0.0F, 1.0F);
            }

            // Truncated, not rounded: rounding puts two texels in three a unit
            // above the game's.
            const usize index  = (static_cast<usize>(sky) * kSize + block) * 4;
            pixels_[index]     = static_cast<u8>(c[0] * 255.0F);
            pixels_[index + 1] = static_cast<u8>(c[1] * 255.0F);
            pixels_[index + 2] = static_cast<u8>(c[2] * 255.0F);
            pixels_[index + 3] = 255;
        }
    }
}

}  // namespace ov::render
