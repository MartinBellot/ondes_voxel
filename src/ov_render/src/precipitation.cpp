#include "ov/render/precipitation.hpp"

#include "ov/math/random.hpp"

#include <algorithm>
#include <cmath>

namespace ov::render {

namespace {

/// Integer arithmetic that wraps, as the game's `int` does. Done in u32 so
/// that an overflow is defined here too.
[[nodiscard]] i32 wrap(u32 value) noexcept { return static_cast<i32>(value); }

[[nodiscard]] u32 u(i32 value) noexcept { return static_cast<u32>(value); }

/// The per-column number both the sheet's random stream and the rain's
/// phase are built from.
[[nodiscard]] u32 column_hash_a(i32 x) noexcept { return u(x) * u(x) * 3121U + u(x) * 45238971U; }
[[nodiscard]] u32 column_hash_b(i32 z) noexcept { return u(z) * u(z) * 418711U + u(z) * 13761U; }

[[nodiscard]] std::array<u8, 4> colour(const std::array<u8, 3>& light, f32 alpha) noexcept {
    const f32 a = std::clamp(alpha, 0.0F, 1.0F);
    return {light[0], light[1], light[2], static_cast<u8>(a * 255.0F + 0.5F)};
}

void sheet(std::vector<EntityVertex>& out, f64 cx, f64 cz, f64 half_x, f64 half_z, f64 low,
           f64 high, f32 u0, f32 u1, f32 v_top, f32 v_bottom, std::array<u8, 4> rgba) {
    const auto at = [](f64 value) { return static_cast<f32>(value); };
    out.push_back(EntityVertex{at(cx - half_x), at(high), at(cz - half_z), u0, v_top, rgba});
    out.push_back(EntityVertex{at(cx + half_x), at(high), at(cz + half_z), u1, v_top, rgba});
    out.push_back(EntityVertex{at(cx + half_x), at(low), at(cz + half_z), u1, v_bottom, rgba});
    out.push_back(EntityVertex{at(cx - half_x), at(low), at(cz - half_z), u0, v_bottom, rgba});
}

}  // namespace

void weather_columns(const WeatherView& view, std::vector<WeatherColumn>& out) {
    out.clear();
    const auto cx = static_cast<i32>(std::floor(view.camera.x));
    const auto cz = static_cast<i32>(std::floor(view.camera.z));
    for (i32 z = cz - view.radius; z <= cz + view.radius; ++z) {
        for (i32 x = cx - view.radius; x <= cx + view.radius; ++x) {
            WeatherColumn column;
            column.x = x;
            column.z = z;
            out.push_back(column);
        }
    }
}

void build_weather(const WeatherView& view, std::span<const WeatherColumn> columns,
                   WeatherMesh& out) {
    out.clear();
    if (view.rain_level <= 0.0F || view.radius <= 0) {
        return;
    }
    const auto cx     = static_cast<i32>(std::floor(view.camera.x));
    const auto cy     = static_cast<i32>(std::floor(view.camera.y));
    const auto cz     = static_cast<i32>(std::floor(view.camera.z));
    const i32  radius = view.radius;
    const f32  time   = static_cast<f32>(view.ticks) + view.partial;

    for (const WeatherColumn& column : columns) {
        if (column.kind == WeatherKind::None) {
            continue;
        }
        const i32 low  = std::max(cy - radius, column.top);
        const i32 high = std::max(cy + radius, column.top);
        if (low == high) {
            continue;  // the ground is above the whole square: nothing to draw
        }

        // Across the line to the camera, half a block each way. Taken from the
        // integer offset of the column, so a sheet does not swivel as the
        // camera moves inside its own block; the column under the camera has
        // no direction and no sheet.
        const auto ox     = static_cast<f32>(column.x - cx);
        const auto oz     = static_cast<f32>(column.z - cz);
        const f32  length = std::sqrt(ox * ox + oz * oz);
        if (length == 0.0F) {
            continue;
        }
        const f64 half_x = static_cast<f64>(-oz / length) * 0.5;
        const f64 half_z = static_cast<f64>(ox / length) * 0.5;

        const u32 a    = column_hash_a(column.x);
        const u32 b    = column_hash_b(column.z);
        const i64 seed = static_cast<i64>(wrap(a ^ b));
        math::LegacyRandomSource random{seed};

        const f64 dx       = static_cast<f64>(column.x) + 0.5 - view.camera.x;
        const f64 dz       = static_cast<f64>(column.z) + 0.5 - view.camera.z;
        const f32 distance = static_cast<f32>(std::sqrt(dx * dx + dz * dz)) / static_cast<f32>(radius);
        const f64 centre_x = static_cast<f64>(column.x) + 0.5;
        const f64 centre_z = static_cast<f64>(column.z) + 0.5;
        const auto v_low   = static_cast<f32>(low) * 0.25F;
        const auto v_high  = static_cast<f32>(high) * 0.25F;

        if (column.kind == WeatherKind::Rain) {
            const i32 phase =
                wrap(u(static_cast<i32>(view.ticks)) + a + b) & 31;
            const f32 speed  = 3.0F + random.next_float();
            const f32 scroll = -(static_cast<f32>(phase) + view.partial) / 32.0F * speed;
            const f32 alpha  = ((1.0F - distance * distance) * 0.5F + 0.5F) * view.rain_level;
            sheet(out.rain, centre_x, centre_z, half_x, half_z, static_cast<f64>(low),
                  static_cast<f64>(high), 0.0F, 1.0F, v_low + scroll, v_high + scroll,
                  colour(column.light, alpha));
        } else {
            // Snow falls slowly and drifts. The draws in their order, named.
            const f32 scroll  = -(static_cast<f32>(view.ticks & 511) + view.partial) / 512.0F;
            const f64 drift_a = random.next_double();
            const f64 drift_b = random.next_gaussian();
            const auto sideways =
                static_cast<f32>(drift_a + static_cast<f64>(time) * 0.01 * static_cast<f64>(static_cast<f32>(drift_b)));
            const f64 wobble_a = random.next_double();
            const f64 wobble_b = random.next_gaussian();
            const auto wobble  = static_cast<f32>(
                wobble_a + static_cast<f64>(time * static_cast<f32>(wobble_b)) * 0.001);
            const f32 alpha = ((1.0F - distance * distance) * 0.3F + 0.5F) * view.rain_level;
            // Snow is drawn brighter than the light it stands in: three
            // quarters of the way from the light to full.
            std::array<u8, 3> lit{};
            for (usize i = 0; i < 3; ++i) {
                lit[i] = static_cast<u8>((static_cast<u32>(column.light[i]) * 3U + 255U) / 4U);
            }
            sheet(out.snow, centre_x, centre_z, half_x, half_z, static_cast<f64>(low),
                  static_cast<f64>(high), sideways, 1.0F + sideways, v_low + scroll + wobble,
                  v_high + scroll + wobble, colour(lit, alpha));
        }
        ++out.columns;
    }
}

}  // namespace ov::render
