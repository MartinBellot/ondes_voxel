#include "ov/render/camera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::render {

namespace {

[[nodiscard]] f32 to_radians(f32 degrees) noexcept {
    return degrees * (std::numbers::pi_v<f32> / 180.0F);
}

}  // namespace

Mat4 Mat4::identity() noexcept {
    Mat4 result;
    result.m[0]  = 1.0F;
    result.m[5]  = 1.0F;
    result.m[10] = 1.0F;
    result.m[15] = 1.0F;
    return result;
}

Mat4 Mat4::operator*(const Mat4& other) const noexcept {
    Mat4 result;
    for (usize column = 0; column < 4; ++column) {
        for (usize row = 0; row < 4; ++row) {
            f32 sum = 0.0F;
            for (usize k = 0; k < 4; ++k) {
                sum += m[k * 4 + row] * other.m[column * 4 + k];
            }
            result.m[column * 4 + row] = sum;
        }
    }
    return result;
}

Mat4 perspective(f32 vertical_fov_radians, f32 aspect, f32 near_plane, f32 far_plane) noexcept {
    const f32 focal = 1.0F / std::tan(vertical_fov_radians * 0.5F);

    Mat4 result;
    result.m[0]  = focal / aspect;
    result.m[5]  = focal;
    result.m[10] = far_plane / (near_plane - far_plane);
    result.m[11] = -1.0F;
    result.m[14] = (far_plane * near_plane) / (near_plane - far_plane);
    return result;
}

Mat4 look_along(Vec3f eye, Vec3f forward, Vec3f up) noexcept {
    const Vec3f f = forward.normalized();
    const Vec3f s = f.cross(up).normalized();
    const Vec3f u = s.cross(f);

    Mat4 result = Mat4::identity();
    result.m[0] = s.x;
    result.m[4] = s.y;
    result.m[8] = s.z;

    result.m[1] = u.x;
    result.m[5] = u.y;
    result.m[9] = u.z;

    result.m[2]  = -f.x;
    result.m[6]  = -f.y;
    result.m[10] = -f.z;

    result.m[12] = -s.dot(eye);
    result.m[13] = -u.dot(eye);
    result.m[14] = f.dot(eye);
    return result;
}

Mat4 translation(Vec3f offset) noexcept {
    Mat4 result  = Mat4::identity();
    result.m[12] = offset.x;
    result.m[13] = offset.y;
    result.m[14] = offset.z;
    return result;
}

Vec3f Camera::forward() const noexcept {
    // Minecraft's own formulation. yaw 0 faces +Z and yaw increases clockwise
    // from above; pitch is positive downwards.
    const f32 yaw       = to_radians(yaw_degrees);
    const f32 pitch     = to_radians(pitch_degrees);
    const f32 cos_pitch = std::cos(pitch);
    return Vec3f{-std::sin(yaw) * cos_pitch, -std::sin(pitch), std::cos(yaw) * cos_pitch};
}

Vec3f Camera::right() const noexcept {
    return forward().cross(Vec3f{0.0F, 1.0F, 0.0F}).normalized();
}

void Camera::turn(f32 delta_x, f32 delta_y, f32 sensitivity) noexcept {
    yaw_degrees -= delta_x * sensitivity;
    pitch_degrees += delta_y * sensitivity;

    // Just short of vertical. At exactly 90 the forward vector is parallel to
    // world up and the view matrix has no defined right vector.
    pitch_degrees = std::clamp(pitch_degrees, -89.9F, 89.9F);

    // Wrapped with fmod, not with one subtraction: a single correction only
    // handles one turn, and a fast flick of the mouse is many. The protocol
    // sends yaw as a float in this range, so letting it drift to 14000 degrees
    // is a desynchronisation waiting to happen rather than a cosmetic issue.
    yaw_degrees = std::fmod(yaw_degrees + 180.0F, 360.0F);
    if (yaw_degrees < 0.0F) {
        yaw_degrees += 360.0F;
    }
    yaw_degrees -= 180.0F;
}

Mat4 Camera::view() const noexcept {
    return look_along(position, forward(), Vec3f{0.0F, 1.0F, 0.0F});
}

Mat4 Camera::projection(f32 aspect) const noexcept {
    return perspective(to_radians(vertical_fov_degrees), aspect, near_plane, far_plane);
}

Mat4 Camera::view_projection(f32 aspect) const noexcept {
    return projection(aspect) * view();
}

}  // namespace ov::render
