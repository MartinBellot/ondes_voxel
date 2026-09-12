// The client's copy of the scoreboard, fed the packets a real 1.20.1 server
// sent a newcomer (scripts/capture_scoreboard.py, "arrival"): what the
// sidebar would show, and the belowName value.
#include "ov/client/scoreboard_view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ov;
using namespace ov::net::scoreboard;

namespace {

TeamUpdate red_team() {
    TeamUpdate red;
    red.name                          = "red";
    red.mode                          = TeamMode::Create;
    red.parameters.display_json       = R"({"color":"dark_red","text":"Rouge"})";
    red.parameters.color              = 12;
    red.parameters.prefix_json        = R"({"text":"[R] "})";
    red.parameters.suffix_json        = R"({"text":" !"})";
    red.entities                      = {"ovprobe"};
    return red;
}

}  // namespace

TEST_CASE("the sidebar shows what the slots and scores say", "[client][scoreboard]") {
    client::ScoreboardView view;
    // The capture's arrival, in order.
    view.apply(red_team());
    view.apply(ObjectiveUpdate{"hp", ObjectiveMode::Create, R"({"text":"hp"})", 1});
    view.apply(DisplayObjective{kSlotList, "hp"});
    view.apply(ObjectiveUpdate{"d", ObjectiveMode::Create, R"({"text":"d"})", 0});
    view.apply(DisplayObjective{kSlotSidebar, "d"});
    view.apply(DisplayObjective{15, "d"});
    view.apply(ScoreUpdate{"a", ScoreAction::Change, "d", std::numeric_limits<i32>::min()});
    view.apply(ScoreUpdate{"nobody", ScoreAction::Change, "d", 0});
    view.apply(ScoreUpdate{"b", ScoreAction::Change, "d", 0});
    view.apply(ScoreUpdate{"a,b", ScoreAction::Change, "d", 0});
    view.apply(ScoreUpdate{"#hidden", ScoreAction::Change, "d", 3});
    view.apply(ScoreUpdate{"ovprobe", ScoreAction::Change, "d", 12});
    view.apply(ObjectiveUpdate{"d2", ObjectiveMode::Create, R"({"text":"Deux"})", 0});
    view.apply(DisplayObjective{kSlotBelowName, "d2"});
    view.apply(ScoreUpdate{"ovprobe", ScoreAction::Change, "d2", 1});

    CHECK(view.sidebar_objective() == "d");
    // Highest first, ties by name, `#hidden` left out.
    using Line = client::ScoreboardView::Line;
    CHECK(view.sidebar_lines() == std::vector<Line>{{"ovprobe", 12},
                                                    {"a,b", 0},
                                                    {"b", 0},
                                                    {"nobody", 0},
                                                    {"a", std::numeric_limits<i32>::min()}});
    CHECK(view.below_name_score("ovprobe") == 1);
    CHECK_FALSE(view.below_name_score("b").has_value());

    // On red, the red team's sidebar wins (here the same objective).
    view.set_own_name("ovprobe");
    CHECK(view.sidebar_objective() == "d");
    view.apply(DisplayObjective{15, ""});
    CHECK(view.sidebar_objective() == "d");  // back to the plain sidebar

    // A holder reset everywhere, then the objective removed.
    view.apply(ScoreUpdate{"ovprobe", ScoreAction::Remove, "", 0});
    CHECK(view.sidebar_lines().front().holder == "a,b");
    view.apply(ObjectiveUpdate{"d", ObjectiveMode::Remove, {}, 0});
    CHECK(view.sidebar_objective().empty());
    CHECK(view.sidebar_lines().empty());
}

TEST_CASE("fifteen lines at most", "[client][scoreboard]") {
    client::ScoreboardView view;
    view.apply(ObjectiveUpdate{"d", ObjectiveMode::Create, R"({"text":"d"})", 0});
    view.apply(DisplayObjective{kSlotSidebar, "d"});
    for (i32 i = 0; i < 20; ++i) {
        view.apply(ScoreUpdate{"p" + std::to_string(i), ScoreAction::Change, "d", i});
    }
    const auto lines = view.sidebar_lines();
    REQUIRE(lines.size() == 15);
    CHECK(lines.front().score == 19);
    CHECK(lines.back().score == 5);
}
