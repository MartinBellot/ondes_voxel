// The day cycle, the lightmap and the fog: everything that decides how bright
// a place is and what colour the air between you and it has.
//
// Almost all of this is documented, and where it is not that is said so rather
// than guessed at. Three formulas carry the file:
//
//   brightness(L) = ambient + (1 - ambient) * f / (4 - 3f),   f = L/15
//   A             = clamp(2 * cos(angle * 2pi) + 0.5, 0, 1)
//   fog           = biome_fog * (0.94A + 0.06, 0.94A + 0.06, 0.91A + 0.09)
//
// The first is what makes a Minecraft cave dark: light level 7 renders at 18 %
// brightness, not the 47 % a straight L/15 would give. The renderer used the
// straight line until now, which is why every cave mouth looked like an
// overcast afternoon.
//
// No GPU here, on purpose: a brightness table and a tick-to-angle function are
// numbers that can be asserted against published values, and they are.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <span>
#include <vector>

namespace ov::render {

/// Ambient light of a dimension: the floor its brightness curve is lifted to.
/// Read from the dimension type, not invented — 1.20.1's overworld and end are
/// both 0 and the nether is 0.1.
inline constexpr f32 kOverworldAmbientLight = 0.0F;
inline constexpr f32 kNetherAmbientLight    = 0.1F;

/// How a light level 0..15 renders.
///
/// Evaluated as `f = L/15` and then `f/(4-3f)`, in that order and in f32. The
/// algebraically identical `L/(60-3L)` gives *exactly* 0.5 at level 12 and
/// exactly 7/9 at level 14, where the shipped values are 0.50000006 and
/// 0.77777773 — the fingerprint of this evaluation order in binary32. Keeping
/// the order keeps those two values, which is what makes the table testable.
[[nodiscard]] f32 light_brightness(u32 level, f32 ambient_light) noexcept;

/// Where the sun is, as a fraction of a full turn. 0 is noon.
///
/// The two halves of the day are not symmetric: the raw fraction is bent by a
/// third of the way towards a cosine, which is what makes dawn and dusk take
/// longer than the arithmetic would suggest.
[[nodiscard]] f64 celestial_angle(i64 time_of_day) noexcept;

/// The factor everything about the sky is multiplied by: 1 in full day, 0 at
/// night, sliding between over dusk and dawn.
///
/// Checked against the tick landmarks the day cycle is documented by — sky
/// light reaches its night minimum at 13670 and begins to climb at 22331 — and
/// against the three noon values, 15 clear, 12 in rain, 10 in a thunderstorm.
[[nodiscard]] f32 sky_darken(i64 time_of_day, f32 rain, f32 thunder) noexcept;

/// The integer the *gameplay* side calls internal sky light, from the same
/// factor. Kept beside it because the two must not drift apart: mobs spawn
/// against this one and the screen is lit by the other.
[[nodiscard]] u8 internal_sky_light(f32 darken) noexcept;

/// The fog colour, from the biome's and the time of day.
///
/// Blue outlasts red and green on the way down — 0.91A + 0.09 against
/// 0.94A + 0.06 — which is why midnight fog is dark blue rather than dark grey.
[[nodiscard]] u32 fog_colour(u32 biome_fog, f32 darken) noexcept;

/// The sky colour: the biome's, scaled. Black at night, exactly.
[[nodiscard]] u32 sky_colour(u32 biome_sky, f32 darken) noexcept;

/// The sky disc: a flat fan of eight triangles `height` blocks above the eye,
/// 512 blocks across the radius, its rim vertices every 45 degrees starting
/// from -X. Nine floats a triangle, positions relative to the eye. A negative
/// height gives the dark disc under the horizon, wound the other way.
///
/// Eight triangles and not a smooth dome because the fog is interpolated
/// linearly across each one, so the fan shows in the gradient — and a smooth
/// dome would put the fog's colour at a different height in every direction
/// but eight.
inline constexpr f32 kSkyDiscHeight = 16.0F;
inline constexpr f32 kSkyDiscRadius = 512.0F;
[[nodiscard]] std::vector<f32> sky_disc(f32 height);

/// Vanilla's 16x16 lightmap, rebuilt every frame.
///
/// Not a formula in a shader: the game builds this texture on the CPU and
/// samples it at (block light, sky light). Doing the same means the flicker of
/// a torch, the gamma slider and the fade of a sunset are all changes to one
/// small image rather than to the terrain shader.
class Lightmap {
public:
    static constexpr u32 kSize  = 16;
    static constexpr u32 kBytes = kSize * kSize * 4;

    /// Rebuild for a moment in time.
    ///
    /// `gamma` is the brightness slider, 0 (Moody) to 1 (Bright); 1.20.1
    /// defaults to 0.5. `flicker` is the torch's, 0 to 1.
    void update(f32 darken, f32 ambient_light, f32 gamma, f32 flicker) noexcept;

    [[nodiscard]] std::span<const u8> pixels() const noexcept { return pixels_; }

private:
    std::array<u8, kBytes> pixels_{};
};

}  // namespace ov::render
