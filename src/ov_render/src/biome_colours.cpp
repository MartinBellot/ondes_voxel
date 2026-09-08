#define OV_LOG_CATEGORY "render"

#include "ov/render/biome_colours.hpp"

#include "ov/base/log.hpp"
#include "png.hpp"

#include <algorithm>
#include <cmath>

namespace ov::render {

namespace {

constexpr std::string_view kGrassPath   = "assets/minecraft/textures/colormap/grass.png";
constexpr std::string_view kFoliagePath = "assets/minecraft/textures/colormap/foliage.png";

/// Vanilla's own swamp grass colour.
///
/// The real modifier picks between two values from a noise field, so a swamp's
/// grass varies in patches. Reproducing that needs the exact noise, which is
/// not documented anywhere this project may read, so the commoner of the two
/// values is used for the whole biome. A swamp is therefore uniformly the right
/// green rather than patchily the right green — a visible difference, and one
/// that will not be guessed away: it stays here until the noise is measured.
constexpr Rgb kSwampGrass = 0x6A7039;

/// dark_forest darkens whatever the colormap gave it.
[[nodiscard]] constexpr Rgb dark_forest(Rgb colour) noexcept {
    return ((colour & 0xFEFEFEU) + 0x28340AU) >> 1U;
}

}  // namespace

std::string_view to_string(BiomeColourError error) noexcept {
    switch (error) {
        case BiomeColourError::MissingColormap:
            return "the pack has no colormap/grass.png or colormap/foliage.png";
        case BiomeColourError::BadColormap:
            return "a colormap would not decode, or is not 256x256";
    }
    return "unknown";
}

Rgb BiomeColours::sample_colormap(std::span<const u8> rgba, u32 width, f64 temperature,
                                  f64 rainfall) noexcept {
    const f64 t = std::clamp(temperature, 0.0, 1.0);
    // Rainfall is scaled by temperature before it is used, which is what makes
    // the lower-right half of the image unreachable: there is no cold, wet
    // climate to sample there, and the pixels are left undefined.
    const f64 r = std::clamp(rainfall, 0.0, 1.0) * t;

    const auto x = static_cast<u32>((1.0 - t) * 255.0);
    const auto y = static_cast<u32>((1.0 - r) * 255.0);

    const usize index = (static_cast<usize>(y) * width + x) * 4;
    if (index + 2 >= rgba.size()) {
        // Vanilla returns magenta here rather than clamping. Keeping that is
        // deliberate: an out-of-range climate should be seen, not smoothed
        // over into a plausible green.
        return 0xFF00FF;
    }
    return (static_cast<Rgb>(rgba[index]) << 16U) | (static_cast<Rgb>(rgba[index + 1]) << 8U) |
           static_cast<Rgb>(rgba[index + 2]);
}

std::expected<BiomeColours, BiomeColourError> BiomeColours::load(
    const AssetSource& assets, const registry::BlockRegistry& registry) {
    const auto grass_bytes   = assets.read(kGrassPath);
    const auto foliage_bytes = assets.read(kFoliagePath);
    if (!grass_bytes || !foliage_bytes) {
        return std::unexpected(BiomeColourError::MissingColormap);
    }

    const auto grass_image   = png::decode_rgba8(*grass_bytes);
    const auto foliage_image = png::decode_rgba8(*foliage_bytes);
    if (!grass_image || !foliage_image) {
        return std::unexpected(BiomeColourError::BadColormap);
    }
    if (grass_image->width != 256 || grass_image->height != 256 || foliage_image->width != 256 ||
        foliage_image->height != 256) {
        return std::unexpected(BiomeColourError::BadColormap);
    }

    BiomeColours colours;
    colours.entries_.resize(registry.biome_count());
    for (u32 index = 0; index < registry.biome_count(); ++index) {
        const auto effects = registry.biome(index);
        Entry&     entry   = colours.entries_[index];

        entry.water = effects.water_colour & 0xFFFFFFU;

        entry.grass = effects.grass_override >= 0
                          ? static_cast<Rgb>(effects.grass_override)
                          : sample_colormap(grass_image->rgba, grass_image->width,
                                            effects.temperature, effects.downfall);
        entry.foliage = effects.foliage_override >= 0
                            ? static_cast<Rgb>(effects.foliage_override)
                            : sample_colormap(foliage_image->rgba, foliage_image->width,
                                              effects.temperature, effects.downfall);

        switch (effects.grass_modifier) {
            case 1:
                entry.grass = dark_forest(entry.grass);
                break;
            case 2:
                entry.grass = kSwampGrass;
                break;
            default:
                break;
        }
    }
    OV_LOG_INFO("biomes: {} resolved against the pack's colormaps", colours.entries_.size());
    return colours;
}

Rgb BiomeColours::grass(u32 biome) const noexcept {
    return biome < entries_.size() ? entries_[biome].grass : Entry{}.grass;
}

Rgb BiomeColours::foliage(u32 biome) const noexcept {
    return biome < entries_.size() ? entries_[biome].foliage : Entry{}.foliage;
}

Rgb BiomeColours::water(u32 biome) const noexcept {
    return biome < entries_.size() ? entries_[biome].water : Entry{}.water;
}

}  // namespace ov::render
