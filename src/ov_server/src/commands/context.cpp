#include "context.hpp"

#include <cmath>
#include <numbers>

namespace ov::server::cmd {

Vec3d Coordinates::position(const CommandSource& source) const {
    if (!local) {
        return Vec3d{axes[0].relative ? source.position.x + axes[0].value : axes[0].value,
                     axes[1].relative ? source.position.y + axes[1].value : axes[1].value,
                     axes[2].relative ? source.position.z + axes[2].value : axes[2].value};
    }
    // Local: `^left ^up ^forwards` in the source's own frame. The angles are
    // turned into radians in double precision — the capture of
    // `tp @s ^ ^ ^1` at yaw 90 moved z by exactly 1.1e-16, which is
    // 0.5 + sin(pi) in double and nothing a float table produces.
    constexpr f64 kRadians = std::numbers::pi / 180.0;
    const f64     f        = std::cos(static_cast<f64>(source.yaw + 90.0F) * kRadians);
    const f64     g        = std::sin(static_cast<f64>(source.yaw + 90.0F) * kRadians);
    const f64     h        = std::cos(static_cast<f64>(-source.pitch) * kRadians);
    const f64     i        = std::sin(static_cast<f64>(-source.pitch) * kRadians);
    const f64     j        = std::cos(static_cast<f64>(-source.pitch + 90.0F) * kRadians);
    const f64     k        = std::sin(static_cast<f64>(-source.pitch + 90.0F) * kRadians);
    const Vec3d   forwards{f * h, i, g * h};
    const Vec3d   up{f * j, k, g * j};
    // left = -(forwards × up)
    const Vec3d left{-(forwards.y * up.z - forwards.z * up.y),
                     -(forwards.z * up.x - forwards.x * up.z),
                     -(forwards.x * up.y - forwards.y * up.x)};
    const f64 x_left     = axes[0].value;
    const f64 y_up       = axes[1].value;
    const f64 z_forwards = axes[2].value;
    const f64 dx = forwards.x * z_forwards + up.x * y_up + left.x * x_left;
    const f64 dy = forwards.y * z_forwards + up.y * y_up + left.y * x_left;
    const f64 dz = forwards.z * z_forwards + up.z * y_up + left.z * x_left;
    return Vec3d{source.position.x + dx, source.position.y + dy, source.position.z + dz};
}

BlockPos Coordinates::block(const CommandSource& source) const {
    const Vec3d p = position(source);
    return BlockPos{static_cast<i32>(std::floor(p.x)), static_cast<i32>(std::floor(p.y)),
                    static_cast<i32>(std::floor(p.z))};
}

f32 wrap_degrees(f32 degrees) noexcept {
    f32 wrapped = std::fmod(degrees, 360.0F);
    if (wrapped >= 180.0F) {
        wrapped -= 360.0F;
    }
    if (wrapped < -180.0F) {
        wrapped += 360.0F;
    }
    return wrapped;
}

f32 AngleArg::resolve(const CommandSource& source) const noexcept {
    return wrap_degrees(relative ? value + source.yaw : value);
}

}  // namespace ov::server::cmd
