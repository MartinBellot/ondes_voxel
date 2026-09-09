#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/ore_feature.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace ov::worldgen {

bool OreTarget::accepts(registry::BlockId block) const noexcept {
    return std::ranges::binary_search(replaceable, block.value());
}

namespace {

/// Whether the air check is skipped for this block.
///
/// The order of the two early exits is what matters. At chance 0 it returns
/// true and draws nothing; at chance 1 it returns false and draws nothing. Only
/// a chance strictly between the two costs a draw — and every draw is a
/// position for the next feature in the chunk, so a spurious one moves
/// everything that comes after.
[[nodiscard]] bool skip_air_check(FeatureRandom& random, f32 chance) {
    if (chance <= 0.0F) {
        return true;
    }
    if (chance >= 1.0F) {
        return false;
    }
    return random.next_float() >= chance;
}

[[nodiscard]] bool touching_air(const FeatureContext& context, const FeatureLevel& level,
                                BlockPos at) {
    constexpr std::array<std::array<i32, 3>, 6> kSides{
        {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    for (const auto& side : kSides) {
        const auto state =
            level.block_at(at.x + side[0], at.y + side[1], at.z + side[2]);
        if (context.blocks->is_air(context.blocks->block_of(state))) {
            return true;
        }
    }
    return false;
}

/// The shared decision: may this ore replace what is here?
[[nodiscard]] bool can_place(const FeatureContext& context, const FeatureLevel& level,
                             FeatureRandom& random, const OreConfig& config,
                             const OreTarget& target, registry::BlockStateId here,
                             BlockPos at) {
    if (!target.accepts(context.blocks->block_of(here))) {
        return false;
    }
    if (skip_air_check(random, config.discard_chance_on_air_exposure)) {
        return true;
    }
    return !touching_air(context, level, at);
}

/// `ore`: a line of spheres between two scattered endpoints.
class OreFeature final : public Feature {
public:
    explicit OreFeature(OreConfig config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view type_name() const override { return "ore"; }

    bool place(const FeatureContext& context, FeatureLevel& level,
               FeatureRandom& random, BlockPos at) const override {
        // Everything the shape depends on is drawn here, before any block is
        // looked at, so that a vein which turns out to have nowhere to go has
        // still consumed exactly the same draws as one that lands in stone.
        const f32 angle     = random.next_float() * std::numbers::pi_v<f32>;
        const f32 half_size = static_cast<f32>(config_.size) / 8.0F;
        const auto slack    = static_cast<i32>(
            std::ceil((static_cast<f32>(config_.size) / 16.0F * 2.0F + 1.0F) / 2.0F));

        // sin on x, cos on z. The two are not interchangeable: swapping them
        // rotates every vein in the world by ninety degrees, which looks
        // exactly as random as the truth. Both are taken in double precision
        // over a single-precision angle, which is what widening a float
        // argument to Math.sin does.
        const f64 sine   = std::sin(static_cast<f64>(angle));
        const f64 cosine = std::cos(static_cast<f64>(angle));
        const f64 reach  = static_cast<f64>(half_size);
        const f64 x_low  = static_cast<f64>(at.x) + sine * reach;
        const f64 x_high = static_cast<f64>(at.x) - sine * reach;
        const f64 z_low  = static_cast<f64>(at.z) + cosine * reach;
        const f64 z_high = static_cast<f64>(at.z) - cosine * reach;
        const f64 y_low  = static_cast<f64>(at.y + random.next_int(3) - 2);
        const f64 y_high = static_cast<f64>(at.y + random.next_int(3) - 2);

        const auto ceil_half = static_cast<i32>(std::ceil(half_size));
        const i32  min_x     = at.x - ceil_half - slack;
        const i32  min_y     = at.y - 2 - slack;
        const i32  min_z     = at.z - ceil_half - slack;
        const i32  width     = 2 * (ceil_half + slack);
        const i32  height    = 2 * (2 + slack);

        // A vein whose whole bounding box is above the ocean floor is in open
        // air and is abandoned — but only *after* the draws above, which is
        // why this test comes here and not first.
        bool underground = false;
        for (i32 x = min_x; x <= min_x + width && !underground; ++x) {
            for (i32 z = min_z; z <= min_z + width; ++z) {
                if (min_y <= level.height(world::HeightmapType::OceanFloorWG, x, z)) {
                    underground = true;
                    break;
                }
            }
        }
        if (!underground) {
            return false;
        }

        return fill(context, level, random, x_low, x_high, z_low, z_high, y_low, y_high, min_x,
                    min_y, min_z, width, height);
    }

private:
    struct Sphere {
        f64 x{0.0};
        f64 y{0.0};
        f64 z{0.0};
        /// Negative once the sphere has been swallowed by another.
        f64 radius{0.0};
    };

    [[nodiscard]] bool fill(const FeatureContext& context, FeatureLevel& level,
                            FeatureRandom& random, f64 x_low, f64 x_high, f64 z_low,
                            f64 z_high, f64 y_low, f64 y_high, i32 min_x, i32 min_y, i32 min_z,
                            i32 width, i32 height) const {
        usize placed = 0;
        // One bit per position in the bounding box, so that two overlapping
        // spheres cannot each replace the same block — which would let the
        // second one's target rule see the first one's ore.
        std::vector<bool> occupied(static_cast<usize>(width) * static_cast<usize>(height) *
                                       static_cast<usize>(width),
                                   false);

        std::vector<Sphere> spheres(static_cast<usize>(config_.size));
        for (i32 index = 0; index < config_.size; ++index) {
            const f32 along = static_cast<f32>(index) / static_cast<f32>(config_.size);
            Sphere&   sphere = spheres[static_cast<usize>(index)];
            sphere.x         = std::lerp(x_low, x_high, static_cast<f64>(along));
            sphere.y         = std::lerp(y_low, y_high, static_cast<f64>(along));
            sphere.z         = std::lerp(z_low, z_high, static_cast<f64>(along));
            // The draw comes after the three interpolations and before the
            // radius, and it is a double where everything around it is a float.
            const f64 scale = random.next_double() * static_cast<f64>(config_.size) / 16.0;
            // sin of a float angle, deliberately: the value is computed in
            // single precision in the game and the difference is visible at the
            // edge of a sphere.
            sphere.radius =
                (static_cast<f64>(std::sin(std::numbers::pi_v<f32> * along) + 1.0F) * scale + 1.0) /
                2.0;
        }

        // A sphere contained in another is dropped rather than drawn twice.
        // Containment is decided on the distance between centres against the
        // difference of radii, both squared.
        for (usize first = 0; first + 1 < spheres.size(); ++first) {
            if (spheres[first].radius <= 0.0) {
                continue;
            }
            for (usize second = first + 1; second < spheres.size(); ++second) {
                if (spheres[second].radius <= 0.0) {
                    continue;
                }
                const f64 dx = spheres[first].x - spheres[second].x;
                const f64 dy = spheres[first].y - spheres[second].y;
                const f64 dz = spheres[first].z - spheres[second].z;
                const f64 dr = spheres[first].radius - spheres[second].radius;
                if (dr * dr > dx * dx + dy * dy + dz * dz) {
                    if (dr > 0.0) {
                        spheres[second].radius = -1.0;
                    } else {
                        spheres[first].radius = -1.0;
                    }
                }
            }
        }

        for (const Sphere& sphere : spheres) {
            if (sphere.radius < 0.0) {
                continue;
            }
            const i32 low_x  = std::max(static_cast<i32>(std::floor(sphere.x - sphere.radius)), min_x);
            const i32 low_y  = std::max(static_cast<i32>(std::floor(sphere.y - sphere.radius)), min_y);
            const i32 low_z  = std::max(static_cast<i32>(std::floor(sphere.z - sphere.radius)), min_z);
            const i32 high_x = std::max(static_cast<i32>(std::floor(sphere.x + sphere.radius)), low_x);
            const i32 high_y = std::max(static_cast<i32>(std::floor(sphere.y + sphere.radius)), low_y);
            const i32 high_z = std::max(static_cast<i32>(std::floor(sphere.z + sphere.radius)), low_z);

            for (i32 x = low_x; x <= high_x; ++x) {
                const f64 fx = (static_cast<f64>(x) + 0.5 - sphere.x) / sphere.radius;
                if (fx * fx >= 1.0) {
                    continue;
                }
                for (i32 y = low_y; y <= high_y; ++y) {
                    const f64 fy = (static_cast<f64>(y) + 0.5 - sphere.y) / sphere.radius;
                    if (fx * fx + fy * fy >= 1.0) {
                        continue;
                    }
                    for (i32 z = low_z; z <= high_z; ++z) {
                        const f64 fz = (static_cast<f64>(z) + 0.5 - sphere.z) / sphere.radius;
                        if (fx * fx + fy * fy + fz * fz >= 1.0) {
                            continue;
                        }
                        if (level.outside_build_height(y)) {
                            continue;
                        }
                        const auto slot =
                            static_cast<usize>(x - min_x) +
                            static_cast<usize>(y - min_y) * static_cast<usize>(width) +
                            static_cast<usize>(z - min_z) * static_cast<usize>(width) *
                                static_cast<usize>(height);
                        if (slot >= occupied.size() || occupied[slot]) {
                            continue;
                        }
                        occupied[slot] = true;

                        const BlockPos here{x, y, z};
                        const auto           state = level.block_at(x, y, z);
                        for (const OreTarget& target : config_.targets) {
                            if (can_place(context, level, random, config_, target, state, here)) {
                                if (level.set_block(x, y, z, target.state)) {
                                    ++placed;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        return placed != 0;
    }

    OreConfig config_;
};

/// `scattered_ore`: single blocks thrown around the origin.
class ScatteredOreFeature final : public Feature {
public:
    explicit ScatteredOreFeature(OreConfig config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view type_name() const override { return "scattered_ore"; }

    bool place(const FeatureContext& context, FeatureLevel& level,
               FeatureRandom& random, BlockPos at) const override {
        // `size + 1`, so a scattered ore of size 3 places nothing about a
        // quarter of the time. Ancient debris is rare for this reason as much
        // as for its placement.
        const i32 count = random.next_int(config_.size + 1);
        for (i32 index = 0; index < count; ++index) {
            // The spread stops growing at seven, so a large vein is not a
            // larger cloud, only a denser one.
            const i32 spread = std::min(index, 7);
            const i32 dx     = axis_offset(random, spread);
            const i32 dy     = axis_offset(random, spread);
            const i32 dz     = axis_offset(random, spread);
            const BlockPos here{at.x + dx, at.y + dy, at.z + dz};
            const auto           state = level.block_at(here.x, here.y, here.z);
            for (const OreTarget& target : config_.targets) {
                if (can_place(context, level, random, config_, target, state, here)) {
                    level.set_block(here.x, here.y, here.z, target.state);
                    break;
                }
            }
        }
        // True whatever happened, exactly as the game reports it: this feature
        // does not tell its caller whether it found anywhere to go.
        return true;
    }

private:
    /// The difference of two draws, rounded. Two draws and not one: the
    /// difference is triangular and centred, where a single draw scaled to
    /// [-spread, spread] would be flat.
    [[nodiscard]] static i32 axis_offset(FeatureRandom& random, i32 spread) {
        const f32 offset = (random.next_float() - random.next_float()) *
                           static_cast<f32>(spread);
        // Java's Math.round on a float: floor of the value plus a half, which
        // rounds a negative half towards zero rather than away from it.
        return static_cast<i32>(std::floor(offset + 0.5F));
    }

    OreConfig config_;
};

}  // namespace

FeatureRef make_ore_feature(OreConfig config) {
    return std::make_shared<const OreFeature>(std::move(config));
}

FeatureRef make_scattered_ore_feature(OreConfig config) {
    return std::make_shared<const ScatteredOreFeature>(std::move(config));
}

}  // namespace ov::worldgen
