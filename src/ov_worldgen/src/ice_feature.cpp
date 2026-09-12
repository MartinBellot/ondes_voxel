#define OV_LOG_CATEGORY "worldgen"

// `iceberg` and `blue_ice` — the frozen oceans. `ice_spike` is in
// surface_feature.cpp.
//
// ── worldgen-3 ── The iceberg is a stack of horizontal slices: above the sea a
// round or elliptical body that narrows towards its top, below it a steeper
// root, then a smoothing pass, then (usually) a hollow carved through it. Every
// open choice here — the order of the three nested loops, which draws are made
// even when their answer cannot matter, the loop bound that draws again on
// every iteration — is measured against a probe world of the real game rather
// than argued: docs/provenance/features.md, « Les icebergs ».

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::worldgen {

namespace {

/// `Mth.ceil` on a float.
[[nodiscard]] i32 ceil_f(f32 value) noexcept {
    const auto whole = static_cast<i32>(value);
    return value > static_cast<f32>(whole) ? whole + 1 : whole;
}

struct IceBlocks {
    const registry::BlockRegistry* blocks{nullptr};
    registry::BlockId              packed_ice{0};
    registry::BlockId              blue_ice{0};
    registry::BlockId              ice{0};
    registry::BlockId              snow_block{0};
    registry::BlockId              snow{0};
    registry::BlockId              water{0};

    [[nodiscard]] registry::BlockId at(const FeatureLevel& level, BlockPos p) const {
        return blocks->block_of(level.block_at(p.x, p.y, p.z));
    }
    [[nodiscard]] bool is_air(registry::BlockId block) const { return blocks->is_air(block); }
    /// Packed ice, snow block or blue ice: what the smoothing and the carving
    /// consider part of a berg.
    [[nodiscard]] bool is_berg(registry::BlockId block) const {
        return block == packed_ice || block == snow_block || block == blue_ice;
    }
    void set(FeatureLevel& level, BlockPos p, registry::BlockId block) const {
        (void)level.set_block(p.x, p.y, p.z, blocks->default_state(block));
    }
    void set_state(FeatureLevel& level, BlockPos p, registry::BlockStateId state) const {
        (void)level.set_block(p.x, p.y, p.z, state);
    }
};

[[nodiscard]] std::expected<IceBlocks, FeatureError> ice_blocks(const registry::BlockRegistry& blocks) {
    IceBlocks out;
    out.blocks = &blocks;
    const std::pair<std::string_view, registry::BlockId*> wanted[] = {
        {"minecraft:packed_ice", &out.packed_ice}, {"minecraft:blue_ice", &out.blue_ice},
        {"minecraft:ice", &out.ice},               {"minecraft:snow_block", &out.snow_block},
        {"minecraft:snow", &out.snow},             {"minecraft:water", &out.water},
    };
    for (const auto& [name, slot] : wanted) {
        auto block = named_block(blocks, name);
        if (!block) {
            return std::unexpected(block.error());
        }
        *slot = *block;
    }
    return out;
}

// ── iceberg ─────────────────────────────────────────────────────────────────

class IcebergFeature final : public Feature {
public:
    IcebergFeature(IceBlocks ice, registry::BlockStateId main) : ice_(ice), main_(main) {}

    [[nodiscard]] std::string_view type_name() const override { return "iceberg"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        // The berg floats: whatever height the pipeline gave, it sits on the
        // sea.
        const BlockPos origin{at.x, level.sea_level(), at.z};

        Shape shape;
        shape.snow_on_top = random.next_double() > 0.7;
        shape.angle       = random.next_double() * 2.0 * std::numbers::pi;
        shape.ellipse_a   = 11 - random.next_int(5);
        shape.ellipse_c   = 3 + random.next_int(3);
        shape.elliptical  = random.next_double() > 0.7;
        i32 above = shape.elliptical ? random.next_int(6) + 6 : random.next_int(15) + 3;
        if (!shape.elliptical && random.next_double() > 0.9) {
            above += random.next_int(19) + 7;
        }
        const i32 below = std::min(above + random.next_int(11), 18);
        const i32 width = std::min(above + random.next_int(7) - random.next_int(5), 11);
        const i32 reach = shape.elliptical ? shape.ellipse_a : 11;

        // The body above the water.
        for (i32 x = -reach; x < reach; ++x) {
            for (i32 z = -reach; z < reach; ++z) {
                for (i32 y = 0; y < above; ++y) {
                    const i32 radius = shape.elliptical ? radius_ellipse(y, above, width)
                                                        : radius_round(random, y, above, width);
                    if (shape.elliptical || x < radius) {
                        berg_block(level, random, origin, above, {x, y, z}, radius, reach, shape);
                    }
                }
            }
        }

        smooth(level, origin, reach, above, shape);

        // The root under it.
        for (i32 x = -reach; x < reach; ++x) {
            for (i32 z = -reach; z < reach; ++z) {
                for (i32 y = -1; y > -below; --y) {
                    const i32 slice =
                        shape.elliptical
                            ? ceil_f(static_cast<f32>(reach) *
                                     (1.0F - static_cast<f32>(std::pow(static_cast<f64>(y), 2.0)) /
                                                 (static_cast<f32>(below) * 8.0F)))
                            : reach;
                    const i32 radius = radius_steep(random, -y, below, width);
                    if (x < radius) {
                        berg_block(level, random, origin, below, {x, y, z}, radius, slice, shape);
                    }
                }
            }
        }

        const bool cut =
            shape.elliptical ? random.next_double() > 0.1 : random.next_double() > 0.7;
        if (cut) {
            cut_out(random, level, width, above, origin, shape);
        }
        return true;
    }

private:
    struct Shape {
        bool snow_on_top{false};
        f64  angle{0.0};
        i32  ellipse_a{0};
        i32  ellipse_c{0};
        bool elliptical{false};
    };

    [[nodiscard]] static i32 radius_round(FeatureRandom& random, i32 y, i32 height, i32 width) {
        const f32 f = 3.5F - random.next_float();
        f32 g = (1.0F - static_cast<f32>(std::pow(static_cast<f64>(y), 2.0)) /
                            (static_cast<f32>(height) * f)) *
                static_cast<f32>(width);
        if (height > 15 + random.next_int(5)) {
            const i32 j = y < 3 + random.next_int(6) ? y / 2 : y;
            g = (1.0F - static_cast<f32>(j) / (static_cast<f32>(height) * f * 0.4F)) *
                static_cast<f32>(width);
        }
        return ceil_f(g / 2.0F);
    }

    [[nodiscard]] static i32 radius_ellipse(i32 y, i32 height, i32 width) {
        const f32 g = (1.0F - static_cast<f32>(std::pow(static_cast<f64>(y), 2.0)) /
                                  (static_cast<f32>(height) * 1.0F)) *
                      static_cast<f32>(width);
        return ceil_f(g / 2.0F);
    }

    [[nodiscard]] static i32 radius_steep(FeatureRandom& random, i32 y, i32 height, i32 width) {
        const f32 f = 1.0F + random.next_float() / 2.0F;
        const f32 g =
            (1.0F - static_cast<f32>(y) / (static_cast<f32>(height) * f)) * static_cast<f32>(width);
        return ceil_f(g / 2.0F);
    }

    [[nodiscard]] static f64 ellipse_distance(i32 x, i32 z, BlockPos centre, i32 a, i32 c,
                                              f64 angle) {
        const f64 dx = static_cast<f64>(x - centre.x);
        const f64 dz = static_cast<f64>(z - centre.z);
        return std::pow((dx * std::cos(angle) - dz * std::sin(angle)) / static_cast<f64>(a), 2.0) +
               std::pow((dx * std::sin(angle) + dz * std::cos(angle)) / static_cast<f64>(c), 2.0) -
               1.0;
    }

    [[nodiscard]] static f64 circle_distance(i32 x, i32 z, i32 radius, FeatureRandom& random) {
        const f32 f =
            10.0F * std::clamp(random.next_float(), 0.2F, 0.8F) / static_cast<f32>(radius);
        return static_cast<f64>(f) + std::pow(static_cast<f64>(x), 2.0) +
               std::pow(static_cast<f64>(z), 2.0) - std::pow(static_cast<f64>(radius), 2.0);
    }

    /// The ellipse's short axis, pinched in over the top three layers.
    [[nodiscard]] static i32 ellipse_c_at(i32 y, i32 height, i32 c) {
        if (y > 0 && height - y <= 3) {
            return c - (4 - (height - y));
        }
        return c;
    }

    void berg_block(FeatureLevel& level, FeatureRandom& random, BlockPos origin, i32 height,
                    BlockPos local, i32 radius, i32 reach, const Shape& shape) const {
        const f64 distance =
            shape.elliptical
                ? ellipse_distance(local.x, local.z, {0, 0, 0}, reach,
                                   ellipse_c_at(local.y, height, shape.ellipse_c), shape.angle)
                : circle_distance(local.x, local.z, radius, random);
        if (distance >= 0.0) {
            return;
        }
        const BlockPos pos = origin.offset(local.x, local.y, local.z);
        const f64      rim = shape.elliptical ? -0.5 : static_cast<f64>(-6 - random.next_int(3));
        if (distance > rim && random.next_double() > 0.9) {
            return;
        }
        set_berg_block(level, random, pos, height - local.y, height, shape);
    }

    void set_berg_block(FeatureLevel& level, FeatureRandom& random, BlockPos pos, i32 remaining,
                        i32 height, const Shape& shape) const {
        const auto here = ice_.at(level, pos);
        if (!ice_.is_air(here) && here != ice_.snow_block && here != ice_.ice && here != ice_.water) {
            return;
        }
        const bool keep  = !shape.elliptical || random.next_double() > 0.05;
        const i32  share = shape.elliptical ? 3 : 2;
        if (shape.snow_on_top && here != ice_.water &&
            static_cast<f64>(remaining) <=
                static_cast<f64>(random.next_int(std::max(1, height / share))) +
                    static_cast<f64>(height) * 0.6 &&
            keep) {
            ice_.set(level, pos, ice_.snow_block);
        } else {
            ice_.set_state(level, pos, main_);
        }
    }

    /// Knock off what hangs over air, and what three open sides leave
    /// standing alone.
    void smooth(FeatureLevel& level, BlockPos origin, i32 reach, i32 height,
                const Shape& shape) const {
        const i32 half = shape.elliptical ? shape.ellipse_a : reach / 2;
        for (i32 x = -half; x <= half; ++x) {
            for (i32 z = -half; z <= half; ++z) {
                for (i32 y = 0; y <= height; ++y) {
                    const BlockPos pos   = origin.offset(x, y, z);
                    const auto     block = ice_.at(level, pos);
                    if (!ice_.is_berg(block) && block != ice_.snow) {
                        continue;
                    }
                    if (ice_.is_air(ice_.at(level, pos.below()))) {
                        ice_.set_state(level, pos, registry::kAirState);
                        ice_.set_state(level, pos.above(), registry::kAirState);
                        continue;
                    }
                    if (!ice_.is_berg(block)) {
                        continue;
                    }
                    i32 open = 0;
                    for (const BlockPos side : {pos.offset(-1, 0, 0), pos.offset(1, 0, 0),
                                                pos.offset(0, 0, -1), pos.offset(0, 0, 1)}) {
                        if (!ice_.is_berg(ice_.at(level, side))) {
                            ++open;
                        }
                    }
                    if (open >= 3) {
                        ice_.set_state(level, pos, registry::kAirState);
                    }
                }
            }
        }
    }

    void cut_out(FeatureRandom& random, FeatureLevel& level, i32 width, i32 height,
                 BlockPos origin, const Shape& shape) const {
        const i32 sign_x = random.next_boolean() ? -1 : 1;
        const i32 sign_z = random.next_boolean() ? -1 : 1;
        i32       dx     = random.next_int(std::max(width / 2 - 2, 1));
        if (random.next_boolean()) {
            dx = width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
        }
        i32 dz = random.next_int(std::max(width / 2 - 2, 1));
        if (random.next_boolean()) {
            dz = width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
        }
        if (shape.elliptical) {
            dx = random.next_int(std::max(shape.ellipse_a - 5, 1));
            dz = dx;
        }
        const BlockPos centre{sign_x * dx, 0, sign_z * dz};
        const f64      angle = shape.elliptical ? shape.angle + std::numbers::pi / 2.0
                                                : random.next_double() * 2.0 * std::numbers::pi;
        for (i32 y = 0; y < height - 3; ++y) {
            const i32 radius = radius_round(random, y, height, width);
            carve(radius, y, origin, level, false, angle, centre, shape);
        }
        // The bound draws on every test, as a loop condition does.
        for (i32 y = -1; y > -height + random.next_int(5); --y) {
            const i32 radius = radius_steep(random, -y, height, width);
            carve(radius, y, origin, level, true, angle, centre, shape);
        }
    }

    void carve(i32 radius, i32 y, BlockPos origin, FeatureLevel& level, bool under_water,
               f64 angle, BlockPos centre, const Shape& shape) const {
        const i32 a = radius + 1 + shape.ellipse_a / 3;
        const i32 c = std::min(radius - 3, 3) + shape.ellipse_c / 2 - 1;
        for (i32 x = -a; x < a; ++x) {
            for (i32 z = -a; z < a; ++z) {
                if (ellipse_distance(x, z, centre, a, c, angle) >= 0.0) {
                    continue;
                }
                const BlockPos pos = origin.offset(x, y, z);
                if (!ice_.is_berg(ice_.at(level, pos))) {
                    continue;
                }
                if (under_water) {
                    ice_.set(level, pos, ice_.water);
                } else {
                    ice_.set_state(level, pos, registry::kAirState);
                    if (ice_.at(level, pos.above()) == ice_.snow) {
                        ice_.set_state(level, pos.above(), registry::kAirState);
                    }
                }
            }
        }
    }

    IceBlocks              ice_;
    registry::BlockStateId main_;
};

// ── blue_ice ────────────────────────────────────────────────────────────────

/// A clump of blue ice grown out of the side of an iceberg's root: a seed
/// under the sea touching packed ice, then two hundred tries at a block next
/// to blue ice already placed.
class BlueIceFeature final : public Feature {
public:
    explicit BlueIceFeature(IceBlocks ice) : ice_(ice) {}

    [[nodiscard]] std::string_view type_name() const override { return "blue_ice"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        if (origin.y > level.sea_level() - 1) {
            return false;
        }
        if (ice_.at(level, origin) != ice_.water && ice_.at(level, origin.below()) != ice_.water) {
            return false;
        }
        bool touches = false;
        for (const BlockPos side : {origin.above(), origin.offset(0, 0, -1), origin.offset(0, 0, 1),
                                    origin.offset(-1, 0, 0), origin.offset(1, 0, 0)}) {
            if (ice_.at(level, side) == ice_.packed_ice) {
                touches = true;
                break;
            }
        }
        if (!touches) {
            return false;
        }
        ice_.set(level, origin, ice_.blue_ice);
        for (i32 attempt = 0; attempt < 200; ++attempt) {
            const i32 dy     = random.next_int(5) - random.next_int(6);
            i32       spread = 3;
            if (dy < 2) {
                spread += dy / 2;
            }
            if (spread < 1) {
                continue;
            }
            const i32      dx  = random.next_int(spread) - random.next_int(spread);
            const i32      dz  = random.next_int(spread) - random.next_int(spread);
            const BlockPos pos = origin.offset(dx, dy, dz);
            const auto     here = ice_.at(level, pos);
            if (!ice_.is_air(here) && here != ice_.water && here != ice_.packed_ice &&
                here != ice_.ice) {
                continue;
            }
            for (const BlockPos side :
                 {pos.below(), pos.above(), pos.offset(0, 0, -1), pos.offset(0, 0, 1),
                  pos.offset(-1, 0, 0), pos.offset(1, 0, 0)}) {
                if (ice_.at(level, side) == ice_.blue_ice) {
                    ice_.set(level, pos, ice_.blue_ice);
                    break;
                }
            }
        }
        return true;
    }

private:
    IceBlocks ice_;
};

}  // namespace

ClaimedFeature parse_ice_feature(std::string_view kind, Json config,
                                 const registry::BlockRegistry& blocks, const BlockTags&) {
    if (kind != "iceberg" && kind != "blue_ice") {
        return std::nullopt;
    }
    auto ice = ice_blocks(blocks);
    if (!ice) {
        return std::unexpected(ice.error());
    }
    if (kind == "blue_ice") {
        return std::static_pointer_cast<const Feature>(std::make_shared<const BlueIceFeature>(*ice));
    }
    auto state_field = config.at_key("state");
    if (state_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto state = parse_block_state(state_field.value(), blocks);
    if (!state) {
        return std::unexpected(state.error());
    }
    return std::static_pointer_cast<const Feature>(std::make_shared<const IcebergFeature>(*ice, *state));
}

}  // namespace ov::worldgen
