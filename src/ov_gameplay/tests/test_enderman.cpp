// ── mobs-5 ── The enderman's rules, against the real server's campaigns
// (scripts/measure_hostile.py `enderman2`, `enderman3`), docs/provenance/mobs-5.md.
#include "ov/gameplay/enderman.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr f64 kDegrees = 3.14159265358979323846 / 180.0;

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] registry::BlockStateId state(const std::string& name) {
    return blocks()->default_state(blocks()->find_block(name).value());
}

/// Flat ground: `floor` at y = −1, `surface` at y = 0 (air by default).
struct Flat {
    registry::BlockStateId floor{};
    registry::BlockStateId surface{registry::kAirState};

    static registry::BlockStateId look_up(void* context, i32, i32 y, i32) {
        const auto* self = static_cast<const Flat*>(context);
        return y == -1 ? self->floor : y == 0 ? self->surface : registry::kAirState;
    }
};

/// The campaign's geometry: the probe's eyes at y 1.62 on flat ground, an
/// enderman in a pit two deep `d` blocks east, its eyes at −2 + 2.55.
[[nodiscard]] bool angered_at(f64 d, f64 offset_degrees) {
    const Vec3d eye{0.5, 1.62, 0.5};
    const Vec3d target{d + 0.5, -2.0 + 2.55, 0.5};
    const f64   pitch = -std::atan2(target.y - eye.y, d) / kDegrees - offset_degrees;
    return looks_at(eye, view_vector(-90.0F, static_cast<f32>(pitch)), target);
}

}  // namespace

TEST_CASE("the view vector follows the protocol's yaw and pitch", "[gameplay][enderman]") {
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(view_vector(0.0F, 0.0F).z, WithinAbs(1.0, 1e-12));
    CHECK_THAT(view_vector(-90.0F, 0.0F).x, WithinAbs(1.0, 1e-12));
    CHECK_THAT(view_vector(90.0F, 0.0F).x, WithinAbs(-1.0, 1e-12));
    CHECK_THAT(view_vector(0.0F, 90.0F).y, WithinAbs(-1.0, 1e-12));
}

TEST_CASE("the stare's cone closes with distance, as measured", "[gameplay][enderman][parity]") {
    // enderman2, cone: angry up to 4° above the eyes at 8 blocks, not at 5°;
    // up to 3° at 16 blocks, not at 4°.
    for (const f64 offset : {0.0, 1.0, 2.0, 3.0, 4.0}) {
        CHECK(angered_at(8.0, offset));
    }
    CHECK_FALSE(angered_at(8.0, 5.0));
    CHECK_FALSE(angered_at(8.0, 7.0));
    for (const f64 offset : {0.0, 1.0, 2.0, 3.0}) {
        CHECK(angered_at(16.0, offset));
    }
    CHECK_FALSE(angered_at(16.0, 4.0));
    CHECK_FALSE(angered_at(16.0, 5.0));
    // Out of range, however straight the look.
    CHECK_FALSE(looks_at(Vec3d{0.0, 0.0, 0.0}, Vec3d{1.0, 0.0, 0.0}, Vec3d{65.0, 0.0, 0.0}));
    CHECK(looks_at(Vec3d{0.0, 0.0, 0.0}, Vec3d{1.0, 0.0, 0.0}, Vec3d{63.0, 0.0, 0.0}));
}

TEST_CASE("anger lasts twenty to thirty-nine seconds", "[gameplay][enderman]") {
    math::LegacyRandomSource random{1234};
    i32                      low  = kAngerMaxTicks;
    i32                      high = 0;
    for (i32 i = 0; i < 2000; ++i) {
        const i32 ticks = draw_anger_ticks(random);
        low             = std::min(low, ticks);
        high            = std::max(high, ticks);
    }
    CHECK(low >= kAngerMinTicks);
    CHECK(high < kAngerMaxTicks);
    // The measured 709, fifty ticks after the stare, is inside the draw.
    CHECK(709 + 50 >= kAngerMinTicks);
    CHECK(709 + 50 < kAngerMaxTicks);
}

TEST_CASE("a teleport lands within 32 blocks, on the ground, with room", "[gameplay][enderman]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    Flat                     flat{state("minecraft:stone")};
    const CollisionWorld     world{*blocks(), &Flat::look_up, &flat};
    math::LegacyRandomSource random{42};
    const Vec3d              from{0.5, 0.0, 0.5};
    usize                    landed = 0;
    for (i32 i = 0; i < 200; ++i) {
        const auto landing = random_teleport(world, from, 0.6, 2.9, -64, random);
        if (!landing) {
            continue;
        }
        ++landed;
        CHECK(std::abs(landing->x - from.x) <= kTeleportHalfSpan);
        CHECK(std::abs(landing->z - from.z) <= kTeleportHalfSpan);
        // On flat ground a landing keeps y, as the sixteen measured hops did.
        CHECK(landing->y == 0.0);
    }
    // Half the draws fall below the ground and are refused; 64 attempts
    // nearly always find one of the others.
    CHECK(landed >= 195);
}

TEST_CASE("a teleport never lands in water", "[gameplay][enderman]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    // Every spot that could be landed on is water: the teleport gives up.
    Flat                     wet{state("minecraft:stone"), state("minecraft:water")};
    const CollisionWorld     world{*blocks(), &Flat::look_up, &wet};
    math::LegacyRandomSource random{7};
    for (i32 i = 0; i < 20; ++i) {
        CHECK_FALSE(random_teleport(world, Vec3d{0.5, 0.0, 0.5}, 0.6, 2.9, -64, random));
    }
}

TEST_CASE("the take looks from the feet up, the put beside the feet", "[gameplay][enderman]") {
    math::LegacyRandomSource random{99};
    const Vec3d              feet{10.5, 64.0, -3.5};
    for (i32 i = 0; i < 2000; ++i) {
        const BlockPos take = pick_target(feet, random);
        CHECK(take.x >= 8);
        CHECK(take.x <= 12);
        CHECK(take.z >= -6);
        CHECK(take.z <= -2);
        // Never the block under the feet: enderman2 took nothing on bare grass.
        CHECK(take.y >= 64);
        CHECK(take.y <= 66);
        const BlockPos put = place_target(feet, random);
        CHECK(put.x >= 9);
        CHECK(put.x <= 11);
        CHECK(put.y >= 64);
        CHECK(put.y <= 65);
    }
}
