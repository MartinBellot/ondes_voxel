#include "ov/gameplay/physics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] const std::optional<registry::BlockRegistry>& pack() {
    static const auto loaded = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        std::optional<registry::BlockRegistry> out;
        if (auto blocks = registry::BlockRegistry::load(path)) {
            out = std::move(*blocks);
        }
        return out;
    }();
    return loaded;
}

/// A floor at y = 0 and nothing else.
struct Floor {
    registry::BlockStateId stone{0};

    static registry::BlockStateId look_up(void* context, i32 /*x*/, i32 y, i32 /*z*/) {
        const auto* self = static_cast<const Floor*>(context);
        return y == -1 ? self->stone : registry::BlockStateId{0};
    }
};

/// Run the same input until the speed stops changing, and report it.
[[nodiscard]] f64 settled_speed(const MoveInput& input, const CollisionWorld& world) {
    MotionState           state{Vec3d{0.5, 0.0, 0.5}, Vec3d{}, true};
    const MotionConstants constants;
    f64                   last = 0.0;
    for (int tick = 0; tick < 400; ++tick) {
        const Vec3d before = state.position;
        state              = step(state, input, constants, world);
        last               = std::hypot(state.position.x - before.x, state.position.z - before.z);
    }
    return last;
}

}  // namespace

TEST_CASE("walking, sprinting and sneaking settle where the game does", "[gameplay][physics]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    Floor floor;
    floor.stone = pack()->default_state(*pack()->find_block("minecraft:stone"));
    const CollisionWorld world{*pack(), &Floor::look_up, &floor};

    // Every figure below was read off a real 1.20.1 client reporting its own
    // position to this server, twenty times a second, and each is a plateau
    // held for dozens of ticks. They agree with the published formulas to four
    // decimals — the two were arrived at separately.
    MoveInput input;
    input.forward = 1.0F;

    REQUIRE(std::abs(settled_speed(input, world) - 0.21578) < 0.0005);

    input.sprint = true;
    REQUIRE(std::abs(settled_speed(input, world) - 0.28058) < 0.0005);

    input.sprint = false;
    input.sneak  = true;
    REQUIRE(std::abs(settled_speed(input, world) - 0.06474) < 0.0005);
}

TEST_CASE("a positive strafe goes to the player's left, as vanilla's xxa", "[gameplay][physics]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    Floor floor;
    floor.stone = pack()->default_state(*pack()->find_block("minecraft:stone"));
    const CollisionWorld world{*pack(), &Floor::look_up, &floor};

    // Where one tick of an input takes a player standing still at the origin.
    const auto moved = [&](f32 yaw, f32 forward, f32 strafe) {
        MoveInput input;
        input.yaw     = yaw;
        input.forward = forward;
        input.strafe  = strafe;
        MotionState state{Vec3d{0.5, 0.0, 0.5}, Vec3d{}, true};
        state = step(state, input, MotionConstants{}, world);
        return Vec3d{state.position.x - 0.5, 0.0, state.position.z - 0.5};
    };

    // Yaw 0 faces south (+Z), so the left hand points east (+X). The client
    // once fed right-minus-left here and every strafe went the wrong way.
    const Vec3d south_left = moved(0.0F, 0.0F, 1.0F);
    REQUIRE(south_left.x > 0.0);
    REQUIRE(std::abs(south_left.z) < 1.0E-9);
    const Vec3d south_ahead = moved(0.0F, 1.0F, 0.0F);
    REQUIRE(south_ahead.z > 0.0);

    // Yaw 90 faces west (-X), so the left hand points south (+Z).
    const Vec3d west_ahead = moved(90.0F, 1.0F, 0.0F);
    REQUIRE(west_ahead.x < 0.0);
    const Vec3d west_left = moved(90.0F, 0.0F, 1.0F);
    REQUIRE(west_left.z > 0.0);
}

TEST_CASE("a jump leaves the ground at exactly 0.42", "[gameplay][physics]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    Floor floor;
    floor.stone = pack()->default_state(*pack()->find_block("minecraft:stone"));
    const CollisionWorld world{*pack(), &Floor::look_up, &floor};

    MoveInput input;
    input.jump = true;

    MotionState state{Vec3d{0.5, 0.0, 0.5}, Vec3d{}, true};
    state = step(state, input, MotionConstants{}, world);

    // The client reported 0.420000 on the first tick of every jump it made,
    // with no rounding to speak of.
    REQUIRE(std::abs(state.position.y - 0.42) < 1.0E-9);

    // And the apex of a standing jump is a fraction over a block and a
    // quarter, which is why a player clears a slab and not a full block.
    input.jump = false;
    f64 apex   = state.position.y;
    for (int tick = 0; tick < 40 && !state.on_ground; ++tick) {
        state = step(state, input, MotionConstants{}, world);
        apex  = std::max(apex, state.position.y);
    }
    REQUIRE(apex > 1.24);
    REQUIRE(apex < 1.26);
    REQUIRE(state.on_ground);
    REQUIRE(std::abs(state.position.y) < 1.0E-9);
}

TEST_CASE("falling matches the trace tick for tick", "[gameplay][physics]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    Floor floor;
    floor.stone = registry::BlockStateId{0};  // nothing to land on
    const CollisionWorld world{*pack(), &Floor::look_up, &floor};

    MotionState state{Vec3d{0.5, 100.0, 0.5}, Vec3d{}, false};

    // The first tick of a fall moves nothing: the displacement uses the
    // velocity left by the tick before, and there was none. The client does not
    // send a position packet for a tick where it did not move, which is why the
    // recorded trace appears to start at 0.0784.
    state = step(state, MoveInput{}, MotionConstants{}, world);
    REQUIRE(state.position.y == 100.0);

    f64 previous = state.position.y;

    // Gravity 0.08 then drag 0.98, in that order: the first tick of a fall is
    // 0.0784 and not 0.08. Fitting the client's own trace gave 0.0799978 and
    // 0.980014, which is the same pair through the noise of a network.
    const std::array<f64, 5> expected{0.0784, 0.155232, 0.23052736, 0.30431681, 0.37663048};
    for (const f64 want : expected) {
        state          = step(state, MoveInput{}, MotionConstants{}, world);
        const f64 fell = previous - state.position.y;
        previous       = state.position.y;
        REQUIRE(std::abs(fell - want) < 1.0E-6);
    }
}
