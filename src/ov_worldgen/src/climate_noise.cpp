#define OV_LOG_CATEGORY "worldgen"

// The fixed-seed climate noises of `Biome`, as the generator asks for them.
//
// ── worldgen-3 ── A thin adapter. The noises and the temperature are already
// written, and measured, in ov_gameplay's weather (layer 9, below this one):
// `gameplay::ClimateNoise` carries the frozen patches and the cooling above
// sea level + 17, validated at 11 285 / 11 285 frozen-ocean ice columns of the
// reference world (docs/provenance/meteo-sommeil.md). Writing them a third
// time here — biome_info_noise.cpp already held a copy of the simplex — would
// be three places for one bug.

#include "climate_noise.hpp"

#include "ov/gameplay/weather.hpp"
#include "ov/math/random.hpp"

#include <array>
#include <cstdlib>
#include <string_view>

namespace ov::worldgen {

namespace {

[[nodiscard]] gameplay::PerlinSimplexNoise make_noise(i64 seed, std::span<const i32> octaves) {
    math::LegacyRandomSource random{seed};
    return gameplay::PerlinSimplexNoise{random, octaves};
}

// Built once per process and never written again: they depend on constants
// only, so they are the same in every world and cannot be seeded wrong by one.
[[nodiscard]] const gameplay::PerlinSimplexNoise& biome_info() {
    static constexpr std::array<i32, 1> kOctaves{0};
    static const auto                   noise = make_noise(2345, kOctaves);
    return noise;
}
[[nodiscard]] const gameplay::PerlinSimplexNoise& temperature() {
    static constexpr std::array<i32, 1> kOctaves{0};
    static const auto                   noise = make_noise(1234, kOctaves);
    return noise;
}
[[nodiscard]] const gameplay::PerlinSimplexNoise& frozen_temperature() {
    static constexpr std::array<i32, 3> kOctaves{-2, -1, 0};
    static const auto                   noise = make_noise(3456, kOctaves);
    return noise;
}
[[nodiscard]] const gameplay::ClimateNoise& climate() {
    static const gameplay::ClimateNoise noise;
    return noise;
}

}  // namespace

f64 biome_info_noise_value(f64 x, f64 z) noexcept {
    return biome_info().value(x, z, false);
}

f64 temperature_noise_value(f64 x, f64 z) noexcept {
    return temperature().value(x, z, false);
}

f64 frozen_temperature_noise_value(f64 x, f64 z) noexcept {
    return frozen_temperature().value(x, z, false);
}

f32 height_adjusted_temperature(f32 base, TemperatureModifier modifier, i32 x, i32 y,
                                i32 z) noexcept {
    // An instrument, read once: `OV_CLIMATE_FLAT=1` answers with the base
    // temperature alone — the control the noise is measured against.
    static const bool flat = [] {
        const char* setting = std::getenv("OV_CLIMATE_FLAT");
        return setting != nullptr && std::string_view(setting) == "1";
    }();
    if (flat) {
        return base;
    }
    gameplay::BiomeClimate biome;
    biome.temperature = base;
    biome.frozen      = modifier == TemperatureModifier::Frozen;
    return climate().temperature_at(biome, BlockPos{x, y, z});
}

}  // namespace ov::worldgen
