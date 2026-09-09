#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/density.hpp"

#include "ov/base/log.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <unordered_map>

namespace ov::worldgen {

namespace {

using Json = simdjson::dom::element;

[[nodiscard]] constexpr f64 clamp(f64 value, f64 low, f64 high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

// ── The nodes ───────────────────────────────────────────────────────────────

class Constant final : public DensityFunction {
public:
    explicit Constant(f64 value) : value_(value) {}
    [[nodiscard]] f64 compute(const FunctionContext&) const override { return value_; }
    [[nodiscard]] f64 min_value() const override { return value_; }
    [[nodiscard]] f64 max_value() const override { return value_; }

private:
    f64 value_;
};

enum class Binary : u8 { Add, Mul, Min, Max };

class BinaryOp final : public DensityFunction {
public:
    BinaryOp(Binary op, DensityRef a, DensityRef b)
        : op_(op), a_(std::move(a)), b_(std::move(b)) {
        // The bounds of a product depend on the signs of both operands, so all
        // four corners are considered rather than the two that look obvious.
        const f64 a0 = a_->min_value();
        const f64 a1 = a_->max_value();
        const f64 b0 = b_->min_value();
        const f64 b1 = b_->max_value();
        switch (op_) {
            case Binary::Add:
                min_ = a0 + b0;
                max_ = a1 + b1;
                break;
            case Binary::Mul: {
                const std::array<f64, 4> corners{a0 * b0, a0 * b1, a1 * b0, a1 * b1};
                min_ = *std::ranges::min_element(corners);
                max_ = *std::ranges::max_element(corners);
                break;
            }
            case Binary::Min:
                min_ = std::min(a0, b0);
                max_ = std::min(a1, b1);
                break;
            case Binary::Max:
                min_ = std::max(a0, b0);
                max_ = std::max(a1, b1);
                break;
        }
    }

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const f64 a = a_->compute(at);
        switch (op_) {
            case Binary::Add:
                return a + b_->compute(at);
            case Binary::Mul:
                // Short-circuited exactly as the game does: a zero left operand
                // means the right is never evaluated. That is not only an
                // optimisation — it is why a multiply by zero can guard an
                // expensive or undefined subtree.
                return a == 0.0 ? 0.0 : a * b_->compute(at);
            case Binary::Min:
                return a < b_->min_value() ? a : std::min(a, b_->compute(at));
            case Binary::Max:
                return a > b_->max_value() ? a : std::max(a, b_->compute(at));
        }
        return 0.0;
    }

    [[nodiscard]] f64 min_value() const override { return min_; }
    [[nodiscard]] f64 max_value() const override { return max_; }

private:
    Binary     op_;
    DensityRef a_;
    DensityRef b_;
    f64        min_{0.0};
    f64        max_{0.0};
};

enum class Unary : u8 { Abs, Square, Cube, HalfNegative, QuarterNegative, Squeeze };

[[nodiscard]] f64 apply_unary(Unary op, f64 v) noexcept {
    switch (op) {
        case Unary::Abs:
            return std::abs(v);
        case Unary::Square:
            return v * v;
        case Unary::Cube:
            return v * v * v;
        case Unary::HalfNegative:
            return v > 0.0 ? v : v * 0.5;
        case Unary::QuarterNegative:
            return v > 0.0 ? v : v * 0.25;
        case Unary::Squeeze: {
            // A soft clamp into [-1, 1] that keeps the middle nearly linear.
            const f64 c = clamp(v, -1.0, 1.0);
            return c / 2.0 - c * c * c / 24.0;
        }
    }
    return v;
}

class UnaryOp final : public DensityFunction {
public:
    UnaryOp(Unary op, DensityRef inner) : op_(op), inner_(std::move(inner)) {
        const f64 lo = inner_->min_value();
        const f64 hi = inner_->max_value();
        // The transforms are monotonic except abs and square, which fold about
        // zero — so a range that straddles it has a minimum of zero rather
        // than of either end.
        const f64 a = apply_unary(op_, lo);
        const f64 b = apply_unary(op_, hi);
        min_        = std::min(a, b);
        max_        = std::max(a, b);
        if ((op_ == Unary::Abs || op_ == Unary::Square) && lo < 0.0 && hi > 0.0) {
            min_ = 0.0;
        }
    }

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        return apply_unary(op_, inner_->compute(at));
    }
    [[nodiscard]] f64 min_value() const override { return min_; }
    [[nodiscard]] f64 max_value() const override { return max_; }

private:
    Unary      op_;
    DensityRef inner_;
    f64        min_{0.0};
    f64        max_{0.0};
};

class Clamp final : public DensityFunction {
public:
    Clamp(DensityRef inner, f64 low, f64 high)
        : inner_(std::move(inner)), low_(low), high_(high) {}
    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        return clamp(inner_->compute(at), low_, high_);
    }
    [[nodiscard]] f64 min_value() const override { return low_; }
    [[nodiscard]] f64 max_value() const override { return high_; }

private:
    DensityRef inner_;
    f64        low_;
    f64        high_;
};

/// A sample of one named noise, at a scale.
class NoiseNode final : public DensityFunction {
public:
    NoiseNode(std::shared_ptr<const NormalNoise> noise, f64 xz_scale, f64 y_scale)
        : noise_(std::move(noise)), xz_scale_(xz_scale), y_scale_(y_scale) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        return noise_->value(static_cast<f64>(at.x) * xz_scale_,
                             static_cast<f64>(at.y) * y_scale_,
                             static_cast<f64>(at.z) * xz_scale_);
    }
    [[nodiscard]] f64 min_value() const override { return -noise_->max_value(); }
    [[nodiscard]] f64 max_value() const override { return noise_->max_value(); }

private:
    std::shared_ptr<const NormalNoise> noise_;
    f64                                xz_scale_;
    f64                                y_scale_;
};

/// The same, but the sample point is displaced by three other functions.
///
/// This is how the climate noises are decorrelated from the grid: the offsets
/// are themselves noises, so the sample point wanders.
class ShiftedNoise final : public DensityFunction {
public:
    ShiftedNoise(std::shared_ptr<const NormalNoise> noise, DensityRef sx, DensityRef sy,
                 DensityRef sz, f64 xz_scale, f64 y_scale)
        : noise_(std::move(noise)),
          shift_x_(std::move(sx)),
          shift_y_(std::move(sy)),
          shift_z_(std::move(sz)),
          xz_scale_(xz_scale),
          y_scale_(y_scale) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        return noise_->value(static_cast<f64>(at.x) * xz_scale_ + shift_x_->compute(at),
                             static_cast<f64>(at.y) * y_scale_ + shift_y_->compute(at),
                             static_cast<f64>(at.z) * xz_scale_ + shift_z_->compute(at));
    }
    [[nodiscard]] f64 min_value() const override { return -noise_->max_value(); }
    [[nodiscard]] f64 max_value() const override { return noise_->max_value(); }

private:
    std::shared_ptr<const NormalNoise> noise_;
    DensityRef                         shift_x_;
    DensityRef                         shift_y_;
    DensityRef                         shift_z_;
    f64                                xz_scale_;
    f64                                y_scale_;
};

/// `shift_a` and `shift_b`: a noise sampled on two of the three axes, with the
/// third fixed at zero, scaled by four. They are the offsets ShiftedNoise uses.
class ShiftAxis final : public DensityFunction {
public:
    ShiftAxis(std::shared_ptr<const NormalNoise> noise, bool swap_z_for_y)
        : noise_(std::move(noise)), swap_(swap_z_for_y) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const f64 x = static_cast<f64>(at.x) * 0.25;
        const f64 z = static_cast<f64>(at.z) * 0.25;
        // shift_a samples (x, 0, z); shift_b samples (z, x, 0). Neither reads
        // the position's own y — shift_b puts *x* in the y slot — and that
        // rotation is what stops the two offsets from moving the sample along
        // the same diagonal.
        const f64 value = swap_ ? noise_->value(z, x, 0.0) : noise_->value(x, 0.0, z);
        return value * 4.0;
    }
    [[nodiscard]] f64 min_value() const override { return -noise_->max_value() * 4.0; }
    [[nodiscard]] f64 max_value() const override { return noise_->max_value() * 4.0; }

private:
    std::shared_ptr<const NormalNoise> noise_;
    bool                               swap_;
};

/// A ramp in y between two heights, clamped outside them.
class YClampedGradient final : public DensityFunction {
public:
    YClampedGradient(f64 from_y, f64 to_y, f64 from_value, f64 to_value)
        : from_y_(from_y), to_y_(to_y), from_value_(from_value), to_value_(to_value) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const f64 t = clamp((static_cast<f64>(at.y) - from_y_) / (to_y_ - from_y_), 0.0, 1.0);
        return from_value_ + t * (to_value_ - from_value_);
    }
    [[nodiscard]] f64 min_value() const override { return std::min(from_value_, to_value_); }
    [[nodiscard]] f64 max_value() const override { return std::max(from_value_, to_value_); }

private:
    f64 from_y_;
    f64 to_y_;
    f64 from_value_;
    f64 to_value_;
};

/// Pick one of two branches by where a third function falls.
class RangeChoice final : public DensityFunction {
public:
    RangeChoice(DensityRef input, f64 low, f64 high, DensityRef in, DensityRef out)
        : input_(std::move(input)),
          low_(low),
          high_(high),
          in_(std::move(in)),
          out_(std::move(out)) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const f64 value = input_->compute(at);
        return (value >= low_ && value < high_) ? in_->compute(at) : out_->compute(at);
    }
    [[nodiscard]] f64 min_value() const override {
        return std::min(in_->min_value(), out_->min_value());
    }
    [[nodiscard]] f64 max_value() const override {
        return std::max(in_->max_value(), out_->max_value());
    }

private:
    DensityRef input_;
    f64        low_;
    f64        high_;
    DensityRef in_;
    DensityRef out_;
};

/// Quantise x and z to multiples of four and evaluate at y = 0.
///
/// Vanilla caches these per quarter-block column, and the cache is not
/// transparent: it is *filled* from the column's origin, so every position
/// inside the cell gets the origin's value. Treating it as an identity gives a
/// smoothly varying field where the game has a stepped one, and every biome
/// boundary moves.
class FlatCache final : public DensityFunction {
public:
    explicit FlatCache(DensityRef inner) : inner_(std::move(inner)) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const FunctionContext quantised{(at.x >> 2) << 2, 0, (at.z >> 2) << 2};
        return inner_->compute(quantised);
    }
    [[nodiscard]] f64 min_value() const override { return inner_->min_value(); }
    [[nodiscard]] f64 max_value() const override { return inner_->max_value(); }

private:
    DensityRef inner_;
};

/// The caches that really are transparent point by point.
///
/// `cache_2d` caches by column and its argument does not depend on y;
/// `cache_once` and `cache_all_in_cell` memoise. All three return the same
/// number a direct evaluation would, so for a point-by-point evaluator they
/// are the identity — the difference is speed, and speed is measured later.
class Passthrough final : public DensityFunction {
public:
    explicit Passthrough(DensityRef inner) : inner_(std::move(inner)) {}
    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        return inner_->compute(at);
    }
    [[nodiscard]] f64 min_value() const override { return inner_->min_value(); }
    [[nodiscard]] f64 max_value() const override { return inner_->max_value(); }

private:
    DensityRef inner_;
};

/// A piecewise cubic curve whose coordinate is another function, and whose
/// values may themselves be splines.
///
/// Floats, not doubles, and deliberately: the game evaluates splines in single
/// precision, and the terrain's large-scale shape comes out of these. Widening
/// them would be a different world.
class Spline final : public DensityFunction {
public:
    struct Point {
        f32        location{0.0F};
        DensityRef value;
        f32        derivative{0.0F};
    };

    Spline(DensityRef coordinate, std::vector<Point> points)
        : coordinate_(std::move(coordinate)), points_(std::move(points)) {
        for (const auto& point : points_) {
            min_ = std::min(min_, point.value->min_value());
            max_ = std::max(max_, point.value->max_value());
        }
        // The ends extend linearly along their derivative, so the reachable
        // range is wider than the points themselves. Widened generously rather
        // than computed: a loose bound only costs time.
        const f64 slack = 1000.0;
        min_ -= slack;
        max_ += slack;
    }

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const auto  coordinate = static_cast<f32>(coordinate_->compute(at));
        const usize count      = points_.size();
        if (count == 0) {
            return 0.0;
        }

        // The interval whose start is the last location at or below the
        // coordinate.
        usize index = 0;
        while (index < count && points_[index].location <= coordinate) {
            ++index;
        }
        // Outside on either side: continue along the end point's derivative.
        if (index == 0) {
            const auto& first = points_.front();
            return static_cast<f64>(static_cast<f32>(first.value->compute(at)) +
                                    first.derivative * (coordinate - first.location));
        }
        if (index == count) {
            const auto& last = points_.back();
            return static_cast<f64>(static_cast<f32>(last.value->compute(at)) +
                                    last.derivative * (coordinate - last.location));
        }

        const auto& lower = points_[index - 1];
        const auto& upper = points_[index];
        const f32   span  = upper.location - lower.location;
        const f32   t     = (coordinate - lower.location) / span;
        const auto  a     = static_cast<f32>(lower.value->compute(at));
        const auto  b     = static_cast<f32>(upper.value->compute(at));
        // Hermite, written the way the game writes it: a linear interpolation
        // plus a correction that carries the two derivatives.
        const f32 n = lower.derivative * span - (b - a);
        const f32 o = -upper.derivative * span + (b - a);
        return static_cast<f64>(a + t * (b - a) + t * (1.0F - t) * (n + t * (o - n)));
    }

    [[nodiscard]] f64 min_value() const override { return min_; }
    [[nodiscard]] f64 max_value() const override { return max_; }

private:
    DensityRef         coordinate_;
    std::vector<Point> points_;
    f64                min_{1.0e9};
    f64                max_{-1.0e9};
};

/// A noise sampled at a scale that the input itself chooses.
///
/// The scale is not continuous: the input falls into one of a handful of bands
/// and each band picks a fixed rarity. That is what makes the cave tunnels come
/// in a few distinct widths rather than in a smooth range.
class WeirdScaledSampler final : public DensityFunction {
public:
    WeirdScaledSampler(DensityRef input, std::shared_ptr<const NormalNoise> noise, bool two_d)
        : input_(std::move(input)), noise_(std::move(noise)), two_d_(two_d) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        // OV_NO_CAVE_NOISE answers, by measurement, whether the cave terms are
        // what pulls the surface down. They enter final_density through a min,
        // so a large value takes them out of the picture entirely.
        static const bool disabled = std::getenv("OV_NO_CAVE_NOISE") != nullptr;
        if (disabled) {
            return 64.0;
        }
        const f64 rarity = map(input_->compute(at));
        return rarity * std::abs(noise_->value(static_cast<f64>(at.x) / rarity,
                                               static_cast<f64>(at.y) / rarity,
                                               static_cast<f64>(at.z) / rarity));
    }
    [[nodiscard]] f64 min_value() const override { return 0.0; }
    [[nodiscard]] f64 max_value() const override {
        return (two_d_ ? 3.0 : 2.0) * noise_->max_value();
    }

private:
    [[nodiscard]] f64 map(f64 value) const noexcept {
        if (two_d_) {
            if (value < -0.75) return 0.5;
            if (value < -0.5) return 0.75;
            if (value < 0.5) return 1.0;
            if (value < 0.75) return 2.0;
            return 3.0;
        }
        if (value < -0.5) return 0.75;
        if (value < 0.0) return 1.0;
        if (value < 0.5) return 1.5;
        return 2.0;
    }

    DensityRef                         input_;
    std::shared_ptr<const NormalNoise> noise_;
    bool                               two_d_;
};

/// Sampled on the coarse cell grid and interpolated between.
///
/// This is the one wrapper that genuinely changes the value. The terrain is not
/// evaluated per block: it is evaluated at the corners of cells four blocks
/// wide and eight tall, and every block inside a cell is a trilinear blend of
/// its eight corners. That is why 1.18 terrain has the smoothness it does, and
/// why evaluating this function directly gives a *different*, rougher world.
///
/// The interpolation order is y, then x, then z — vanilla's. With floating
/// point that is not the same as any other order, and terrain parity is exactly
/// the kind of claim that order decides.
///
/// The corner values are memoised. Vanilla fills them a slice at a time as it
/// walks a chunk; a cache keyed by the corner reaches the same numbers with the
/// same count of evaluations, and does not require the caller to walk in any
/// particular order. Not thread-safe, and generation is single-threaded — when
/// it stops being, this becomes per-chunk state rather than a shared map.
class Interpolated final : public DensityFunction {
public:
    Interpolated(DensityRef inner, i32 cell_width, i32 cell_height, i32 min_y)
        : inner_(std::move(inner)),
          cell_width_(cell_width),
          cell_height_(cell_height),
          min_y_(min_y) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        const i32 cell_x = floor_div(at.x, cell_width_);
        const i32 cell_z = floor_div(at.z, cell_width_);
        const i32 cell_y = floor_div(at.y - min_y_, cell_height_);

        const f64 tx = static_cast<f64>(at.x - cell_x * cell_width_) /
                       static_cast<f64>(cell_width_);
        const f64 tz = static_cast<f64>(at.z - cell_z * cell_width_) /
                       static_cast<f64>(cell_width_);
        const f64 ty = static_cast<f64>(at.y - min_y_ - cell_y * cell_height_) /
                       static_cast<f64>(cell_height_);

        const f64 c000 = corner(cell_x, cell_y, cell_z);
        const f64 c001 = corner(cell_x, cell_y, cell_z + 1);
        const f64 c010 = corner(cell_x, cell_y + 1, cell_z);
        const f64 c011 = corner(cell_x, cell_y + 1, cell_z + 1);
        const f64 c100 = corner(cell_x + 1, cell_y, cell_z);
        const f64 c101 = corner(cell_x + 1, cell_y, cell_z + 1);
        const f64 c110 = corner(cell_x + 1, cell_y + 1, cell_z);
        const f64 c111 = corner(cell_x + 1, cell_y + 1, cell_z + 1);

        // y first, then x, then z.
        const f64 xz00 = lerp(ty, c000, c010);
        const f64 xz01 = lerp(ty, c001, c011);
        const f64 xz10 = lerp(ty, c100, c110);
        const f64 xz11 = lerp(ty, c101, c111);
        const f64 z0   = lerp(tx, xz00, xz10);
        const f64 z1   = lerp(tx, xz01, xz11);
        return lerp(tz, z0, z1);
    }

    [[nodiscard]] f64 min_value() const override { return inner_->min_value(); }
    [[nodiscard]] f64 max_value() const override { return inner_->max_value(); }

private:
    [[nodiscard]] static constexpr i32 floor_div(i32 value, i32 divisor) noexcept {
        const i32 quotient = value / divisor;
        return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
    }
    [[nodiscard]] static constexpr f64 lerp(f64 t, f64 a, f64 b) noexcept {
        return a + t * (b - a);
    }

    [[nodiscard]] f64 corner(i32 cell_x, i32 cell_y, i32 cell_z) const {
        // Three fields that do not overlap. The first version shifted
        // thirty-two-bit values by 40 and 16 and XORed them, so x's low bits
        // sat on top of z's high ones and y's on top of both: different cells
        // collided and handed each other their values. The terrain came out
        // two blocks low on average with a tail to eight, which looks exactly
        // like a noise being slightly wrong.
        //
        // The ranges are known: a cell coordinate is a block coordinate over
        // four, so the world border fits in twenty-four bits, and there are
        // forty-eight vertical cells.
        const u64 key = (static_cast<u64>(static_cast<u32>(cell_x) & 0xFFFFFFU) << 40) |
                        (static_cast<u64>(static_cast<u32>(cell_z) & 0xFFFFFFU) << 16) |
                        static_cast<u64>(static_cast<u32>(cell_y) & 0xFFFFU);
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }
        const FunctionContext at{cell_x * cell_width_, min_y_ + cell_y * cell_height_,
                                 cell_z * cell_width_};
        const f64             value = inner_->compute(at);
        // Bounded, so a long generation run does not grow without limit. A
        // chunk needs about 1225 corners per interpolated node; clearing at a
        // hundred thousand keeps several chunks' worth and costs a refill.
        if (cache_.size() > 100000) {
            cache_.clear();
        }
        cache_.emplace(key, value);
        return value;
    }

    DensityRef inner_;
    i32        cell_width_{4};
    i32        cell_height_{8};
    i32        min_y_{-64};

    mutable std::unordered_map<u64, f64> cache_;
};

/// The old terrain noise, as a node.
class BlendedNoiseNode final : public DensityFunction {
public:
    explicit BlendedNoiseNode(std::shared_ptr<const BlendedNoise> noise)
        : noise_(std::move(noise)),
          gain_(gain_from_environment()),
          shift_(shift_from_environment()) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        return noise_->value(at.x + shift_, at.y, at.z + shift_) * gain_;
    }
    [[nodiscard]] f64 min_value() const override { return -noise_->max_value() * gain_; }
    [[nodiscard]] f64 max_value() const override { return noise_->max_value() * gain_; }

private:
    /// OV_BASE3D_GAIN scales this noise and nothing else.
    ///
    /// It exists to settle one question by measurement. This noise is the only
    /// term of the terrain that the biome comparison does not already check,
    /// and `quarter_negative` above it has a kink at zero: the slope is four
    /// times steeper on the positive side, so a given amount of noise raises
    /// the surface four times as far as it lowers it. The mean height of the
    /// surface is therefore proportional to this noise's *amplitude*, not
    /// only to its mean — and a surface uniformly a block or two low is what
    /// an amplitude that is too small looks like. Sweeping the gain and
    /// watching the surface histogram says whether that is what is happening.
    [[nodiscard]] static f64 gain_from_environment() {
        const char* text = std::getenv("OV_BASE3D_GAIN");
        if (text == nullptr) {
            return 1.0;
        }
        const f64 value = std::strtod(text, nullptr);
        return value == 0.0 ? 1.0 : value;
    }

    /// OV_BASE3D_SHIFT samples the same noise somewhere else entirely.
    ///
    /// It is the control every correlation in this comparison needs. Asking
    /// whether a candidate field matches the game's surface is only a question
    /// if a field that certainly does *not* match scores lower, and two fields
    /// built the same way share so much structure that a naive correlation
    /// answers yes for both. Displacing the sample by ten thousand blocks keeps
    /// every statistic of the field and destroys the alignment, so whatever it
    /// scores is the artefact floor and only the excess above it is evidence.
    [[nodiscard]] static i32 shift_from_environment() {
        const char* text = std::getenv("OV_BASE3D_SHIFT");
        return text == nullptr ? 0 : static_cast<i32>(std::strtol(text, nullptr, 10));
    }

    std::shared_ptr<const BlendedNoise> noise_;
    f64                                 gain_{1.0};
    i32                                 shift_{0};
};

}  // namespace

DensityFunction::~DensityFunction() = default;

std::string_view to_string(DensityError error) noexcept {
    switch (error) {
        case DensityError::Missing:
            return "a density function file is missing";
        case DensityError::Malformed:
            return "a density function file is not the shape one has";
        case DensityError::Unsupported:
            return "a density function node type is not implemented";
        case DensityError::Cycle:
            return "a density function refers to itself";
    }
    return "unknown";
}

// ── Loading ─────────────────────────────────────────────────────────────────

struct NoiseRouter::Impl {
    std::filesystem::path root;
    /// Kept alive because simdjson's elements point into these.
    std::vector<std::unique_ptr<simdjson::dom::parser>>   parsers;
    std::vector<std::unique_ptr<simdjson::padded_string>> documents;

    math::XoroshiroPositionalFactory factory{0, 0};
    /// The blended noise gets a generator forked from the world seed's own
    /// stream, not from the positional factory. Kept here because the fork
    /// consumes state and must happen exactly once.
    math::XoroshiroRandomSource blended_random{0};
    /// The blended noise this router built, kept so a harness can read the
    /// selector stack on its own. Nothing in generation reads it; it is the
    /// handle `NoiseRouter::blended_noise()` hands out.
    std::shared_ptr<const BlendedNoise> blended;

    std::unordered_map<std::string, std::shared_ptr<const NormalNoise>> noises;
    std::unordered_map<std::string, DensityRef>                         functions;
    std::unordered_map<std::string, DensityRef>                         router;
    /// Names currently being resolved, so a cycle is reported rather than
    /// overflowing the stack.
    std::set<std::string> resolving;
    /// Router entries that could not be built, and why. Reported rather than
    /// silently absent.
    std::map<std::string, DensityError> unavailable;

    i32 sea_level{63};
    i32 min_y{-64};
    i32 height{384};
    i32 cell_width{4};
    i32 cell_height{8};

    [[nodiscard]] std::expected<Json, DensityError> read(const std::filesystem::path& path);
    [[nodiscard]] std::expected<std::shared_ptr<const NormalNoise>, DensityError> noise(
        std::string_view name);
    [[nodiscard]] std::expected<DensityRef, DensityError> parse(Json node);
    /// The cell grid, read from the settings before the router is parsed.
    /// `interpolated` needs it and nothing else does.
    [[nodiscard]] std::expected<DensityRef, DensityError> reference(std::string_view name);
    [[nodiscard]] std::expected<Spline::Point, DensityError> spline_point(Json node);
    [[nodiscard]] std::expected<DensityRef, DensityError> spline(Json node);
};

namespace {

/// "minecraft:overworld/continents" -> "overworld/continents".
[[nodiscard]] std::string strip_namespace(std::string_view name) {
    const auto colon = name.find(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

}  // namespace

std::expected<Json, DensityError> NoiseRouter::Impl::read(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        OV_LOG_ERROR("worldgen: {} is missing", path.string());
        return std::unexpected(DensityError::Missing);
    }
    auto text = simdjson::padded_string::load(path.string());
    if (text.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    documents.push_back(std::make_unique<simdjson::padded_string>(std::move(text.value())));
    parsers.push_back(std::make_unique<simdjson::dom::parser>());
    auto parsed = parsers.back()->parse(*documents.back());
    if (parsed.error() != simdjson::SUCCESS) {
        OV_LOG_ERROR("worldgen: {} is not valid JSON", path.string());
        return std::unexpected(DensityError::Malformed);
    }
    return parsed.value();
}

std::expected<std::shared_ptr<const NormalNoise>, DensityError> NoiseRouter::Impl::noise(
    std::string_view name) {
    const std::string key(name);
    if (const auto found = noises.find(key); found != noises.end()) {
        return found->second;
    }

    auto document = read(root / "worldgen" / "noise" / (strip_namespace(name) + ".json"));
    if (!document) {
        return std::unexpected(document.error());
    }
    i64 first_octave = 0;
    if (document->at_key("firstOctave").get(first_octave) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    simdjson::dom::array raw;
    if (document->at_key("amplitudes").get(raw) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    std::vector<f64> amplitudes;
    for (auto value : raw) {
        f64 amplitude = 0.0;
        if (value.get(amplitude) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        amplitudes.push_back(amplitude);
    }

    // Seeded by name, from the world seed's positional factory. This is why
    // adding a noise to a datapack does not disturb the others.
    auto source = factory.from_hash_of(name);
    auto built  = std::make_shared<const NormalNoise>(
        NormalNoise::create(source, static_cast<i32>(first_octave), amplitudes));
    noises.emplace(key, built);
    return built;
}

std::expected<DensityRef, DensityError> NoiseRouter::Impl::reference(std::string_view name) {
    const std::string key(name);
    if (const auto found = functions.find(key); found != functions.end()) {
        return found->second;
    }
    // OV_MUTE_FUNCTION replaces named functions by a large constant.
    //
    // It is a measuring instrument, and it exists because "the cave terms" is
    // not a localisation: `final_density` reaches them through several `min`
    // nodes and a large constant takes exactly one of them out of the picture
    // without touching the rest. `OV_NO_CAVE_NOISE` mutes every
    // `weird_scaled_sampler` at once and so cannot say *which*. A
    // comma-separated list of full names, and a name that is never reached
    // silently does nothing — which is why the list is echoed at load.
    if (const char* muted = std::getenv("OV_MUTE_FUNCTION"); muted != nullptr) {
        const std::string_view list{muted};
        for (usize start = 0; start <= list.size();) {
            const usize comma = list.find(',', start);
            const auto  piece = list.substr(start, comma - start);
            if (piece == name) {
                OV_LOG_WARN("worldgen: {} is muted to +64 by OV_MUTE_FUNCTION", key);
                auto constant = std::make_shared<const Constant>(64.0);
                functions.emplace(key, constant);
                return DensityRef{constant};
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    if (resolving.contains(key)) {
        OV_LOG_ERROR("worldgen: {} refers to itself", key);
        return std::unexpected(DensityError::Cycle);
    }
    resolving.insert(key);

    auto document = read(root / "worldgen" / "density_function" / (strip_namespace(name) + ".json"));
    if (!document) {
        resolving.erase(key);
        return std::unexpected(document.error());
    }
    auto parsed = parse(*document);
    resolving.erase(key);
    if (!parsed) {
        return parsed;
    }
    functions.emplace(key, *parsed);
    return *parsed;
}

std::expected<Spline::Point, DensityError> NoiseRouter::Impl::spline_point(Json node) {
    Spline::Point point;
    f64           location = 0.0;
    if (node.at_key("location").get(location) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    point.location = static_cast<f32>(location);

    f64 derivative = 0.0;
    if (node.at_key("derivative").get(derivative) == simdjson::SUCCESS) {
        point.derivative = static_cast<f32>(derivative);
    }

    auto value = node.at_key("value");
    if (value.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    // A point's value is either a number or another spline. That nesting is
    // how the terrain gets its shape: continentalness picks a curve over
    // erosion, which picks a curve over ridges.
    f64 number = 0.0;
    if (value.get(number) == simdjson::SUCCESS) {
        point.value = std::make_shared<Constant>(number);
        return point;
    }
    auto nested = spline(value.value());
    if (!nested) {
        return std::unexpected(nested.error());
    }
    point.value = *nested;
    return point;
}

std::expected<DensityRef, DensityError> NoiseRouter::Impl::spline(Json node) {
    std::string_view coordinate_name;
    auto             coordinate_field = node.at_key("coordinate");
    if (coordinate_field.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    DensityRef coordinate;
    if (coordinate_field.get(coordinate_name) == simdjson::SUCCESS) {
        auto resolved = reference(coordinate_name);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        coordinate = *resolved;
    } else {
        auto inline_function = parse(coordinate_field.value());
        if (!inline_function) {
            return inline_function;
        }
        coordinate = *inline_function;
    }

    simdjson::dom::array raw_points;
    if (node.at_key("points").get(raw_points) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    std::vector<Spline::Point> points;
    for (auto entry : raw_points) {
        auto point = spline_point(entry);
        if (!point) {
            return std::unexpected(point.error());
        }
        points.push_back(std::move(*point));
    }
    return std::static_pointer_cast<const DensityFunction>(
        std::make_shared<const Spline>(std::move(coordinate), std::move(points)));
}

std::expected<DensityRef, DensityError> NoiseRouter::Impl::parse(Json node) {

    // A bare number is a constant, and a bare string is a reference. Both are
    // common enough in the data that treating them as errors would reject the
    // vanilla files outright.
    f64 number = 0.0;
    if (node.get(number) == simdjson::SUCCESS) {
        return std::static_pointer_cast<const DensityFunction>(
            std::make_shared<const Constant>(number));
    }
    std::string_view name;
    if (node.get(name) == simdjson::SUCCESS) {
        return reference(name);
    }

    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    const auto argument = [&](const char* key) -> std::expected<DensityRef, DensityError> {
        auto field = node.at_key(key);
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        return parse(field.value());
    };
    const auto number_at = [&](const char* key, f64 fallback) {
        f64  value = fallback;
        auto field = node.at_key(key);
        if (field.error() == simdjson::SUCCESS) {
            (void)field.get(value);
        }
        return value;
    };
    const auto wrap = [](auto pointer) {
        return std::static_pointer_cast<const DensityFunction>(std::move(pointer));
    };

    if (kind == "add" || kind == "mul" || kind == "min" || kind == "max") {
        auto a = argument("argument1");
        auto b = argument("argument2");
        if (!a) return a;
        if (!b) return b;
        const Binary op = kind == "add"   ? Binary::Add
                          : kind == "mul" ? Binary::Mul
                          : kind == "min" ? Binary::Min
                                          : Binary::Max;
        return wrap(std::make_shared<const BinaryOp>(op, *a, *b));
    }
    if (kind == "abs" || kind == "square" || kind == "cube" || kind == "half_negative" ||
        kind == "quarter_negative" || kind == "squeeze") {
        auto inner = argument("argument");
        if (!inner) return inner;
        const Unary op = kind == "abs"              ? Unary::Abs
                         : kind == "square"         ? Unary::Square
                         : kind == "cube"           ? Unary::Cube
                         : kind == "half_negative"  ? Unary::HalfNegative
                         : kind == "quarter_negative" ? Unary::QuarterNegative
                                                      : Unary::Squeeze;
        return wrap(std::make_shared<const UnaryOp>(op, *inner));
    }
    if (kind == "clamp") {
        auto inner = argument("input");
        if (!inner) return inner;
        return wrap(std::make_shared<const Clamp>(*inner, number_at("min", -1.0),
                                                  number_at("max", 1.0)));
    }
    if (kind == "noise") {
        std::string_view noise_name;
        if (node.at_key("noise").get(noise_name) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        auto built = noise(noise_name);
        if (!built) return std::unexpected(built.error());
        return wrap(std::make_shared<const NoiseNode>(*built, number_at("xz_scale", 1.0),
                                                      number_at("y_scale", 1.0)));
    }
    if (kind == "shifted_noise") {
        std::string_view noise_name;
        if (node.at_key("noise").get(noise_name) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        auto built = noise(noise_name);
        auto sx    = argument("shift_x");
        auto sy    = argument("shift_y");
        auto sz    = argument("shift_z");
        if (!built) return std::unexpected(built.error());
        if (!sx) return sx;
        if (!sy) return sy;
        if (!sz) return sz;
        return wrap(std::make_shared<const ShiftedNoise>(*built, *sx, *sy, *sz,
                                                         number_at("xz_scale", 1.0),
                                                         number_at("y_scale", 1.0)));
    }
    if (kind == "shift_a" || kind == "shift_b") {
        std::string_view noise_name;
        if (node.at_key("argument").get(noise_name) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        auto built = noise(noise_name);
        if (!built) return std::unexpected(built.error());
        return wrap(std::make_shared<const ShiftAxis>(*built, kind == "shift_b"));
    }
    if (kind == "y_clamped_gradient") {
        return wrap(std::make_shared<const YClampedGradient>(
            number_at("from_y", 0.0), number_at("to_y", 1.0), number_at("from_value", 0.0),
            number_at("to_value", 1.0)));
    }
    if (kind == "range_choice") {
        auto input = argument("input");
        auto in    = argument("when_in_range");
        auto out   = argument("when_out_of_range");
        if (!input) return input;
        if (!in) return in;
        if (!out) return out;
        return wrap(std::make_shared<const RangeChoice>(*input, number_at("min_inclusive", 0.0),
                                                        number_at("max_exclusive", 1.0), *in,
                                                        *out));
    }
    if (kind == "flat_cache") {
        auto inner = argument("argument");
        if (!inner) return inner;
        return wrap(std::make_shared<const FlatCache>(*inner));
    }
    if (kind == "interpolated") {
        auto inner = argument("argument");
        if (!inner) return inner;
        // OV_NO_INTERPOLATION exists to answer one question by measurement
        // rather than by reasoning: is the cell-grid interpolation helping or
        // hurting? Turning it off and comparing against the same reference
        // world settles it in one run.
        if (std::getenv("OV_NO_INTERPOLATION") != nullptr) {
            return wrap(std::make_shared<const Passthrough>(*inner));
        }
        return wrap(std::make_shared<const Interpolated>(*inner, this->cell_width,
                                                        this->cell_height, this->min_y));
    }
    if (kind == "cache_2d" || kind == "cache_once" || kind == "cache_all_in_cell") {
        // These three really are transparent: two memoise and one caches by
        // column over an argument that does not depend on y. Same number, so
        // for a point-by-point evaluator they are the identity and the
        // difference is speed.
        auto inner = argument("argument");
        if (!inner) return inner;
        return wrap(std::make_shared<const Passthrough>(*inner));
    }
    if (kind == "old_blended_noise") {
        // Its own generator, forked from the world seed's — not the positional
        // factory the named noises use. The fork consumes two draws, so its
        // position in the sequence is part of what the seed decides.
        auto forked = std::make_shared<BlendedNoise>(BlendedNoise::create(
            blended_random, number_at("xz_scale", 1.0), number_at("y_scale", 1.0),
            number_at("xz_factor", 80.0), number_at("y_factor", 160.0),
            number_at("smear_scale_multiplier", 8.0)));
        blended = forked;
        return wrap(std::make_shared<const BlendedNoiseNode>(std::move(forked)));
    }
    if (kind == "weird_scaled_sampler") {
        std::string_view noise_name;
        if (node.at_key("noise").get(noise_name) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        std::string_view mapper;
        if (node.at_key("rarity_value_mapper").get(mapper) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        auto built = noise(noise_name);
        auto input = argument("input");
        if (!built) return std::unexpected(built.error());
        if (!input) return input;
        return wrap(std::make_shared<const WeirdScaledSampler>(*input, *built, mapper == "type_2"));
    }
    if (kind == "spline") {
        auto field = node.at_key("spline");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        return spline(field.value());
    }
    if (kind == "constant") {
        return wrap(std::make_shared<const Constant>(number_at("argument", 0.0)));
    }
    if (kind == "blend_alpha") {
        // No old world to blend with, so the blend is entirely the new one.
        return wrap(std::make_shared<const Constant>(1.0));
    }
    if (kind == "blend_offset") {
        return wrap(std::make_shared<const Constant>(0.0));
    }
    if (kind == "blend_density") {
        return argument("argument");
    }

    OV_LOG_ERROR("worldgen: density function type '{}' is not implemented", kind);
    return std::unexpected(DensityError::Unsupported);
}

NoiseRouter::NoiseRouter() : impl_(std::make_unique<Impl>()) {}
NoiseRouter::NoiseRouter(NoiseRouter&&) noexcept            = default;
NoiseRouter& NoiseRouter::operator=(NoiseRouter&&) noexcept = default;
NoiseRouter::~NoiseRouter()                                 = default;

std::expected<NoiseRouter, DensityError> NoiseRouter::load(const std::filesystem::path& data_root,
                                                           std::string_view settings, i64 seed) {
    NoiseRouter router;
    Impl&       impl = *router.impl_;
    impl.root        = data_root;

    // Every noise the world uses is seeded from one factory forked from the
    // world seed. That fork is the whole of "the same seed gives the same
    // world".
    math::XoroshiroRandomSource source{seed};
    impl.factory        = source.fork_positional();
    impl.blended_random = source.fork();

    auto document = impl.read(data_root / "worldgen" / "noise_settings" /
                              (std::string(settings) + ".json"));
    if (!document) {
        return std::unexpected(document.error());
    }

    i64 value = 0;
    if (document->at_key("sea_level").get(value) == simdjson::SUCCESS) {
        impl.sea_level = static_cast<i32>(value);
    }
    auto shape = document->at_key("noise");
    if (shape.error() == simdjson::SUCCESS) {
        if (shape.at_key("min_y").get(value) == simdjson::SUCCESS) {
            impl.min_y = static_cast<i32>(value);
        }
        if (shape.at_key("height").get(value) == simdjson::SUCCESS) {
            impl.height = static_cast<i32>(value);
        }
        // The cell the terrain is sampled on: four blocks across per unit of
        // size_horizontal, four per unit of size_vertical.
        if (shape.at_key("size_horizontal").get(value) == simdjson::SUCCESS) {
            impl.cell_width = static_cast<i32>(value) * 4;
        }
        if (shape.at_key("size_vertical").get(value) == simdjson::SUCCESS) {
            impl.cell_height = static_cast<i32>(value) * 4;
        }
    }

    simdjson::dom::object entries;
    if (document->at_key("noise_router").get(entries) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    // One entry failing does not sink the rest, but it is *named*. The climate
    // functions and the terrain density are independent subtrees, and a
    // renderer that can choose biomes while the terrain shape is still missing
    // a node type is more useful than one that refuses to start — as long as
    // nobody can mistake the missing entry for a working one. Asking for it
    // returns nothing at all, never zero.
    usize failures = 0;
    for (auto [key, node] : entries) {
        auto parsed = impl.parse(node);
        if (!parsed) {
            impl.unavailable.emplace(std::string(key), parsed.error());
            ++failures;
            continue;
        }
        impl.router.emplace(std::string(key), *parsed);
    }
    if (failures != 0) {
        std::string names;
        for (const auto& [name, error] : impl.unavailable) {
            names += (names.empty() ? "" : ", ") + name;
        }
        OV_LOG_WARN("worldgen: {} router entries could not be built: {}", failures, names);
    }

    OV_LOG_INFO("worldgen: {} router entries, {} functions, {} noises, seed {}",
                impl.router.size(), impl.functions.size(), impl.noises.size(), seed);
    return router;
}

const DensityFunction* NoiseRouter::entry(std::string_view name) const {
    const auto found = impl_->router.find(std::string(name));
    return found == impl_->router.end() ? nullptr : found->second.get();
}

std::vector<std::pair<std::string, DensityError>> NoiseRouter::unavailable() const {
    return {impl_->unavailable.begin(), impl_->unavailable.end()};
}

const DensityFunction* NoiseRouter::function(std::string_view name) const {
    const auto found = impl_->functions.find(std::string(name));
    return found == impl_->functions.end() ? nullptr : found->second.get();
}

const BlendedNoise* NoiseRouter::blended_noise() const noexcept {
    return impl_->blended.get();
}

i32 NoiseRouter::sea_level() const noexcept {
    return impl_->sea_level;
}
i32 NoiseRouter::min_y() const noexcept {
    return impl_->min_y;
}
i32 NoiseRouter::height() const noexcept {
    return impl_->height;
}
i32 NoiseRouter::cell_width() const noexcept {
    return impl_->cell_width;
}
i32 NoiseRouter::cell_height() const noexcept {
    return impl_->cell_height;
}

}  // namespace ov::worldgen
