#define OV_LOG_CATEGORY "worldgen"

// The dripstone caves: `pointed_dripstone`, `dripstone_cluster`,
// `large_dripstone`.
//
// Three features over one small vocabulary — a column scanned up and down from
// a point, a dripstone block where stone was, a pointed dripstone column built
// from base to tip — and each one draws heavily. Measured against a probe
// world, see docs/provenance/features.md, "Les features de l'Overworld".

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/carver.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace ov::worldgen {

namespace {

// ── Float providers ─────────────────────────────────────────────────────────

/// `FloatProvider`: a constant, a uniform range, or a clamped normal.
class FloatProvider {
public:
    enum class Kind : u8 { Constant, Uniform, ClampedNormal };

    [[nodiscard]] f32 sample(FeatureRandom& random) const {
        switch (kind_) {
            case Kind::Constant:
                return a_;
            case Kind::Uniform: {
                // `Mth.randomBetween`: one float, scaled.
                const f32 roll = random.next_float();
                return roll * (b_ - a_) + a_;
            }
            case Kind::ClampedNormal: {
                const auto gaussian = static_cast<f32>(random.next_gaussian());
                return std::clamp(a_ + gaussian * b_, c_, d_);
            }
        }
        return a_;
    }

    static std::expected<FloatProvider, FeatureError> parse(Json node) {
        FloatProvider out;
        f64           number = 0.0;
        if (node.get(number) == simdjson::SUCCESS) {
            out.kind_ = Kind::Constant;
            out.a_    = static_cast<f32>(number);
            return out;
        }
        std::string_view type;
        auto             value = node.at_key("value");
        if (node.at_key("type").get(type) != simdjson::SUCCESS || value.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        const std::string kind = strip_namespace(type);
        const auto        read = [&](std::string_view key) {
            f64 v = 0.0;
            (void)value.at_key(key).get(v);
            return static_cast<f32>(v);
        };
        if (kind == "uniform") {
            out.kind_ = Kind::Uniform;
            out.a_    = read("min_inclusive");
            out.b_    = read("max_exclusive");
            return out;
        }
        if (kind == "clamped_normal") {
            out.kind_ = Kind::ClampedNormal;
            out.a_    = read("mean");
            out.b_    = read("deviation");
            out.c_    = read("min");
            out.d_    = read("max");
            return out;
        }
        if (kind == "constant") {
            out.kind_ = Kind::Constant;
            f64 v     = 0.0;
            (void)node.at_key("value").get(v);
            out.a_ = static_cast<f32>(v);
            return out;
        }
        // `trapezoid` is in the format and in no vanilla dripstone file.
        OV_LOG_ERROR("worldgen: float provider '{}' is not implemented", kind);
        return std::unexpected(FeatureError::Unsupported);
    }

private:
    Kind kind_{Kind::Constant};
    f32  a_{0.0F};
    f32  b_{0.0F};
    f32  c_{0.0F};
    f32  d_{0.0F};
};

[[nodiscard]] std::expected<FloatProvider, FeatureError> float_field(Json node,
                                                                    std::string_view key) {
    auto field = node.at_key(key);
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    return FloatProvider::parse(field.value());
}

[[nodiscard]] f64 number_field(Json node, std::string_view key, f64 fallback) {
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

/// The bounds of a uniform int provider, as numbers: `large_dripstone` clamps
/// against them.
[[nodiscard]] std::expected<std::pair<i32, i32>, FeatureError> int_bounds(Json node,
                                                                         std::string_view key) {
    auto field = node.at_key(key);
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    i64 constant = 0;
    if (field.get(constant) == simdjson::SUCCESS) {
        return std::pair<i32, i32>{static_cast<i32>(constant), static_cast<i32>(constant)};
    }
    i64 low  = 0;
    i64 high = 0;
    auto value = field.at_key("value");
    if (value.error() != simdjson::SUCCESS ||
        value.at_key("min_inclusive").get(low) != simdjson::SUCCESS ||
        value.at_key("max_inclusive").get(high) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    return std::pair<i32, i32>{static_cast<i32>(low), static_cast<i32>(high)};
}

// ── The shared vocabulary ───────────────────────────────────────────────────

constexpr std::array<std::array<i32, 3>, 6> kDirections{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
}};

constexpr std::array<std::array<i32, 2>, 4> kHorizontal{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};

struct Dripstone {
    const registry::BlockRegistry* blocks{nullptr};
    std::vector<u16>               replaceable;  ///< #dripstone_replaceable_blocks
    std::vector<u16>               base_stone;   ///< #base_stone_overworld
    registry::BlockId              dripstone_block{0};
    registry::BlockId              pointed{0};
    registry::BlockId              water{0};
    registry::BlockId              lava{0};

    [[nodiscard]] registry::BlockId at(const FeatureLevel& level, BlockPos pos) const {
        return blocks->block_of(level.block_at(pos.x, pos.y, pos.z));
    }
    [[nodiscard]] bool empty_or_water(registry::BlockId block) const {
        return blocks->is_air(block) || block == water;
    }
    [[nodiscard]] bool empty_or_water_or_lava(registry::BlockId block) const {
        return empty_or_water(block) || block == lava;
    }
    [[nodiscard]] bool base(registry::BlockId block) const {
        return block == dripstone_block || holds(replaceable, block);
    }
    [[nodiscard]] bool is_water_at(const FeatureLevel& level, BlockPos pos) const {
        const auto state = level.block_at(pos.x, pos.y, pos.z);
        const auto block = blocks->block_of(state);
        if (block == water) {
            return true;
        }
        const auto property = blocks->find_property(block, "waterlogged");
        return property && blocks->property_value(state, *property) == "true";
    }

    bool place_block_if_possible(FeatureLevel& level, BlockPos pos) const {
        if (!holds(replaceable, at(level, pos))) {
            return false;
        }
        (void)level.set_block(pos.x, pos.y, pos.z, blocks->default_state(dripstone_block));
        return true;
    }

    [[nodiscard]] registry::BlockStateId pointed_state(bool up, std::string_view thickness) const {
        auto state = blocks->default_state(pointed);
        state      = with_property_value(*blocks, state, "vertical_direction", up ? "up" : "down");
        state      = with_property_value(*blocks, state, "thickness", thickness);
        return state;
    }

    /// Base, middles, frustum, tip — only when the block behind the start is
    /// something a dripstone can hang from.
    void grow_pointed(FeatureLevel& level, BlockPos pos, bool up, i32 height, bool merge) const {
        const i32 dy = up ? 1 : -1;
        if (!base(at(level, pos.above(-dy)))) {
            return;
        }
        BlockPos   cursor = pos;
        const auto put    = [&](std::string_view thickness) {
            auto state = pointed_state(up, thickness);
            state = with_property_value(*blocks, state, "waterlogged",
                                        is_water_at(level, cursor) ? "true" : "false");
            (void)level.set_block(cursor.x, cursor.y, cursor.z, state);
            cursor = cursor.above(dy);
        };
        if (height >= 3) {
            put("base");
            for (i32 i = 0; i < height - 3; ++i) {
                put("middle");
            }
        }
        if (height >= 2) {
            put("frustum");
        }
        if (height >= 1) {
            put(merge ? "tip_merge" : "tip");
        }
    }
};

/// `Column.scan`: from a point inside the column, up and down, stopping after
/// `range - 1` steps or at the first block that is not inside. The edge is
/// kept only if it passes `edge`.
struct Column {
    std::optional<i32> floor;
    std::optional<i32> ceiling;

    [[nodiscard]] std::optional<i32> height() const {
        if (floor && ceiling) {
            return *ceiling - *floor - 1;
        }
        return std::nullopt;
    }
};

template<typename Inside, typename Edge>
[[nodiscard]] std::optional<Column> scan_column(const FeatureLevel& level, const Dripstone& d,
                                                BlockPos pos, i32 range, Inside inside, Edge edge) {
    if (!inside(d.at(level, pos))) {
        return std::nullopt;
    }
    const auto direction = [&](i32 dy) -> std::optional<i32> {
        BlockPos cursor = pos;
        for (i32 i = 1; i < range && inside(d.at(level, cursor)); ++i) {
            cursor = cursor.above(dy);
        }
        if (edge(d.at(level, cursor))) {
            return cursor.y;
        }
        return std::nullopt;
    };
    Column column;
    column.ceiling = direction(1);
    column.floor   = direction(-1);
    return column;
}

// ── pointed_dripstone ───────────────────────────────────────────────────────

class PointedDripstoneFeature final : public Feature {
public:
    PointedDripstoneFeature(Dripstone d, f32 taller, f32 directional, f32 radius2, f32 radius3)
        : d_(std::move(d)),
          taller_(taller),
          directional_(directional),
          radius2_(radius2),
          radius3_(radius3) {}

    [[nodiscard]] std::string_view type_name() const override { return "pointed_dripstone"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos pos) const override {
        const bool above = d_.base(d_.at(level, pos.above()));
        const bool below = d_.base(d_.at(level, pos.below()));
        bool       up    = false;
        if (above && below) {
            up = !random.next_boolean();
        } else if (above) {
            up = false;
        } else if (below) {
            up = true;
        } else {
            return false;
        }
        const i32 dy = up ? 1 : -1;
        patch(level, random, pos.above(-dy));
        const f32  roll  = random.next_float();
        const bool room  = d_.empty_or_water(d_.at(level, pos.above(dy)));
        const i32  count = roll < taller_ && room ? 2 : 1;
        d_.grow_pointed(level, pos, up, count, false);
        return true;
    }

private:
    void patch(FeatureLevel& level, FeatureRandom& random, BlockPos pos) const {
        d_.place_block_if_possible(level, pos);
        for (const auto& side : kHorizontal) {
            if (random.next_float() > directional_) {
                continue;
            }
            const BlockPos one = pos.offset(side[0], 0, side[1]);
            d_.place_block_if_possible(level, one);
            if (random.next_float() > radius2_) {
                continue;
            }
            const auto&    turn = kDirections[static_cast<usize>(random.next_int(6))];
            const BlockPos two  = one.offset(turn[0], turn[1], turn[2]);
            d_.place_block_if_possible(level, two);
            if (random.next_float() > radius3_) {
                continue;
            }
            const auto&    last  = kDirections[static_cast<usize>(random.next_int(6))];
            const BlockPos three = two.offset(last[0], last[1], last[2]);
            d_.place_block_if_possible(level, three);
        }
    }

    Dripstone d_;
    f32       taller_;
    f32       directional_;
    f32       radius2_;
    f32       radius3_;
};

// ── dripstone_cluster ───────────────────────────────────────────────────────

struct ClusterConfig {
    i32            search_range{12};
    IntProviderRef height;
    IntProviderRef radius;
    i32            max_height_diff{1};
    i32            height_deviation{3};
    IntProviderRef layer_thickness;
    FloatProvider  density;
    FloatProvider  wetness;
    f32            chance_at_max_distance{0.1F};
    i32            max_distance_from_edge{3};
    i32            max_distance_from_center{8};
};

/// `Mth.clampedMap` on floats.
[[nodiscard]] f32 clamped_map(f32 input, f32 in_min, f32 in_max, f32 out_min, f32 out_max) {
    const f32 delta = (input - in_min) / (in_max - in_min);
    if (delta < 0.0F) {
        return out_min;
    }
    if (delta > 1.0F) {
        return out_max;
    }
    return out_min + delta * (out_max - out_min);
}

[[nodiscard]] f64 clamped_map(f64 input, f64 in_min, f64 in_max, f64 out_min, f64 out_max) {
    const f64 delta = (input - in_min) / (in_max - in_min);
    if (delta < 0.0) {
        return out_min;
    }
    if (delta > 1.0) {
        return out_max;
    }
    return out_min + delta * (out_max - out_min);
}

class DripstoneClusterFeature final : public Feature {
public:
    DripstoneClusterFeature(Dripstone d, ClusterConfig c) : d_(std::move(d)), c_(std::move(c)) {}

    [[nodiscard]] std::string_view type_name() const override { return "dripstone_cluster"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos pos) const override {
        if (!d_.empty_or_water(d_.at(level, pos))) {
            return false;
        }
        const i32 height   = c_.height->sample(random);
        const f32 wetness  = c_.wetness.sample(random);
        const f32 density  = c_.density.sample(random);
        const i32 radius_x = c_.radius->sample(random);
        const i32 radius_z = c_.radius->sample(random);
        for (i32 dx = -radius_x; dx <= radius_x; ++dx) {
            for (i32 dz = -radius_z; dz <= radius_z; ++dz) {
                const f64 chance = chance_of_column(radius_x, radius_z, dx, dz);
                place_column(level, random, pos.offset(dx, 0, dz), dx, dz, wetness, chance, height,
                             density);
            }
        }
        return true;
    }

private:
    [[nodiscard]] f64 chance_of_column(i32 radius_x, i32 radius_z, i32 x, i32 z) const {
        const i32 from_edge = std::min(radius_x - std::abs(x), radius_z - std::abs(z));
        return static_cast<f64>(clamped_map(static_cast<f32>(from_edge), 0.0F,
                                            static_cast<f32>(c_.max_distance_from_edge),
                                            c_.chance_at_max_distance, 1.0F));
    }

    [[nodiscard]] i32 dripstone_height(FeatureRandom& random, i32 dx, i32 dz, f32 density,
                                       i32 height) const {
        if (random.next_float() > density) {
            return 0;
        }
        const i32 distance = std::abs(dx) + std::abs(dz);
        const auto mean    = static_cast<f32>(clamped_map(
            static_cast<f64>(distance), 0.0, static_cast<f64>(c_.max_distance_from_center),
            static_cast<f64>(height) / 2.0, 0.0));
        const auto gaussian = static_cast<f32>(random.next_gaussian());
        const f32  value    = std::clamp(mean + gaussian * static_cast<f32>(c_.height_deviation),
                                         0.0F, static_cast<f32>(height));
        return static_cast<i32>(value);
    }

    [[nodiscard]] bool can_be_next_to_water(const FeatureLevel& level, BlockPos pos) const {
        return holds(d_.base_stone, d_.at(level, pos)) || d_.is_water_at(level, pos);
    }

    [[nodiscard]] bool can_place_pool(const FeatureLevel& level, BlockPos pos) const {
        const auto here = d_.at(level, pos);
        if (here == d_.water || here == d_.dripstone_block || here == d_.pointed) {
            return false;
        }
        if (d_.is_water_at(level, pos.above())) {
            return false;
        }
        for (const auto& side : kHorizontal) {
            if (!can_be_next_to_water(level, pos.offset(side[0], 0, side[1]))) {
                return false;
            }
        }
        return can_be_next_to_water(level, pos.below());
    }

    void replace_with_blocks(FeatureLevel& level, BlockPos pos, i32 thickness, i32 dy) const {
        BlockPos cursor = pos;
        for (i32 i = 0; i < thickness; ++i) {
            if (!d_.place_block_if_possible(level, cursor)) {
                return;
            }
            cursor = cursor.above(dy);
        }
    }

    void place_column(FeatureLevel& level, FeatureRandom& random, BlockPos pos, i32 dx, i32 dz,
                      f32 wetness, f64 chance, i32 height, f32 density) const {
        const auto scanned = scan_column(
            level, d_, pos, c_.search_range,
            [&](registry::BlockId b) { return d_.empty_or_water(b); },
            [&](registry::BlockId b) { return !d_.empty_or_water(b); });
        if (!scanned) {
            return;
        }
        const auto ceiling = scanned->ceiling;
        if (!ceiling && !scanned->floor) {
            return;
        }
        const f32 wet_roll = random.next_float();
        const bool wet     = wet_roll < wetness;
        Column     column  = *scanned;
        if (wet && scanned->floor && can_place_pool(level, pos.above(*scanned->floor - pos.y))) {
            const i32 floor_y = *scanned->floor;
            column.floor      = floor_y - 1;
            (void)level.set_block(pos.x, floor_y, pos.z, d_.blocks->default_state(d_.water));
        }
        const auto floor = column.floor;

        const f64 up_roll = random.next_double();
        i32       down_height = 0;
        if (ceiling && up_roll < chance && d_.at(level, {pos.x, *ceiling, pos.z}) != d_.lava) {
            const i32 thickness = c_.layer_thickness->sample(random);
            replace_with_blocks(level, {pos.x, *ceiling, pos.z}, thickness, 1);
            const i32 room = floor ? std::min(height, *ceiling - *floor) : height;
            down_height    = dripstone_height(random, dx, dz, density, room);
        }
        const f64 down_roll = random.next_double();
        i32       up_height = 0;
        if (floor && down_roll < chance && d_.at(level, {pos.x, *floor, pos.z}) != d_.lava) {
            const i32 thickness = c_.layer_thickness->sample(random);
            replace_with_blocks(level, {pos.x, *floor, pos.z}, thickness, -1);
            if (ceiling) {
                const i32 spread = c_.max_height_diff;
                const i32 jitter = random.next_int(spread - -spread + 1) + -spread;
                up_height        = std::max(0, down_height + jitter);
            } else {
                up_height = dripstone_height(random, dx, dz, density, height);
            }
        }

        i32 stalactite = down_height;
        i32 stalagmite = up_height;
        if (ceiling && floor && *ceiling - down_height <= *floor + up_height) {
            const i32 low    = std::max(*ceiling - down_height, *floor + 1);
            const i32 high   = std::min(*floor + up_height, *ceiling - 1);
            const i32 meet   = random.next_int(high + 1 - low + 1) + low;
            stalactite       = *ceiling - meet;
            stalagmite       = meet - 1 - *floor;
        }
        const bool coin  = random.next_boolean();
        const auto total = column.height();
        const bool merge = coin && stalactite > 0 && stalagmite > 0 && total &&
                           stalactite + stalagmite == *total;
        if (ceiling) {
            d_.grow_pointed(level, {pos.x, *ceiling - 1, pos.z}, false, stalactite, merge);
        }
        if (floor) {
            d_.grow_pointed(level, {pos.x, *floor + 1, pos.z}, true, stalagmite, merge);
        }
    }

    Dripstone     d_;
    ClusterConfig c_;
};

// ── large_dripstone ─────────────────────────────────────────────────────────

struct LargeConfig {
    i32           search_range{30};
    i32           radius_min{3};
    i32           radius_max{19};
    FloatProvider height_scale;
    f32           max_radius_ratio{0.33F};
    FloatProvider stalactite_bluntness;
    FloatProvider stalagmite_bluntness;
    FloatProvider wind_speed;
    i32           min_radius_for_wind{4};
    f32           min_bluntness_for_wind{0.6F};
};

/// `DripstoneUtils.getDripstoneHeight`: the profile of a cone of rock.
[[nodiscard]] f64 cone_height(f64 radius, f64 scale, f64 height_scale, f64 bluntness) {
    if (radius < bluntness) {
        radius = bluntness;
    }
    const f64 e      = radius / scale * 0.384;
    const f64 f      = 0.75 * std::pow(e, 1.3333333333333333);
    const f64 g      = std::pow(e, 0.6666666666666666);
    const f64 h      = 0.3333333333333333 * std::log(e);
    const f64 height = std::max(height_scale * (f - g - h), 0.0);
    return height / 0.384 * scale;
}

struct Wind {
    bool enabled{false};
    i32  origin_y{0};
    f64  x{0.0};
    f64  z{0.0};

    [[nodiscard]] BlockPos offset(BlockPos pos) const {
        if (!enabled) {
            return pos;
        }
        const auto dy = static_cast<f64>(origin_y - pos.y);
        return pos.offset(static_cast<i32>(std::floor(x * dy)), 0,
                          static_cast<i32>(std::floor(z * dy)));
    }
};

class LargeDripstone {
public:
    LargeDripstone(BlockPos root, bool up, i32 radius, f64 bluntness, f64 scale)
        : root_(root), up_(up), radius_(radius), bluntness_(bluntness), scale_(scale) {}

    [[nodiscard]] i32 height_at(f32 radius) const {
        return static_cast<i32>(cone_height(static_cast<f64>(radius), static_cast<f64>(radius_),
                                            scale_, bluntness_));
    }
    [[nodiscard]] i32 height() const { return height_at(0.0F); }

    [[nodiscard]] bool suits_wind(const LargeConfig& c) const {
        return radius_ >= c.min_radius_for_wind &&
               bluntness_ >= static_cast<f64>(c.min_bluntness_for_wind);
    }

    bool settle(const FeatureLevel& level, const Dripstone& d, const Wind& wind) {
        while (radius_ > 1) {
            BlockPos  cursor = root_;
            const i32 steps  = std::min(10, height());
            for (i32 i = 0; i < steps; ++i) {
                if (d.at(level, cursor) == d.lava) {
                    return false;
                }
                if (embedded(level, d, wind.offset(cursor), radius_)) {
                    root_ = cursor;
                    return true;
                }
                cursor = cursor.above(up_ ? -1 : 1);
            }
            radius_ /= 2;
        }
        return false;
    }

    void place(FeatureLevel& level, const Dripstone& d, FeatureRandom& random,
               const Wind& wind) const {
        for (i32 i = -radius_; i <= radius_; ++i) {
            for (i32 j = -radius_; j <= radius_; ++j) {
                const f32 distance = std::sqrt(static_cast<f32>(i * i + j * j));
                if (distance > static_cast<f32>(radius_)) {
                    continue;
                }
                i32 length = height_at(distance);
                if (length <= 0) {
                    continue;
                }
                const f32 shorten = random.next_float();
                if (static_cast<f64>(shorten) < 0.2) {
                    const f32 factor = random.next_float() * (1.0F - 0.8F) + 0.8F;
                    length           = static_cast<i32>(static_cast<f32>(length) * factor);
                }
                BlockPos  cursor  = root_.offset(i, 0, j);
                bool      started = false;
                const i32 limit   = up_ ? level.height(world::HeightmapType::WorldSurfaceWG,
                                                       cursor.x, cursor.z)
                                        : std::numeric_limits<i32>::max();
                for (i32 m = 0; m < length && cursor.y < limit; ++m) {
                    const BlockPos at    = wind.offset(cursor);
                    const auto     block = d.at(level, at);
                    if (d.empty_or_water_or_lava(block)) {
                        started = true;
                        (void)level.set_block(at.x, at.y, at.z,
                                              d.blocks->default_state(d.dripstone_block));
                    } else if (started && holds(d.base_stone, block)) {
                        break;
                    }
                    cursor = cursor.above(up_ ? 1 : -1);
                }
            }
        }
    }

private:
    /// `isCircleMostlyEmbeddedInStone`: the centre and points around the
    /// circle, every six blocks of arc, all solid. The angle accumulates in a
    /// float and the trigonometry is the table.
    [[nodiscard]] static bool embedded(const FeatureLevel& level, const Dripstone& d,
                                       BlockPos pos, i32 radius) {
        if (d.empty_or_water_or_lava(d.at(level, pos))) {
            return false;
        }
        const f32 step = 6.0F / static_cast<f32>(radius);
        for (f32 angle = 0.0F; angle < 3.1415927F * 2.0F; angle += step) {
            const auto x = static_cast<i32>(mth_cos(angle) * static_cast<f32>(radius));
            const auto z = static_cast<i32>(mth_sin(angle) * static_cast<f32>(radius));
            if (d.empty_or_water_or_lava(d.at(level, pos.offset(x, 0, z)))) {
                return false;
            }
        }
        return true;
    }

    BlockPos root_;
    bool     up_;
    i32      radius_;
    f64      bluntness_;
    f64      scale_;
};

class LargeDripstoneFeature final : public Feature {
public:
    LargeDripstoneFeature(Dripstone d, LargeConfig c) : d_(std::move(d)), c_(std::move(c)) {}

    [[nodiscard]] std::string_view type_name() const override { return "large_dripstone"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos pos) const override {
        if (!d_.empty_or_water(d_.at(level, pos))) {
            return false;
        }
        const auto column = scan_column(
            level, d_, pos, c_.search_range,
            [&](registry::BlockId b) { return d_.empty_or_water(b); },
            [&](registry::BlockId b) { return d_.base(b) || b == d_.lava; });
        if (!column || !column->height() || *column->height() < 4) {
            return false;
        }
        const i32 cave   = *column->height();
        const auto ratio = static_cast<i32>(static_cast<f32>(cave) * c_.max_radius_ratio);
        const i32 upper  = std::clamp(ratio, c_.radius_min, c_.radius_max);
        const i32 radius = random.next_int(upper - c_.radius_min + 1) + c_.radius_min;

        const f32 top_blunt = c_.stalactite_bluntness.sample(random);
        const f32 top_scale = c_.height_scale.sample(random);
        LargeDripstone stalactite{{pos.x, *column->ceiling - 1, pos.z}, false, radius,
                                  static_cast<f64>(top_blunt), static_cast<f64>(top_scale)};
        const f32 bottom_blunt = c_.stalagmite_bluntness.sample(random);
        const f32 bottom_scale = c_.height_scale.sample(random);
        LargeDripstone stalagmite{{pos.x, *column->floor + 1, pos.z}, true, radius,
                                  static_cast<f64>(bottom_blunt), static_cast<f64>(bottom_scale)};

        Wind wind;
        if (stalactite.suits_wind(c_) && stalagmite.suits_wind(c_)) {
            wind.enabled    = true;
            wind.origin_y   = pos.y;
            const f32 speed = c_.wind_speed.sample(random);
            const f32 angle = random.next_float() * (3.1415927F - 0.0F) + 0.0F;
            wind.x          = static_cast<f64>(mth_cos(angle) * speed);
            wind.z          = static_cast<f64>(mth_sin(angle) * speed);
        }
        const bool top    = stalactite.settle(level, d_, wind);
        const bool bottom = stalagmite.settle(level, d_, wind);
        if (top) {
            stalactite.place(level, d_, random, wind);
        }
        if (bottom) {
            stalagmite.place(level, d_, random, wind);
        }
        return true;
    }

private:
    Dripstone   d_;
    LargeConfig c_;
};

[[nodiscard]] std::expected<Dripstone, FeatureError> dripstone_vocabulary(
    const registry::BlockRegistry& blocks, const BlockTags& tags) {
    Dripstone d;
    d.blocks = &blocks;
    auto replaceable = tag_members(blocks, tags, "minecraft:dripstone_replaceable_blocks");
    if (!replaceable) return std::unexpected(replaceable.error());
    auto base = tag_members(blocks, tags, "minecraft:base_stone_overworld");
    if (!base) return std::unexpected(base.error());
    d.replaceable = std::move(*replaceable);
    d.base_stone  = std::move(*base);
    for (const auto& [name, into] :
         std::array<std::pair<std::string_view, registry::BlockId*>, 4>{{
             {"minecraft:dripstone_block", &d.dripstone_block},
             {"minecraft:pointed_dripstone", &d.pointed},
             {"minecraft:water", &d.water},
             {"minecraft:lava", &d.lava},
         }}) {
        auto block = named_block(blocks, name);
        if (!block) return std::unexpected(block.error());
        *into = *block;
    }
    return d;
}

}  // namespace

ClaimedFeature parse_dripstone_feature(std::string_view kind, Json config,
                                       const registry::BlockRegistry& blocks,
                                       const BlockTags&               tags) {
    if (kind != "pointed_dripstone" && kind != "dripstone_cluster" && kind != "large_dripstone") {
        return std::nullopt;
    }
    auto d = dripstone_vocabulary(blocks, tags);
    if (!d) return std::unexpected(d.error());

    if (kind == "pointed_dripstone") {
        return std::static_pointer_cast<const Feature>(std::make_shared<const PointedDripstoneFeature>(
            std::move(*d), static_cast<f32>(number_field(config, "chance_of_taller_dripstone", 0.2)),
            static_cast<f32>(number_field(config, "chance_of_directional_spread", 0.7)),
            static_cast<f32>(number_field(config, "chance_of_spread_radius2", 0.5)),
            static_cast<f32>(number_field(config, "chance_of_spread_radius3", 0.5))));
    }

    if (kind == "dripstone_cluster") {
        ClusterConfig c;
        c.search_range       = static_cast<i32>(number_field(config, "floor_to_ceiling_search_range", 12));
        c.max_height_diff    = static_cast<i32>(number_field(config, "max_stalagmite_stalactite_height_diff", 1));
        c.height_deviation   = static_cast<i32>(number_field(config, "height_deviation", 3));
        c.chance_at_max_distance = static_cast<f32>(
            number_field(config, "chance_of_dripstone_column_at_max_distance_from_center", 0.1));
        c.max_distance_from_edge = static_cast<i32>(
            number_field(config, "max_distance_from_edge_affecting_chance_of_dripstone_column", 3));
        c.max_distance_from_center = static_cast<i32>(
            number_field(config, "max_distance_from_center_affecting_height_bias", 8));
        auto height    = config.at_key("height");
        auto radius    = config.at_key("radius");
        auto thickness = config.at_key("dripstone_block_layer_thickness");
        if (height.error() != simdjson::SUCCESS || radius.error() != simdjson::SUCCESS ||
            thickness.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto h = parse_int_provider(height.value());
        auto r = parse_int_provider(radius.value());
        auto t = parse_int_provider(thickness.value());
        if (!h || !r || !t) return std::unexpected(FeatureError::Malformed);
        c.height          = *h;
        c.radius          = *r;
        c.layer_thickness = *t;
        auto density = float_field(config, "density");
        auto wetness = float_field(config, "wetness");
        if (!density) return std::unexpected(density.error());
        if (!wetness) return std::unexpected(wetness.error());
        c.density = *density;
        c.wetness = *wetness;
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const DripstoneClusterFeature>(std::move(*d), std::move(c)));
    }

    LargeConfig c;
    c.search_range     = static_cast<i32>(number_field(config, "floor_to_ceiling_search_range", 30));
    c.max_radius_ratio = static_cast<f32>(number_field(config, "max_column_radius_to_cave_height_ratio", 0.33));
    c.min_radius_for_wind    = static_cast<i32>(number_field(config, "min_radius_for_wind", 4));
    c.min_bluntness_for_wind = static_cast<f32>(number_field(config, "min_bluntness_for_wind", 0.6));
    auto bounds = int_bounds(config, "column_radius");
    if (!bounds) return std::unexpected(bounds.error());
    c.radius_min = bounds->first;
    c.radius_max = bounds->second;
    auto scale = float_field(config, "height_scale");
    auto top   = float_field(config, "stalactite_bluntness");
    auto floor = float_field(config, "stalagmite_bluntness");
    auto wind  = float_field(config, "wind_speed");
    if (!scale) return std::unexpected(scale.error());
    if (!top) return std::unexpected(top.error());
    if (!floor) return std::unexpected(floor.error());
    if (!wind) return std::unexpected(wind.error());
    c.height_scale         = *scale;
    c.stalactite_bluntness = *top;
    c.stalagmite_bluntness = *floor;
    c.wind_speed           = *wind;
    return std::static_pointer_cast<const Feature>(
        std::make_shared<const LargeDripstoneFeature>(std::move(*d), std::move(c)));
}

}  // namespace ov::worldgen
