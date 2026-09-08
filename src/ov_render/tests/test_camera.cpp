#include "ov/render/camera.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

namespace {

/// Apply a matrix to a point and divide through by w.
Vec3f project_point(const Mat4& matrix, Vec3f point) {
    const auto& m = matrix.m;
    const f32   x = m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12];
    const f32   y = m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13];
    const f32   z = m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14];
    const f32   w = m[3] * point.x + m[7] * point.y + m[11] * point.z + m[15];
    return Vec3f{x / w, y / w, z / w};
}

}  // namespace

TEST_CASE("the camera faces the way the protocol says it does", "[camera]") {
    // Minecraft's own convention, and the reason not to invent one: yaw and
    // pitch arrive over the wire in exactly this form. yaw 0 looks south (+Z),
    // and yaw increases clockwise seen from above, so yaw 90 looks west (-X).
    Camera camera;

    camera.yaw_degrees = 0.0F;
    CHECK(camera.forward().z == Approx(1.0F));
    CHECK(camera.forward().x == Approx(0.0F).margin(1e-6));

    camera.yaw_degrees = 90.0F;
    CHECK(camera.forward().x == Approx(-1.0F));

    camera.yaw_degrees = 180.0F;
    CHECK(camera.forward().z == Approx(-1.0F));

    camera.yaw_degrees = 270.0F;
    CHECK(camera.forward().x == Approx(1.0F));

    // Pitch is positive downwards, as the protocol sends it.
    camera.yaw_degrees   = 0.0F;
    camera.pitch_degrees = 90.0F;
    CHECK(camera.forward().y == Approx(-1.0F));
}

TEST_CASE("pitch stops just short of vertical", "[camera]") {
    // At exactly vertical the forward vector is parallel to world up, the right
    // vector is undefined, and the view matrix collapses to nothing on screen.
    Camera camera;
    camera.turn(0.0F, 100000.0F);
    CHECK(camera.pitch_degrees < 90.0F);
    CHECK(camera.pitch_degrees > 89.0F);

    camera.turn(0.0F, -100000.0F);
    CHECK(camera.pitch_degrees > -90.0F);
    CHECK(camera.pitch_degrees < -89.0F);
}

TEST_CASE("yaw stays inside the range the protocol uses", "[camera]") {
    Camera camera;
    camera.turn(-100000.0F, 0.0F);
    CHECK(camera.yaw_degrees <= 180.0F);
    CHECK(camera.yaw_degrees >= -180.0F);
}

TEST_CASE("the projection puts near at 0 and far at 1", "[camera]") {
    // Vulkan's depth range, not OpenGL's. Getting this wrong gives a picture
    // that looks right until half of it is clipped away.
    const Mat4 projection = perspective(1.0F, 16.0F / 9.0F, 0.1F, 100.0F);

    // The camera looks down -Z, so a point in front has a negative z.
    CHECK(project_point(projection, Vec3f{0.0F, 0.0F, -0.1F}).z == Approx(0.0F).margin(1e-5));
    CHECK(project_point(projection, Vec3f{0.0F, 0.0F, -100.0F}).z == Approx(1.0F).margin(1e-5));
}

TEST_CASE("the view matrix puts what the camera looks at in the middle", "[camera]") {
    Camera camera;
    camera.position      = Vec3f{5.0F, 5.0F, 5.0F};
    camera.yaw_degrees   = 0.0F;
    camera.pitch_degrees = 0.0F;

    // Looking south, so a block further south is dead ahead.
    const Vec3f target = camera.position + camera.forward() * 10.0F;
    const Vec3f centre = project_point(camera.view_projection(1.0F), target);

    CHECK(centre.x == Approx(0.0F).margin(1e-5));
    CHECK(centre.y == Approx(0.0F).margin(1e-5));
}

TEST_CASE("up is up on screen", "[camera]") {
    // The viewport flips Y, so a point above the camera's line of sight has to
    // land at *negative* clip y for it to appear above the centre. Pinning it
    // here is what stops the flip being applied twice.
    Camera camera;
    camera.position    = Vec3f{0.0F, 0.0F, 0.0F};
    camera.yaw_degrees = 0.0F;

    const Vec3f ahead = Vec3f{0.0F, 0.0F, 10.0F};
    const Vec3f above = Vec3f{0.0F, 3.0F, 10.0F};

    const f32 y_ahead = project_point(camera.view_projection(1.0F), ahead).y;
    const f32 y_above = project_point(camera.view_projection(1.0F), above).y;

    CHECK(y_above > y_ahead);
}

TEST_CASE("multiplying by the identity changes nothing", "[camera]") {
    const Mat4 projection = perspective(1.2F, 1.5F, 0.05F, 200.0F);
    const Mat4 same       = projection * Mat4::identity();
    for (usize i = 0; i < 16; ++i) {
        CHECK(same.m[i] == Approx(projection.m[i]));
    }
}
