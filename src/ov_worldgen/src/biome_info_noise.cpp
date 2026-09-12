#define OV_LOG_CATEGORY "worldgen"

// `Biome.BIOME_INFO_NOISE`, and the two placement modifiers that read it:
// `noise_threshold_count` and `noise_based_count`.
//
// The noise is a `PerlinSimplexNoise` over a *legacy* generator seeded 2345,
// with the single octave 0 — which reduces to one 2D simplex noise with
// factors of one. ── worldgen-3 ── The noise itself now lives in
// climate_noise.cpp, beside the two temperature noises built the same way.
//
// What these two modifiers gate is large and visible: the grass and flowers of
// every plains, and the kelp and coral of the warm oceans.

#include "overworld_feature.hpp"

#include "climate_noise.hpp"

#include "ov/base/log.hpp"

#include <cmath>

namespace ov::worldgen {

namespace {

class NoiseThresholdCount final : public PlacementModifier {
public:
    NoiseThresholdCount(f64 level, i32 below, i32 above)
        : level_(level), below_(below), above_(above) {}

    void positions(const FeatureContext&, const FeatureLevel&, FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        const f64 noise =
            biome_info_noise_value(static_cast<f64>(at.x) / 200.0, static_cast<f64>(at.z) / 200.0);
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
        const f64 noise = biome_info_noise_value(static_cast<f64>(at.x) / factor_,
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
