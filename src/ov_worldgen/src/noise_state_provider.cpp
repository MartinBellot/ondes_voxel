#define OV_LOG_CATEGORY "worldgen"

// The three block state providers driven by a noise field:
// `noise_provider`, `noise_threshold_provider`, `dual_noise_provider`.
//
// They choose *which* flower grows where — the tulip fields of the plains, the
// bands of a flower forest, the mixed meadow — and each one reads a
// `NormalNoise` built on a **legacy** generator seeded from the provider's own
// `seed` field: `NormalNoise.create(new WorldgenRandom(new
// LegacyRandomSource(seed)), parameters)`. noise.cpp builds its noises on
// Xoroshiro only, so the legacy form is built here: each Perlin stack takes a
// positional factory from one `nextLong`, and each octave is seeded by the Java
// string hash of `octave_<n>`.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/noise.hpp"

#include <cmath>
#include <limits>

namespace ov::worldgen {

namespace {

/// One Perlin stack seeded by name from a legacy positional factory.
class LegacyPerlin {
public:
    LegacyPerlin(math::LegacyRandomSource& random, i32 first_octave, std::vector<f64> amplitudes)
        : amplitudes_(std::move(amplitudes)) {
        const math::LegacyPositionalFactory factory{static_cast<u64>(random.next_long())};
        octaves_.resize(amplitudes_.size());
        for (usize i = 0; i < amplitudes_.size(); ++i) {
            if (amplitudes_[i] == 0.0) {
                continue;
            }
            auto source = factory.from_hash_of("octave_" +
                                               std::to_string(first_octave + static_cast<i32>(i)));
            octaves_[i] = std::make_unique<ImprovedNoise>(source);
        }
        const auto count     = static_cast<f64>(amplitudes_.size());
        lowest_input_factor_ = std::pow(2.0, static_cast<f64>(first_octave));
        lowest_value_factor_ = std::pow(2.0, count - 1.0) / (std::pow(2.0, count) - 1.0);
    }

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z) const noexcept {
        f64 total  = 0.0;
        f64 input  = lowest_input_factor_;
        f64 output = lowest_value_factor_;
        for (usize i = 0; i < octaves_.size(); ++i) {
            if (octaves_[i] != nullptr) {
                total += amplitudes_[i] *
                         octaves_[i]->noise(PerlinNoise::wrap(x * input), PerlinNoise::wrap(y * input),
                                            PerlinNoise::wrap(z * input)) *
                         output;
            }
            input *= 2.0;
            output /= 2.0;
        }
        return total;
    }

private:
    std::vector<f64>                            amplitudes_;
    std::vector<std::unique_ptr<ImprovedNoise>> octaves_;
    f64                                         lowest_input_factor_{1.0};
    f64                                         lowest_value_factor_{1.0};
};

/// `NormalNoise` over a legacy generator: two stacks, the second sampled at
/// the fixed off-round factor, scaled by the span of non-zero octaves.
class LegacyNormalNoise {
public:
    LegacyNormalNoise(i64 seed, i32 first_octave, const std::vector<f64>& amplitudes)
        : random_(seed),
          first_(random_, first_octave, amplitudes),
          second_(random_, first_octave, amplitudes) {
        i32 lowest  = std::numeric_limits<i32>::max();
        i32 highest = std::numeric_limits<i32>::min();
        for (usize i = 0; i < amplitudes.size(); ++i) {
            if (amplitudes[i] != 0.0) {
                lowest  = std::min(lowest, static_cast<i32>(i));
                highest = std::max(highest, static_cast<i32>(i));
            }
        }
        const f64 span = static_cast<f64>(highest - lowest);
        factor_        = (1.0 / 6.0) / (0.1 * (1.0 + 1.0 / (span + 1.0)));
    }

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z) const noexcept {
        constexpr f64 k = NormalNoise::kInputFactor;
        return (first_.value(x, y, z) + second_.value(x * k, y * k, z * k)) * factor_;
    }

private:
    // Declared first: the two stacks are built from it, in this order.
    math::LegacyRandomSource random_;
    LegacyPerlin             first_;
    LegacyPerlin             second_;
    f64                      factor_{1.0};
};

/// `NoiseBasedStateProvider.getRandomState(list, value)`.
[[nodiscard]] registry::BlockStateId pick_by_noise(const std::vector<registry::BlockStateId>& states,
                                                   f64 noise) {
    const f64 t = std::clamp((1.0 + noise) / 2.0, 0.0, 0.9999);
    return states[static_cast<usize>(t * static_cast<f64>(states.size()))];
}

class NoiseProvider final : public StateProvider {
public:
    NoiseProvider(std::shared_ptr<const LegacyNormalNoise> noise, f64 scale,
                  std::vector<registry::BlockStateId> states)
        : noise_(std::move(noise)), scale_(scale), states_(std::move(states)) {}

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&, FeatureRandom&,
                                               BlockPos at) const override {
        return pick_by_noise(states_, sample(at));
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        return states_;
    }

private:
    [[nodiscard]] f64 sample(BlockPos at) const {
        return noise_->value(static_cast<f64>(at.x) * scale_, static_cast<f64>(at.y) * scale_,
                             static_cast<f64>(at.z) * scale_);
    }

    std::shared_ptr<const LegacyNormalNoise> noise_;
    f64                                      scale_;
    std::vector<registry::BlockStateId>      states_;
};

/// Below the threshold, one of the low states; above it, one of the high
/// states with the given chance, else the default. One draw for the choice of
/// state, and one float first when above the threshold.
class NoiseThresholdProvider final : public StateProvider {
public:
    NoiseThresholdProvider(std::shared_ptr<const LegacyNormalNoise> noise, f64 scale, f32 threshold,
                           f32 high_chance, registry::BlockStateId fallback,
                           std::vector<registry::BlockStateId> low,
                           std::vector<registry::BlockStateId> high)
        : noise_(std::move(noise)),
          scale_(scale),
          threshold_(threshold),
          high_chance_(high_chance),
          default_(fallback),
          low_(std::move(low)),
          high_(std::move(high)) {}

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&, FeatureRandom& random,
                                               BlockPos at) const override {
        const f64 noise = noise_->value(static_cast<f64>(at.x) * scale_,
                                        static_cast<f64>(at.y) * scale_,
                                        static_cast<f64>(at.z) * scale_);
        if (noise < static_cast<f64>(threshold_)) {
            return low_[static_cast<usize>(random.next_int(static_cast<i32>(low_.size())))];
        }
        const f32 roll = random.next_float();
        if (roll < high_chance_) {
            return high_[static_cast<usize>(random.next_int(static_cast<i32>(high_.size())))];
        }
        return default_;
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        std::vector<registry::BlockStateId> out{default_};
        out.insert(out.end(), low_.begin(), low_.end());
        out.insert(out.end(), high_.begin(), high_.end());
        return out;
    }

private:
    std::shared_ptr<const LegacyNormalNoise> noise_;
    f64                                      scale_;
    f32                                      threshold_;
    f32                                      high_chance_;
    registry::BlockStateId                   default_;
    std::vector<registry::BlockStateId>      low_;
    std::vector<registry::BlockStateId>      high_;
};

/// A slow noise decides how many of the states are in play here (the
/// "variety"), and which ones — each chosen at a far-offset sample of the slow
/// noise; the fast noise then picks among those. No draws at all.
class DualNoiseProvider final : public StateProvider {
public:
    DualNoiseProvider(std::shared_ptr<const LegacyNormalNoise> noise, f64 scale,
                      std::shared_ptr<const LegacyNormalNoise> slow, f64 slow_scale,
                      i32 variety_min, i32 variety_max, std::vector<registry::BlockStateId> states)
        : noise_(std::move(noise)),
          scale_(scale),
          slow_(std::move(slow)),
          slow_scale_(slow_scale),
          variety_min_(variety_min),
          variety_max_(variety_max),
          states_(std::move(states)) {}

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&, FeatureRandom&,
                                               BlockPos at) const override {
        const f64 slow = slow_value(at);
        // `Mth.clampedMap(slow, -1, 1, min, max + 1)`, truncated.
        const f64 t     = (slow - -1.0) / (1.0 - -1.0);
        const f64 lo    = static_cast<f64>(variety_min_);
        const f64 hi    = static_cast<f64>(variety_max_ + 1);
        const f64 count = t < 0.0 ? lo : t > 1.0 ? hi : lo + t * (hi - lo);
        const auto n    = static_cast<i32>(count);
        std::vector<registry::BlockStateId> chosen;
        chosen.reserve(static_cast<usize>(std::max(n, 0)));
        for (i32 j = 0; j < n; ++j) {
            chosen.push_back(pick_by_noise(states_, slow_value(at.offset(j * 54545, 0, j * 34234))));
        }
        if (chosen.empty()) {
            // `list.get(...)` on an empty list throws in the game; the variety
            // in vanilla's files never reaches zero.
            return states_.front();
        }
        const f64 fast = noise_->value(static_cast<f64>(at.x) * scale_,
                                       static_cast<f64>(at.y) * scale_,
                                       static_cast<f64>(at.z) * scale_);
        return pick_by_noise(chosen, fast);
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        return states_;
    }

private:
    [[nodiscard]] f64 slow_value(BlockPos at) const {
        return slow_->value(static_cast<f64>(at.x) * slow_scale_, static_cast<f64>(at.y) * slow_scale_,
                            static_cast<f64>(at.z) * slow_scale_);
    }

    std::shared_ptr<const LegacyNormalNoise> noise_;
    f64                                      scale_;
    std::shared_ptr<const LegacyNormalNoise> slow_;
    f64                                      slow_scale_;
    i32                                      variety_min_;
    i32                                      variety_max_;
    std::vector<registry::BlockStateId>      states_;
};

[[nodiscard]] std::expected<std::shared_ptr<const LegacyNormalNoise>, FeatureError> read_noise(
    Json params, i64 seed) {
    i64 first = 0;
    simdjson::dom::array amplitudes;
    if (params.at_key("firstOctave").get(first) != simdjson::SUCCESS ||
        params.at_key("amplitudes").get(amplitudes) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    std::vector<f64> values;
    for (auto value : amplitudes) {
        f64 number = 0.0;
        if (value.get(number) != simdjson::SUCCESS) {
            i64 whole = 0;
            if (value.get(whole) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            number = static_cast<f64>(whole);
        }
        values.push_back(number);
    }
    if (values.empty()) {
        return std::unexpected(FeatureError::Malformed);
    }
    return std::make_shared<const LegacyNormalNoise>(seed, static_cast<i32>(first), values);
}

[[nodiscard]] std::expected<std::vector<registry::BlockStateId>, FeatureError> read_states(
    Json node, std::string_view key, const registry::BlockRegistry& blocks) {
    simdjson::dom::array list;
    if (node.at_key(key).get(list) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    std::vector<registry::BlockStateId> out;
    for (auto entry : list) {
        auto state = parse_block_state(entry, blocks);
        if (!state) return std::unexpected(state.error());
        out.push_back(*state);
    }
    if (out.empty()) {
        return std::unexpected(FeatureError::Malformed);
    }
    return out;
}

[[nodiscard]] f64 number(Json node, std::string_view key, f64 fallback) {
    f64 value = fallback;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return value;
    }
    i64 whole = 0;
    if (node.at_key(key).get(whole) == simdjson::SUCCESS) {
        return static_cast<f64>(whole);
    }
    return fallback;
}

}  // namespace

std::optional<std::expected<StateProviderRef, FeatureError>> parse_noise_state_provider(
    std::string_view kind, Json node, const registry::BlockRegistry& blocks) {
    if (kind != "noise_provider" && kind != "noise_threshold_provider" &&
        kind != "dual_noise_provider") {
        return std::nullopt;
    }
    i64 seed = 0;
    auto params = node.at_key("noise");
    if (node.at_key("seed").get(seed) != simdjson::SUCCESS || params.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto noise = read_noise(params.value(), seed);
    if (!noise) return std::unexpected(noise.error());
    const f64 scale = number(node, "scale", 1.0);

    if (kind == "noise_provider") {
        auto states = read_states(node, "states", blocks);
        if (!states) return std::unexpected(states.error());
        return std::static_pointer_cast<const StateProvider>(
            std::make_shared<const NoiseProvider>(*noise, scale, std::move(*states)));
    }
    if (kind == "noise_threshold_provider") {
        auto fallback = node.at_key("default_state");
        if (fallback.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
        auto state = parse_block_state(fallback.value(), blocks);
        if (!state) return std::unexpected(state.error());
        auto low  = read_states(node, "low_states", blocks);
        auto high = read_states(node, "high_states", blocks);
        if (!low) return std::unexpected(low.error());
        if (!high) return std::unexpected(high.error());
        return std::static_pointer_cast<const StateProvider>(std::make_shared<const NoiseThresholdProvider>(
            *noise, scale, static_cast<f32>(number(node, "threshold", 0.0)),
            static_cast<f32>(number(node, "high_chance", 0.0)), *state, std::move(*low),
            std::move(*high)));
    }

    auto slow_params = node.at_key("slow_noise");
    auto variety     = node.at_key("variety");
    i64  low         = 0;
    i64  high        = 0;
    if (slow_params.error() != simdjson::SUCCESS || variety.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    // `variety` is an inclusive range, written either as [min, max] or as an
    // object; the vanilla files use the list.
    simdjson::dom::array pair;
    if (variety.get(pair) == simdjson::SUCCESS) {
        usize index = 0;
        for (auto value : pair) {
            i64 v = 0;
            if (value.get(v) != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
            (index++ == 0 ? low : high) = v;
        }
    } else if (variety.at_key("min_inclusive").get(low) != simdjson::SUCCESS ||
               variety.at_key("max_inclusive").get(high) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto slow = read_noise(slow_params.value(), seed);
    if (!slow) return std::unexpected(slow.error());
    auto states = read_states(node, "states", blocks);
    if (!states) return std::unexpected(states.error());
    return std::static_pointer_cast<const StateProvider>(std::make_shared<const DualNoiseProvider>(
        *noise, scale, *slow, number(node, "slow_scale", 1.0), static_cast<i32>(low),
        static_cast<i32>(high), std::move(*states)));
}

}  // namespace ov::worldgen
