// The three fixed-seed climate noises of `Biome`, and the temperature they
// make. Private to the module.
//
// They are `PerlinSimplexNoise` stacks over *legacy* generators with constant
// seeds, so they are the same noise in every world:
//
//   BIOME_INFO_NOISE            seed 2345, octave 0
//   TEMPERATURE_NOISE           seed 1234, octave 0
//   FROZEN_TEMPERATURE_NOISE    seed 3456, octaves -2, -1, 0
//
// ── worldgen-3 ── Moved out of biome_info_noise.cpp so that the frozen-ocean
// surface pass and the freezing of the top layer can ask the same questions:
// "does this frozen ocean melt its icebergs slightly" is a temperature.
#pragma once

#include "ov/base/types.hpp"

#include <span>

namespace ov::worldgen {

/// `BIOME_INFO_NOISE.getValue(x, z, false)`.
[[nodiscard]] f64 biome_info_noise_value(f64 x, f64 z) noexcept;

/// `TEMPERATURE_NOISE.getValue(x, z, false)`.
[[nodiscard]] f64 temperature_noise_value(f64 x, f64 z) noexcept;

/// `FROZEN_TEMPERATURE_NOISE.getValue(x, z, false)`.
[[nodiscard]] f64 frozen_temperature_noise_value(f64 x, f64 z) noexcept;

/// The temperature modifier a biome carries.
enum class TemperatureModifier : u8 { None = 0, Frozen = 1 };

/// `Biome.getHeightAdjustedTemperature(pos)`: the modifier applied to the base
/// temperature, then the cooling above y = 80. Float arithmetic, as the game's.
[[nodiscard]] f32 height_adjusted_temperature(f32 base, TemperatureModifier modifier, i32 x, i32 y,
                                              i32 z) noexcept;

}  // namespace ov::worldgen
