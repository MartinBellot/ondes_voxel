#include "ov/render/frustum.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;

namespace {

Camera looking_south() {
    Camera camera;
    camera.position      = Vec3f{0.0F, 0.0F, 0.0F};
    camera.yaw_degrees   = 0.0F;  // +Z
    camera.pitch_degrees = 0.0F;
    camera.near_plane    = 0.1F;
    camera.far_plane     = 100.0F;
    return camera;
}

Frustum frustum_of(const Camera& camera) {
    return Frustum::from_view_projection(camera.view_projection(1.0F));
}

/// A one-block box at a position.
std::pair<Vec3f, Vec3f> block_at(f32 x, f32 y, f32 z) {
    return {Vec3f{x, y, z}, Vec3f{x + 1.0F, y + 1.0F, z + 1.0F}};
}

}  // namespace

TEST_CASE("a box in front of the camera is kept", "[frustum]") {
    const auto frustum = frustum_of(looking_south());
    const auto box     = block_at(0.0F, 0.0F, 10.0F);
    CHECK(frustum.intersects(box.first, box.second));
}

TEST_CASE("a box behind the camera is culled", "[frustum]") {
    const auto frustum = frustum_of(looking_south());
    const auto box     = block_at(0.0F, 0.0F, -20.0F);
    CHECK_FALSE(frustum.intersects(box.first, box.second));
}

TEST_CASE("a box past the far plane is culled", "[frustum]") {
    const auto frustum = frustum_of(looking_south());
    const auto box     = block_at(0.0F, 0.0F, 500.0F);
    CHECK_FALSE(frustum.intersects(box.first, box.second));
}

TEST_CASE("a box far off to the side is culled", "[frustum]") {
    const auto frustum = frustum_of(looking_south());
    const auto box     = block_at(500.0F, 0.0F, 10.0F);
    CHECK_FALSE(frustum.intersects(box.first, box.second));
    const auto above = block_at(0.0F, 500.0F, 10.0F);
    CHECK_FALSE(frustum.intersects(above.first, above.second));
}

TEST_CASE("a box the camera stands inside is kept", "[frustum]") {
    // The section the player is in straddles the near plane and every side
    // plane. Culling it would delete the ground under their feet.
    const auto  frustum = frustum_of(looking_south());
    const Vec3f minimum{-8.0F, -8.0F, -8.0F};
    const Vec3f maximum{8.0F, 8.0F, 8.0F};
    CHECK(frustum.intersects(minimum, maximum));
}

TEST_CASE("the near plane uses Vulkan's depth range, not OpenGL's", "[frustum]") {
    // With the OpenGL form (w + z) the near plane lands halfway to the far
    // plane, and everything closer than fifty blocks vanishes — which looks
    // exactly like the world disappearing as you walk towards it.
    const auto frustum = frustum_of(looking_south());
    for (f32 distance : {0.5F, 1.0F, 5.0F, 20.0F, 49.0F, 60.0F}) {
        const auto box = block_at(0.0F, 0.0F, distance);
        CHECK(frustum.intersects(box.first, box.second));
    }
}

TEST_CASE("turning the camera changes what survives", "[frustum]") {
    auto       camera   = looking_south();
    const auto in_front = block_at(0.0F, 0.0F, 20.0F);

    CHECK(frustum_of(camera).intersects(in_front.first, in_front.second));

    camera.yaw_degrees = 180.0F;  // now facing -Z
    CHECK_FALSE(frustum_of(camera).intersects(in_front.first, in_front.second));
}

TEST_CASE("the planes come out normalised", "[frustum]") {
    // Not cosmetic: an unnormalised plane still gives the right sign, but the
    // distance it reports is scaled, and the next person to use it for a
    // near-plane fade will get a silently wrong number.
    const auto frustum = frustum_of(looking_south());
    for (const auto& plane : frustum.planes()) {
        const f32 length_squared = plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2];
        CHECK(length_squared > 0.99F);
        CHECK(length_squared < 1.01F);
    }
}
