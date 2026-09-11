// The rain and snow sheets, and the colours the weather gives the fog and sky.
#include "ov/render/environment.hpp"
#include "ov/render/precipitation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace {

[[nodiscard]] std::vector<WeatherColumn> flat(const WeatherView& view, i32 top, WeatherKind kind) {
    std::vector<WeatherColumn> columns;
    weather_columns(view, columns);
    for (WeatherColumn& column : columns) {
        column.top  = top;
        column.kind = kind;
    }
    return columns;
}

}  // namespace

TEST_CASE("the square of columns is (2r+1)², z outer and x inner") {
    WeatherView view;
    view.camera = {10.5, 70.0, -3.2};
    view.radius = kWeatherRadiusFancy;
    std::vector<WeatherColumn> columns;
    weather_columns(view, columns);
    REQUIRE(columns.size() == 21U * 21U);
    CHECK(columns.front().x == 0);
    CHECK(columns.front().z == -14);
    CHECK(columns[1].x == 1);
    CHECK(columns.back().x == 20);
    CHECK(columns.back().z == 6);
}

TEST_CASE("one sheet a column, from the ground to ten above the eye") {
    WeatherView view;
    view.camera     = {0.5, 64.0, 0.5};
    view.rain_level = 1.0F;
    WeatherMesh mesh;
    build_weather(view, flat(view, 60, WeatherKind::Rain), mesh);
    // Every column but the one under the camera, which has no direction.
    CHECK(mesh.columns == 21U * 21U - 1U);
    CHECK(mesh.rain.size() == mesh.columns * 4U);
    CHECK(mesh.snow.empty());
    f32 low  = 1e9F;
    f32 high = -1e9F;
    for (const EntityVertex& v : mesh.rain) {
        low  = std::min(low, v.y);
        high = std::max(high, v.y);
    }
    CHECK(low == 60.0F);   // the ground is above the bottom of the square
    CHECK(high == 74.0F);  // ten above the eye's block

    // Alpha fades from the middle out — to nothing at the corners of the
    // square, which are further than the radius — and scales with the level.
    const auto alpha_at = [](const WeatherMesh& m, usize quad) { return m.rain[quad * 4].colour[3]; };
    const auto brightest = [&](const WeatherMesh& m) {
        u8 best = 0;
        for (usize q = 0; q < m.columns; ++q) {
            best = std::max(best, alpha_at(m, q));
        }
        return best;
    };
    CHECK(alpha_at(mesh, 0) == 0);
    CHECK(brightest(mesh) > 200);
    WeatherMesh half;
    view.rain_level = 0.5F;
    build_weather(view, flat(view, 60, WeatherKind::Rain), half);
    CHECK(brightest(half) < brightest(mesh));
}

TEST_CASE("a roof stops the sheet, and no rain draws nothing") {
    WeatherView view;
    view.camera     = {0.5, 64.0, 0.5};
    view.rain_level = 1.0F;
    WeatherMesh mesh;
    // The ground ten or more above the eye: nothing to draw under it.
    build_weather(view, flat(view, 74, WeatherKind::Rain), mesh);
    CHECK(mesh.columns == 0);
    build_weather(view, flat(view, 70, WeatherKind::Rain), mesh);
    CHECK(mesh.columns == 21U * 21U - 1U);
    view.rain_level = 0.0F;
    build_weather(view, flat(view, 60, WeatherKind::Rain), mesh);
    CHECK(mesh.columns == 0);
    view.rain_level = 1.0F;
    build_weather(view, flat(view, 60, WeatherKind::None), mesh);
    CHECK(mesh.columns == 0);
}

TEST_CASE("snow drifts, is brighter than its light, and is the same frame to frame") {
    WeatherView view;
    view.camera     = {0.5, 64.0, 0.5};
    view.rain_level = 1.0F;
    view.ticks      = 1234;
    auto columns    = flat(view, 60, WeatherKind::Snow);
    for (WeatherColumn& column : columns) {
        column.light = {100, 100, 100};
    }
    WeatherMesh a;
    WeatherMesh b;
    build_weather(view, columns, a);
    build_weather(view, columns, b);
    REQUIRE(a.snow.size() == b.snow.size());
    CHECK(a.snow[5].u == b.snow[5].u);
    CHECK(a.snow[0].colour[0] > 100);
    CHECK(a.rain.empty());
}

TEST_CASE("rain and thunder dim the fog, and a flash lights the sky") {
    const u32 fog = 0xC0D8FF;
    CHECK(weather_fog_colour(fog, 0.0F, 0.0F) == fog);
    const u32 wet = weather_fog_colour(fog, 1.0F, 0.0F);
    CHECK(((wet >> 16) & 0xFF) == 96);   // 192 halved
    CHECK((wet & 0xFF) == 153);          // 255 times 0.6
    const u32 storm = weather_fog_colour(fog, 1.0F, 1.0F);
    CHECK(((storm >> 16) & 0xFF) == 48);
    const u32 sky = 0x78A7FF;
    CHECK(weather_sky_colour(sky, 0.0F, 0.0F, 0.0F) == sky);
    const u32 grey = weather_sky_colour(sky, 1.0F, 0.0F, 0.0F);
    CHECK((grey & 0xFF) < (sky & 0xFF));
    const u32 flash = weather_sky_colour(0x000000, 0.0F, 0.0F, 1.0F);
    CHECK((flash & 0xFF) == 115);  // 0.45 of the way to white-blue
}
