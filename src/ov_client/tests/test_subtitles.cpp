// The subtitle lines: which, in what order, with which arrow, how faded.
#include "ov/client/subtitles.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using client::SubtitleOverlay;

TEST_CASE("subtitles: newest at the bottom, a repeat refreshes its line", "[subtitles]") {
    SubtitleOverlay overlay;
    overlay.heard("subtitles.block.generic.footsteps", Vec3d{0, 64, 3}, false);
    overlay.advance(0.5);
    overlay.heard("subtitles.entity.zombie.ambient", Vec3d{0, 64, 5}, false);
    overlay.advance(0.5);
    overlay.heard("subtitles.block.generic.footsteps", Vec3d{0, 64, 2}, false);

    const audio::Listener head{Vec3d{0, 64, 0}, 0.0F, 0.0F};
    const auto            lines = overlay.lines(head);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].key == "subtitles.entity.zombie.ambient");
    CHECK(lines[1].key == "subtitles.block.generic.footsteps");
    CHECK(lines[1].freshness == 1.0F);
    CHECK(lines[0].freshness < 1.0F);
    CHECK(lines[0].freshness > 0.8F);
}

TEST_CASE("subtitles: an arrow points to a sound off to the side, none ahead", "[subtitles]") {
    SubtitleOverlay overlay;
    // Yaw 0 faces +Z: -X is the right hand.
    overlay.heard("right", Vec3d{-6, 64, 1}, false);
    overlay.heard("left", Vec3d{6, 64, 1}, false);
    overlay.heard("ahead", Vec3d{1, 64, 8}, false);
    overlay.heard("interface", Vec3d{50, 64, 0}, true);
    const auto lines = overlay.lines(audio::Listener{Vec3d{0, 64, 0}, 0.0F, 30.0F});
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].arrow == 1);
    CHECK(lines[1].arrow == -1);
    CHECK(lines[2].arrow == 0);
    CHECK(lines[3].arrow == 0);  // relative: no direction
}

TEST_CASE("subtitles: a line leaves after its display time, scaled by the option",
          "[subtitles]") {
    SubtitleOverlay overlay;
    overlay.heard("a", Vec3d{}, true);
    overlay.advance(SubtitleOverlay::kDisplaySeconds - 0.01);
    overlay.prune();
    CHECK(overlay.size() == 1);
    overlay.advance(0.02);
    overlay.prune();
    CHECK(overlay.size() == 0);

    overlay.set_display_time(2.0);  // notificationDisplayTime 2.0
    overlay.heard("b", Vec3d{}, true);
    overlay.advance(SubtitleOverlay::kDisplaySeconds * 1.5);
    overlay.prune();
    CHECK(overlay.size() == 1);
}
