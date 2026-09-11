#include "ov/client/block_particles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace ov;
using namespace ov::client;

namespace {

constexpr AABB kCube{Vec3d{0, 0, 0}, Vec3d{1, 1, 1}};
constexpr AABB kSlab{Vec3d{0, 0, 0}, Vec3d{1, 0.5, 1}};
constexpr ParticleSprite kSprite{0.25F, 0.5F, 0.375F, 0.625F};

}  // namespace

TEST_CASE("a full block breaks into a four-by-four-by-four grid", "[particles]") {
    BlockParticles particles{42};
    particles.destroy(BlockPos{10, 64, -3}, std::array{kCube}, kSprite);
    REQUIRE(particles.count() == 64);
    for (const BlockParticle& p : particles.particles()) {
        CHECK(p.position.x > 10.0);
        CHECK(p.position.x < 11.0);
        CHECK(p.position.y > 64.0);
        CHECK(p.position.y < 65.0);
        CHECK(p.position.z > -3.0);
        CHECK(p.position.z < -2.0);
        CHECK(p.lifetime >= 4);
        CHECK(p.lifetime <= 40);
        // A quarter of the sprite, inside it.
        CHECK(std::abs((p.uv[0] - p.uv[2]) - (kSprite.u1 - kSprite.u0) / 4.0F) < 1e-6F);
        CHECK(p.uv[2] >= kSprite.u0);
        CHECK(p.uv[0] <= kSprite.u1);
        CHECK(p.uv[1] >= kSprite.v0);
        CHECK(p.uv[3] <= kSprite.v1);
        CHECK(p.colour[0] == 0.6F);
    }
}

TEST_CASE("a slab breaks into half as many, and two boxes into both grids", "[particles]") {
    BlockParticles slab{1};
    slab.destroy(BlockPos{0, 0, 0}, std::array{kSlab}, kSprite);
    CHECK(slab.count() == 4 * 2 * 4);

    // A torch-thin box still gets two a side.
    BlockParticles thin{1};
    thin.destroy(BlockPos{0, 0, 0}, std::array{AABB{Vec3d{0.4375, 0, 0.4375}, Vec3d{0.5625, 0.625, 0.5625}}},
                 kSprite);
    CHECK(thin.count() == 2 * 3 * 2);

    BlockParticles two{1};
    two.destroy(BlockPos{0, 0, 0}, std::array{kSlab, AABB{Vec3d{0, 0.5, 0}, Vec3d{0.5, 1, 1}}},
                kSprite);
    CHECK(two.count() == 32 + 2 * 2 * 4);
}

TEST_CASE("a crack particle sits just outside the face it came from", "[particles]") {
    BlockParticles particles{7};
    particles.crack(BlockPos{0, 60, 0}, Direction::Up, kCube, kSprite);
    particles.crack(BlockPos{0, 60, 0}, Direction::North, kSlab, kSprite);
    REQUIRE(particles.count() == 2);
    const BlockParticle& up = particles.particles()[0];
    CHECK(std::abs(up.position.y - 61.1) < 1e-9);
    CHECK(up.position.x >= 0.1);
    CHECK(up.position.x <= 0.9);
    const BlockParticle& north = particles.particles()[1];
    CHECK(std::abs(north.position.z + 0.1) < 1e-9);
    CHECK(north.position.y >= 60.1);
    CHECK(north.position.y <= 60.4);
    // Smaller and slower than a destroy particle.
    CHECK(up.size <= 0.1F * 0.6F + 1e-6F);
}

TEST_CASE("particles fall, age and go", "[particles]") {
    BlockParticles particles{3};
    particles.crack(BlockPos{0, 100, 0}, Direction::Up, kCube, kSprite);
    const BlockParticle first = particles.particles()[0];
    // The crack keeps the 0.1 lift and nothing else of note: it rises, then
    // gravity (0.04 a tick) and drag turn it round.
    CHECK(first.velocity.y > 0.09);
    CHECK(first.velocity.y < 0.11);
    f64 highest = first.position.y;
    f64 last    = first.position.y;
    for (i32 tick = 0; tick < 3 && particles.count() > 0; ++tick) {
        particles.tick(nullptr);
        if (particles.count() > 0) {
            last    = particles.particles()[0].position.y;
            highest = std::max(highest, last);
        }
    }
    CHECK(highest > first.position.y);

    BlockParticles many{4};
    many.destroy(BlockPos{0, 100, 0}, std::array{kCube}, kSprite);
    for (i32 tick = 0; tick < 41; ++tick) {
        many.tick(nullptr);
    }
    CHECK(many.count() == 0);  // the longest lives 40 ticks
}

TEST_CASE("free fall accelerates by gravity and drag alone", "[particles]") {
    BlockParticles particles{5};
    particles.crack(BlockPos{0, 100, 0}, Direction::Up, kCube, kSprite);
    const f64 v0 = particles.particles()[0].velocity.y;
    particles.tick(nullptr);
    REQUIRE(particles.count() == 1);
    // (v - 0.04) * 0.98, with nothing below to stop it.
    CHECK(std::abs(particles.particles()[0].velocity.y - (v0 - 0.04) * 0.98) < 1e-12);
    CHECK_FALSE(particles.particles()[0].on_ground);
}

TEST_CASE("each particle draws as one camera-facing square", "[particles]") {
    BlockParticles particles{9};
    particles.crack(BlockPos{0, 0, 0}, Direction::Up, kCube, kSprite);
    std::vector<render::EntityVertex> out;
    particles.build(Vec3f{1, 0, 0}, Vec3f{0, 1, 0}, 0.0F,
                    [](Vec3f) { return std::array<u8, 3>{255, 128, 255}; }, out);
    REQUIRE(out.size() == 4);
    const f32 size = particles.particles()[0].size;
    CHECK(std::abs((out[1].x - out[0].x) - 2.0F * size) < 1e-5F);
    CHECK(std::abs((out[3].y - out[0].y) - 2.0F * size) < 1e-5F);
    CHECK(out[0].z == out[2].z);
    // 0.6 of the light.
    CHECK(out[0].colour[0] == 153);
    CHECK(out[0].colour[1] == 76);
}

TEST_CASE("past the cap the oldest go first", "[particles]") {
    BlockParticles particles{11};
    for (usize i = 0; i < BlockParticles::kMaxParticles / 64 + 2; ++i) {
        particles.destroy(BlockPos{static_cast<i32>(i), 0, 0}, std::array{kCube}, kSprite);
    }
    CHECK(particles.count() == BlockParticles::kMaxParticles);
    CHECK(particles.particles()[0].position.x >= 2.0);
}
