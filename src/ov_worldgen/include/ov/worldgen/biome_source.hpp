// Which biome a place is, from six climate values.
//
// There is no map and no seeded diagram. The world has six climate noises —
// temperature, humidity, continentalness, erosion, depth and weirdness — and a
// table of about seven and a half thousand boxes in that six-dimensional space,
// each labelled with a biome. A place gets the biome whose box is nearest.
//
// The table is *not* in the datapack: `multi_noise_biome_source_parameter_list`
// only names a preset, and the preset is Java. It is however exported by the
// official data generator into `reports/biome_parameters`, which is where this
// reads it — generated locally like every other piece of vanilla data, and
// committed nowhere.
//
// The nearest box is found by a plain scan. Vanilla builds an R-tree over the
// same boxes, but an R-tree search returns the true minimum rather than an
// approximation, so the answer is identical and only the speed differs.
#pragma once

#include "ov/base/types.hpp"
#include "ov/worldgen/density.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <array>
#include <vector>

namespace ov::worldgen {

/// A climate reading, in the fixed-point form the comparison uses.
///
/// Quantised by ten thousand and held as integers, exactly as the game does:
/// the distance between two boxes is an integer sum of squares, so there is no
/// rounding anywhere in the search and two builds cannot disagree about a
/// boundary.
struct ClimatePoint {
    std::array<i64, 7> coordinates{};
};

class BiomeSource {
public:
    /// Read the exported parameter table.
    [[nodiscard]] static std::expected<BiomeSource, DensityError> load(
        const std::filesystem::path& data_root, std::string_view dimension);

    /// The climate at a *quart* position — the 4x4x4 cell biomes are stored
    /// in, not a block position.
    [[nodiscard]] ClimatePoint sample(const NoiseRouter& router, i32 quart_x, i32 quart_y,
                                      i32 quart_z) const;

    /// The biome whose box is nearest, by name.
    [[nodiscard]] std::string_view biome_at(const ClimatePoint& climate) const;

    /// How far a climate is from the nearest box of a named biome, per axis.
    ///
    /// The whole point of a parity harness is to say *which* axis is wrong, not
    /// merely that something is. When the game names a biome we did not, the
    /// axis carrying the distance to that biome's nearest box is the one to
    /// look at.
    [[nodiscard]] std::array<i64, 7> gap_to(const ClimatePoint& climate,
                                            std::string_view    biome) const;

    /// The squared distance to the nearest box of a named biome. Equal
    /// distances mean a tie, which is decided by the order of the table rather
    /// than by the climate — a different kind of disagreement from a numeric
    /// one, and worth telling apart.
    [[nodiscard]] i64 distance_to(const ClimatePoint& climate, std::string_view biome) const;

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }

    /// The biome one box belongs to.
    [[nodiscard]] std::string_view entry_biome(usize index) const;

    /// The middle of one box.
    ///
    /// Exposed so a test can ask the question that matters: is every biome in
    /// the table actually *reachable*? A box that another box swallows would
    /// name a biome the world can never contain, and nothing about the
    /// generated terrain would reveal it.
    [[nodiscard]] ClimatePoint entry_centre(usize index) const;

    /// Every distinct biome, sorted.
    [[nodiscard]] std::vector<std::string_view> biomes() const;
    [[nodiscard]] usize biome_count() const noexcept;

    /// Quantise a climate value the way the game does.
    [[nodiscard]] static i64 quantise(f32 value) noexcept {
        return static_cast<i64>(value * 10000.0F);
    }

private:
    struct Range {
        i64 low{0};
        i64 high{0};

        /// Zero inside the range, and the gap to the nearer edge outside it.
        [[nodiscard]] i64 distance(i64 value) const noexcept {
            const i64 above = value - high;
            const i64 below = low - value;
            return above > 0 ? above : (below > 0 ? below : 0);
        }
    };

    struct Entry {
        std::array<Range, 7> box{};
        std::string          biome;
    };

    std::vector<Entry> entries_;
};

}  // namespace ov::worldgen
