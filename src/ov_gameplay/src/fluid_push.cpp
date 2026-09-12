#include "ov/gameplay/fluid_push.hpp"

#include <cmath>

namespace ov::gameplay {
namespace {

[[nodiscard]] i32 floor_to_int(f64 value) noexcept {
    const auto truncated = static_cast<i32>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

[[nodiscard]] f64 length_of(const Vec3d& v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

}  // namespace

Vec3d current_push(const FluidRules& rules, const world::LevelView& level, const AABB& box,
                   FluidKind kind, const Vec3d& velocity, bool player, f64 strength,
                   const CurrentConstants& constants) {
    // The same deflated box the fluid test uses: standing against a wall of
    // water is not being in it.
    constexpr f64 kShrink = 0.001;
    const f64     bottom  = box.min.y + kShrink;

    Vec3d sum{};
    i32   counted = 0;
    for (i32 x = floor_to_int(box.min.x + kShrink); x <= floor_to_int(box.max.x - kShrink); ++x) {
        for (i32 y = floor_to_int(bottom); y <= floor_to_int(box.max.y - kShrink); ++y) {
            for (i32 z = floor_to_int(box.min.z + kShrink); z <= floor_to_int(box.max.z - kShrink);
                 ++z) {
                const BlockPos   pos{x, y, z};
                const FluidState here = rules.fluid_at(level.block_at(pos));
                if (here.kind != kind) {
                    continue;
                }
                // A block under more of the same fluid is full to the top;
                // otherwise it stands at its own height, in ninths.
                const FluidState above = rules.fluid_at(level.block_at(BlockPos{x, y + 1, z}));
                const f64 height = above.kind == kind ? 1.0 : static_cast<f64>(here.height()) / 9.0;
                const f64 top    = static_cast<f64>(y) + height;
                if (top < bottom) {
                    continue;
                }
                Vec3d     flow = rules.flow_vector(level, pos);
                const f64 size = length_of(flow);
                if (size > 0.0) {
                    flow = Vec3d{flow.x / size, flow.y / size, flow.z / size};
                }
                const f64 depth = top - bottom;
                if (depth < constants.shallow) {
                    flow = Vec3d{flow.x * depth, flow.y * depth, flow.z * depth};
                }
                sum.x += flow.x;
                sum.y += flow.y;
                sum.z += flow.z;
                ++counted;
            }
        }
    }

    if (counted == 0 || length_of(sum) <= 0.0) {
        return Vec3d{};
    }
    const f64 average = 1.0 / static_cast<f64>(counted);
    sum               = Vec3d{sum.x * average, sum.y * average, sum.z * average};
    if (!player) {
        const f64 size = length_of(sum);
        sum            = Vec3d{sum.x / size, sum.y / size, sum.z / size};
    }
    Vec3d push{sum.x * strength, sum.y * strength, sum.z * strength};

    // A nearly still entity always feels at least the floor, in the same
    // direction — which is what lets a weak current start something moving.
    const f64 size = length_of(push);
    if (std::abs(velocity.x) < constants.still && std::abs(velocity.z) < constants.still &&
        size < constants.min_push && size > 0.0) {
        const f64 scale = constants.min_push / size;
        push            = Vec3d{push.x * scale, push.y * scale, push.z * scale};
    }
    return push;
}

}  // namespace ov::gameplay
