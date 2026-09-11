// Rain and snow, as the client draws them.
//
// The game does not draw particles for falling rain. It draws, for every
// column in a square around the camera, one textured sheet from the ground —
// the top of MOTION_BLOCKING — up to a fixed distance above the eye, turned
// across the line from the camera to the column. The texture scrolls down the
// sheet: fast and in a different phase per column for rain, slowly and with a
// sideways drift for snow. A column under a roof has its sheet end at the roof,
// which is what keeps a house dry inside.
//
// Nothing here knows what a biome or a chunk is. The caller fills one
// `WeatherColumn` per column — its surface, whether rain or snow falls there,
// the light — and this file turns that into vertices for the entity pipeline's
// translucent pass. That split is what lets test_ov_render assert the sheets
// without a world.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/entity_mesh.hpp"

#include <array>
#include <span>
#include <vector>

namespace ov::render {

enum class WeatherKind : u8 { None, Rain, Snow };

/// One column the precipitation pass asks about.
struct WeatherColumn {
    i32 x{0};
    i32 z{0};
    /// The first free block above MOTION_BLOCKING: where the sheet stops.
    i32         top{0};
    WeatherKind kind{WeatherKind::None};
    /// The light the sheet is drawn in, already through the lightmap.
    std::array<u8, 3> light{255, 255, 255};
};

struct WeatherView {
    Vec3d camera{};
    /// A tick counter and the fraction of the next tick: the scroll.
    i64 ticks{0};
    f32 partial{0.0F};
    /// The rain level, 0..1. Every sheet's alpha is scaled by it, so rain
    /// fades in over the hundred ticks the level takes to climb.
    f32 rain_level{0.0F};
    /// Columns out from the camera: 10 with fancy graphics, 5 with fast.
    i32 radius{10};
};

inline constexpr i32 kWeatherRadiusFancy = 10;
inline constexpr i32 kWeatherRadiusFast  = 5;

struct WeatherMesh {
    std::vector<EntityVertex> rain;
    std::vector<EntityVertex> snow;
    u32                       columns{0};

    void clear() noexcept {
        rain.clear();
        snow.clear();
        columns = 0;
    }
};

/// The columns a frame asks about, (2r+1)² of them, z outer and x inner,
/// with only x and z filled: the caller fills the rest.
void weather_columns(const WeatherView& view, std::vector<WeatherColumn>& out);

/// One sheet per column that has something falling on it and room for a
/// sheet between the ground and the top of the square. Allocates nothing once
/// the two vectors have grown to their working size.
void build_weather(const WeatherView& view, std::span<const WeatherColumn> columns,
                   WeatherMesh& out);

}  // namespace ov::render
