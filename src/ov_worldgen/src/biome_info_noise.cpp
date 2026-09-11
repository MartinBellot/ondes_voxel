#define OV_LOG_CATEGORY "worldgen"

// `Biome.BIOME_INFO_NOISE`, and the two placement modifiers that read it:
// `noise_threshold_count` and `noise_based_count`.
//
// The noise is a `PerlinSimplexNoise` over a *legacy* generator seeded 2345,
// with the single octave 0 — which reduces to one 2D simplex noise with
// factors of one. Nothing else in this generator is simplex noise, so it is
// written here in full: the offsets and the shuffle are those of
// `ImprovedNoise`, and the 2D sample is the standard three-corner simplex with
// Mojang's twelve-gradient table.
//
// What these two modifiers gate is large and visible: the grass and flowers of
// every plains, and the kelp and coral of the warm oceans.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/math/random.hpp"

#include <array>
#include <cmath>

namespace ov::worldgen {

namespace {

class SimplexNoise {
public:
    explicit SimplexNoise(math::LegacyRandomSource& random) {
        xo_ = random.next_double() * 256.0;
        yo_ = random.next_double() * 256.0;
        zo_ = random.next_double() * 256.0;
        for (usize i = 0; i < 256; ++i) {
            p_[i] = static_cast<i32>(i);
        }
        for (usize i = 0; i < 256; ++i) {
            const auto j = static_cast<usize>(random.next_int(static_cast<i32>(256 - i)));
            std::swap(p_[i], p_[i + j]);
        }
    }

    /// Two-dimensional simplex noise, in [-1, 1] roughly.
    [[nodiscard]] f64 value(f64 x, f64 y) const noexcept {
        static const f64 kF2 = 0.5 * (std::sqrt(3.0) - 1.0);
        static const f64 kG2 = (3.0 - std::sqrt(3.0)) / 6.0;
        const f64 skew = (x + y) * kF2;
        const i32 i    = static_cast<i32>(std::floor(x + skew));
        const i32 j    = static_cast<i32>(std::floor(y + skew));
        const f64 unskew = static_cast<f64>(i + j) * kG2;
        const f64 x0     = x - (static_cast<f64>(i) - unskew);
        const f64 y0     = y - (static_cast<f64>(j) - unskew);
        const i32 i1     = x0 > y0 ? 1 : 0;
        const i32 j1     = x0 > y0 ? 0 : 1;
        const f64 x1     = x0 - static_cast<f64>(i1) + kG2;
        const f64 y1     = y0 - static_cast<f64>(j1) + kG2;
        const f64 x2     = x0 - 1.0 + 2.0 * kG2;
        const f64 y2     = y0 - 1.0 + 2.0 * kG2;
        const i32 ii     = i & 0xFF;
        const i32 jj     = j & 0xFF;
        const i32 g0     = p(ii + p(jj)) % 12;
        const i32 g1     = p(ii + i1 + p(jj + j1)) % 12;
        const i32 g2     = p(ii + 1 + p(jj + 1)) % 12;
        return 70.0 * (corner(g0, x0, y0) + corner(g1, x1, y1) + corner(g2, x2, y2));
    }

private:
    [[nodiscard]] i32 p(i32 index) const noexcept { return p_[static_cast<usize>(index & 0xFF)]; }

    [[nodiscard]] static f64 corner(i32 gradient, f64 x, f64 y) noexcept {
        static constexpr std::array<std::array<i32, 3>, 16> kGradients{{
            {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
            {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
            {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1},
        }};
        f64 falloff = 0.5 - x * x - y * y;
        if (falloff < 0.0) {
            return 0.0;
        }
        falloff *= falloff;
        const auto& g = kGradients[static_cast<usize>(gradient)];
        return falloff * falloff * (static_cast<f64>(g[0]) * x + static_cast<f64>(g[1]) * y);
    }

    f64                  xo_{0.0};
    f64                  yo_{0.0};
    f64                  zo_{0.0};
    std::array<i32, 256> p_{};
};

/// `BIOME_INFO_NOISE`: one octave, factors of one, no offsets.
///
/// Built once per process: it depends on nothing but the constant 2345, so it
/// is the same noise in every world — which is also why it cannot be seeded
/// wrong by a world, only built wrong.
[[nodiscard]] const SimplexNoise& biome_info_noise() {
    static const SimplexNoise noise = [] {
        math::LegacyRandomSource random{2345};
        return SimplexNoise{random};
    }();
    return noise;
}

class NoiseThresholdCount final : public PlacementModifier {
public:
    NoiseThresholdCount(f64 level, i32 below, i32 above)
        : level_(level), below_(below), above_(above) {}

    void positions(const FeatureContext&, const FeatureLevel&, FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        const f64 noise =
            biome_info_noise().value(static_cast<f64>(at.x) / 200.0, static_cast<f64>(at.z) / 200.0);
        const i32 count = noise < level_ ? below_ : above_;
        for (i32 i = 0; i < count; ++i) {
            out.push_back(at);
        }
    }

    [[nodiscard]] std::string_view name() const override { return "noise_threshold_count"; }

private:
    f64 level_;
    i32 below_;
    i32 above_;
};

class NoiseBasedCount final : public PlacementModifier {
public:
    NoiseBasedCount(i32 ratio, f64 factor, f64 offset)
        : ratio_(ratio), factor_(factor), offset_(offset) {}

    void positions(const FeatureContext&, const FeatureLevel&, FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        const f64 noise = biome_info_noise().value(static_cast<f64>(at.x) / factor_,
                                                   static_cast<f64>(at.z) / factor_);
        const auto count = static_cast<i32>(std::ceil((noise + offset_) * static_cast<f64>(ratio_)));
        for (i32 i = 0; i < count; ++i) {
            out.push_back(at);
        }
    }

    [[nodiscard]] std::string_view name() const override { return "noise_based_count"; }

private:
    i32 ratio_;
    f64 factor_;
    f64 offset_;
};

[[nodiscard]] std::optional<f64> number(Json node, std::string_view key) {
    f64 value = 0.0;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return value;
    }
    i64 whole = 0;
    if (node.at_key(key).get(whole) == simdjson::SUCCESS) {
        return static_cast<f64>(whole);
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::expected<PlacementModifierRef, FeatureError>> parse_noise_placement(
    std::string_view kind, Json node) {
    if (kind == "noise_threshold_count") {
        const auto level = number(node, "noise_level");
        const auto below = number(node, "below_noise");
        const auto above = number(node, "above_noise");
        if (!level || !below || !above) {
            return std::unexpected(FeatureError::Malformed);
        }
        return std::static_pointer_cast<const PlacementModifier>(
            std::make_shared<const NoiseThresholdCount>(*level, static_cast<i32>(*below),
                                                        static_cast<i32>(*above)));
    }
    if (kind == "noise_based_count") {
        const auto ratio  = number(node, "noise_to_count_ratio");
        const auto factor = number(node, "noise_factor");
        const auto offset = number(node, "noise_offset");
        if (!ratio || !factor) {
            return std::unexpected(FeatureError::Malformed);
        }
        return std::static_pointer_cast<const PlacementModifier>(std::make_shared<const NoiseBasedCount>(
            static_cast<i32>(*ratio), *factor, offset.value_or(0.0)));
    }
    return std::nullopt;
}

}  // namespace ov::worldgen
