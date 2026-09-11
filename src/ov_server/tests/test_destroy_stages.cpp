#include "../src/destroy_stages.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

constexpr net::WirePosition kStone{1, -60, 3};

/// Run `ticks` server ticks of a dig at `rate`, then stop it; every stage
/// that would go out, in order.
[[nodiscard]] std::vector<i32> replay(f32 rate, i32 ticks, bool stop = true) {
    DestroyStageState state;
    std::vector<i32>  sent;
    for (i32 elapsed = 1; elapsed <= ticks; ++elapsed) {
        if (const auto stage = next_destroy_stage(state, 1, true, kStone,
                                                  rate * static_cast<f32>(elapsed))) {
            CHECK(stage->entity_id == 1);
            CHECK(stage->position == kStone);
            sent.push_back(stage->stage);
        }
    }
    if (stop) {
        if (const auto stage = next_destroy_stage(state, 1, false, kStone, 0.0F)) {
            sent.push_back(stage->stage);
        }
        // Once said, -1 is not repeated.
        CHECK_FALSE(next_destroy_stage(state, 1, false, kStone, 0.0F).has_value());
    }
    return sent;
}

}  // namespace

// The real server's own sequences, read off scripts/capture_destroy_stage.py
// (docs/provenance/cassage-bloc.md): stone held by hand and aborted before
// stage 13 was due went 0, 1, ... 12, then -1 — one stage every fifteen ticks.
TEST_CASE("stone by hand: one stage per fifteen ticks, then -1 on the abort", "[breaking]") {
    const auto sent = replay(1.0F / 1.5F / 100.0F, 190);
    const std::vector<i32> expected{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -1};
    CHECK(sent == expected);
}

TEST_CASE("a late claim runs the stage past nine", "[breaking]") {
    // Dirt by hand, 1/15 a tick, claimed at 20 ticks: the capture went to 14.
    const auto sent = replay(1.0F / 0.5F / 30.0F, 21);
    REQUIRE(sent.size() >= 2);
    CHECK(sent[sent.size() - 2] == 14);
    CHECK(sent.back() == -1);
}

TEST_CASE("a dig the server finished on its own clock ends without -1", "[breaking]") {
    DestroyStageState state;
    std::vector<i32>  sent;
    const f32         rate = 1.0F / 2.0F / 30.0F;  // planks by hand: 60 ticks
    for (i32 elapsed = 1; elapsed <= 60; ++elapsed) {
        if (const auto stage = next_destroy_stage(state, 1, true, kStone,
                                                  rate * static_cast<f32>(elapsed))) {
            sent.push_back(stage->stage);
        }
    }
    forget_destroy_stage(state);
    CHECK_FALSE(next_destroy_stage(state, 1, false, kStone, 0.0F).has_value());
    // The capture's last stage before World Event 2001 was 10.
    REQUIRE_FALSE(sent.empty());
    CHECK(sent.back() == 10);
}

TEST_CASE("a new block moves the crack even at the same stage", "[breaking]") {
    DestroyStageState state;
    REQUIRE(next_destroy_stage(state, 7, true, kStone, 0.01F).has_value());
    CHECK_FALSE(next_destroy_stage(state, 7, true, kStone, 0.02F).has_value());
    const auto moved = next_destroy_stage(state, 7, true, net::WirePosition{2, -60, 3}, 0.01F);
    REQUIRE(moved.has_value());
    CHECK(moved->stage == 0);
    CHECK(moved->position == net::WirePosition{2, -60, 3});
}
