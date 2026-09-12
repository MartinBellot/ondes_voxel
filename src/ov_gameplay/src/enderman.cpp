// ── mobs-5 ── See enderman.hpp.
#include "ov/gameplay/enderman.hpp"

#include "ov/math/aabb.hpp"

#include <cmath>

namespace ov::gameplay {
namespace {

constexpr f64 kDegrees = 3.14159265358979323846 / 180.0;

[[nodiscard]] i32 floor_of(f64 v) noexcept { return static_cast<i32>(std::floor(v)); }

}  // namespace

Vec3d view_vector(f32 yaw, f32 pitch) noexcept {
    const f64 y = static_cast<f64>(yaw) * kDegrees;
    const f64 p = static_cast<f64>(pitch) * kDegrees;
    return Vec3d{-std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)};
}

bool looks_at(const Vec3d& eye, const Vec3d& view, const Vec3d& target_eye, f64 slack) noexcept {
    const Vec3d to{target_eye.x - eye.x, target_eye.y - eye.y, target_eye.z - eye.z};
    const f64   distance = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    if (distance < 1e-9 || distance > kStareRange) {
        return false;
    }
    const f64 view_length = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
    if (view_length < 1e-9) {
        return false;
    }
    const f64 cosine = (view.x * to.x + view.y * to.y + view.z * to.z) / (view_length * distance);
    return cosine > 1.0 - slack / distance;
}

i32 draw_anger_ticks(math::LegacyRandomSource& random) noexcept {
    return kAngerMinTicks + random.next_int(kAngerMaxTicks - kAngerMinTicks);
}

std::optional<Vec3d> teleport_attempt(const CollisionWorld& collisions, const Vec3d& from,
                                      f64 width, f64 height, i32 min_y,
                                      math::LegacyRandomSource& random) {
    const f64 x = from.x + (random.next_double() - 0.5) * 2.0 * kTeleportHalfSpan;
    const f64 y = from.y + std::floor((random.next_double() - 0.5) * 2.0 * kTeleportHalfSpan);
    const f64 z = from.z + (random.next_double() - 0.5) * 2.0 * kTeleportHalfSpan;

    const registry::BlockRegistry& blocks = collisions.blocks();
    const i32                      bx     = floor_of(x);
    const i32                      bz     = floor_of(z);
    i32                            by     = floor_of(y);
    // Lowered onto the first block below that stops motion.
    while (by > min_y &&
           !blocks.blocks_motion(blocks.block_of(collisions.state_at(bx, by - 1, bz)))) {
        --by;
    }
    if (by <= min_y) {
        return std::nullopt;
    }
    // Never into a liquid: an enderman that teleports to escape water must not
    // land in it again.
    if (blocks.holds_fluid(collisions.state_at(bx, by, bz))) {
        return std::nullopt;
    }
    const Vec3d landing{x, static_cast<f64>(by), z};
    if (collisions.overlaps(AABB::from_entity(landing, width, height))) {
        return std::nullopt;
    }
    return landing;
}

std::optional<Vec3d> random_teleport(const CollisionWorld& collisions, const Vec3d& from,
                                     f64 width, f64 height, i32 min_y,
                                     math::LegacyRandomSource& random) {
    for (i32 attempt = 0; attempt < kTeleportAttempts; ++attempt) {
        if (const auto landing = teleport_attempt(collisions, from, width, height, min_y, random)) {
            return landing;
        }
    }
    return std::nullopt;
}

BlockPos pick_target(const Vec3d& feet, math::LegacyRandomSource& random) noexcept {
    const i32 x = floor_of(feet.x - 2.0 + random.next_double() * 4.0);
    const i32 y = floor_of(feet.y + random.next_double() * 3.0);
    const i32 z = floor_of(feet.z - 2.0 + random.next_double() * 4.0);
    return BlockPos{x, y, z};
}

BlockPos place_target(const Vec3d& feet, math::LegacyRandomSource& random) noexcept {
    const i32 x = floor_of(feet.x - 1.0 + random.next_double() * 2.0);
    const i32 y = floor_of(feet.y + random.next_double() * 2.0);
    const i32 z = floor_of(feet.z - 1.0 + random.next_double() * 2.0);
    return BlockPos{x, y, z};
}

}  // namespace ov::gameplay
