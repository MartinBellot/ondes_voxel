#include "ov/client/menu_layouts.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::client;

// The seed rule of Create World, and the geometry of the menus. The layout
// numbers below are the ones the running 1.20.1 client reported at 854×480
// GUI pixels (window 1280×720, GUI scale 3) — scripts/measure_screens.py,
// docs/provenance/ecrans.md.

TEST_CASE("Java's String.hashCode over UTF-16 units", "[menus]") {
    // Reference values from a UTF-16 hash computed independently.
    CHECK(java_string_hash("") == 0);
    CHECK(java_string_hash("hello") == 99162322);
    CHECK(java_string_hash("abc") == 96354);
    CHECK(java_string_hash("\xC3\xA9t\xC3\xA9") == 227742);          // été
    CHECK(java_string_hash("Ondes VOXEL") == -811816711);
    CHECK(java_string_hash("\xF0\x9F\x98\x80") == 1772899);          // a surrogate pair
}

TEST_CASE("a seed is a number when it reads as one, else the text's hash", "[menus]") {
    CHECK_FALSE(seed_from_text(""));
    CHECK_FALSE(seed_from_text("   "));
    CHECK(seed_from_text("1234567890") == 1234567890);
    CHECK(seed_from_text("-5") == -5);
    CHECK(seed_from_text(" 42 ") == 42);
    CHECK(seed_from_text("hello") == 99162322);
    // One past Long.MAX_VALUE does not parse as a long: it is hashed.
    CHECK(seed_from_text("9223372036854775808") == -1773151197);
}

namespace {

[[nodiscard]] const Widget* by_id(const std::vector<Widget>& widgets, std::string_view id) {
    for (const Widget& widget : widgets) {
        if (widget.id == id) {
            return &widget;
        }
    }
    return nullptr;
}

void check_rect(const std::vector<Widget>& widgets, std::string_view id, f32 x, f32 y, f32 w,
                f32 h) {
    const Widget* widget = by_id(widgets, id);
    INFO(std::string(id));
    REQUIRE(widget != nullptr);
    CHECK(widget->rect.x == x);
    CHECK(widget->rect.y == y);
    CHECK(widget->rect.w == w);
    CHECK(widget->rect.h == h);
}

}  // namespace

TEST_CASE("a click lands on the widget drawn under it, never on a label", "[menus]") {
    std::vector<Widget> widgets = title_layout(854.0F, 480.0F);
    const Widget*       hit     = widget_at(widgets, 327.0F + 100.0F, 168.0F + 10.0F);
    REQUIRE(hit != nullptr);
    CHECK(hit->id == "singleplayer");
    // One pixel past the right edge and one above: nothing.
    CHECK(widget_at(widgets, 527.0F, 170.0F) == nullptr);
    CHECK(widget_at(widgets, 400.0F, 167.0F) == nullptr);
    // The image buttons are hit like buttons.
    const Widget* globe = widget_at(widgets, 310.0F, 260.0F);
    REQUIRE(globe != nullptr);
    CHECK(globe->id == "language");
    Widget label;
    label.kind = WidgetKind::Label;
    label.rect = Rect{0.0F, 0.0F, 100.0F, 100.0F};
    const std::vector<Widget> only_label{label};
    CHECK(widget_at(only_label, 50.0F, 50.0F) == nullptr);
}

TEST_CASE("a slider's handle centre follows the pointer, clamped to the track", "[menus]") {
    const Rect track{100.0F, 0.0F, 150.0F, 20.0F};
    CHECK(slider_value_at(track, 104.0F) == 0.0);
    CHECK(slider_value_at(track, 50.0F) == 0.0);
    CHECK(slider_value_at(track, 246.0F) == 1.0);
    CHECK(slider_value_at(track, 400.0F) == 1.0);
    CHECK(slider_value_at(track, 175.0F) == 0.5);
}

TEST_CASE("the death screen's buttons are where vanilla puts them", "[menus]") {
    const auto widgets = death_layout(854.0F, 480.0F);
    check_rect(widgets, "respawn", 327.0F, 192.0F, 200.0F, 20.0F);
    check_rect(widgets, "title", 327.0F, 216.0F, 200.0F, 20.0F);
}

TEST_CASE("the title screen's buttons are where vanilla puts them", "[menus]") {
    const auto widgets = title_layout(854.0F, 480.0F);
    check_rect(widgets, "singleplayer", 327.0F, 168.0F, 200.0F, 20.0F);
    check_rect(widgets, "multiplayer", 327.0F, 192.0F, 200.0F, 20.0F);
    check_rect(widgets, "realms", 327.0F, 216.0F, 200.0F, 20.0F);
    check_rect(widgets, "options", 327.0F, 252.0F, 98.0F, 20.0F);
    check_rect(widgets, "quit", 429.0F, 252.0F, 98.0F, 20.0F);
    check_rect(widgets, "language", 303.0F, 252.0F, 20.0F, 20.0F);
    check_rect(widgets, "accessibility", 531.0F, 252.0F, 20.0F, 20.0F);
}
