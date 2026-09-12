#define OV_LOG_CATEGORY "worldgen"

// ── worldgen-3 ── The eroded badlands' pillars and the frozen oceans'
// icebergs, as surface passes. See surface_extension.hpp for where they sit in
// the column loop, and docs/provenance/surface-rules.md § 10 for what was
// measured.

#include "surface_extension.hpp"

#include "climate_noise.hpp"

#include <algorithm>
#include <cmath>

namespace ov::worldgen {

namespace {

/// The column slot for a height, or null when the height is outside it.
[[nodiscard]] registry::BlockStateId* slot(std::span<registry::BlockStateId> column, i32 min_y,
                                           i32 y) noexcept {
    const i32 index = y - min_y;
    if (index < 0 || index >= static_cast<i32>(column.size())) {
        return nullptr;
    }
    return &column[static_cast<usize>(index)];
}

}  // namespace

void SurfaceExtension::eroded_badlands(std::span<registry::BlockStateId> column, i32 x, i32 z,
                                       i32 height) const {
    const auto fx = static_cast<f64>(x + shift);
    const auto fz = static_cast<f64>(z);
    const f64  pillar =
        std::min(std::abs(badlands_surface->value(fx, 0.0, fz) * 8.25),
                 badlands_pillar->value(fx * 0.2, 0.0, fz * 0.2) * 15.0);
    if (pillar <= 0.0) {
        return;
    }
    const f64 roof = std::abs(badlands_pillar_roof->value(fx * 0.75, 0.0, fz * 0.75) * 1.5);
    const f64 top  = 64.0 + std::min(pillar * pillar * 2.5, std::ceil(roof * 50.0) + 24.0);
    const auto peak = static_cast<i32>(std::floor(top));
    if (height > peak) {
        return;
    }
    const auto stone = blocks->block_of(default_block);
    // Down to the stone: a pillar is never raised out of water.
    for (i32 y = peak; y >= min_y; --y) {
        const auto* here = slot(column, min_y, y);
        if (here == nullptr) {
            continue;
        }
        const auto block = blocks->block_of(*here);
        if (block == stone) {
            break;
        }
        if (block == water) {
            return;
        }
    }
    for (i32 y = peak; y >= min_y; --y) {
        auto* here = slot(column, min_y, y);
        if (here == nullptr) {
            continue;
        }
        if (*here != registry::kAirState && !blocks->is_air(blocks->block_of(*here))) {
            break;
        }
        *here = default_block;
    }
}

void SurfaceExtension::frozen_ocean(std::span<registry::BlockStateId> column, i32 x, i32 z,
                                    i32 height, i32 min_surface_level, f32 base_temperature) const {
    const auto fx = static_cast<f64>(x + shift);
    const auto fz = static_cast<f64>(z);
    const f64  pillar =
        std::min(std::abs(iceberg_surface->value(fx, 0.0, fz) * 8.25),
                 iceberg_pillar->value(fx * 1.28, 0.0, fz * 1.28) * 15.0);
    if (pillar <= 1.8) {
        return;
    }
    const f64 roof = std::abs(iceberg_pillar_roof->value(fx * 1.17, 0.0, fz * 1.17) * 1.5);
    f64       top  = std::min(pillar * pillar * 1.2, std::ceil(roof * 40.0) + 14.0);
    // A frozen ocean warm enough to melt its bergs "slightly" loses two blocks
    // off each one. Asked at the sea's surface, with the frozen modifier.
    if (height_adjusted_temperature(base_temperature, TemperatureModifier::Frozen, x, 63, z) >
        0.1F) {
        top -= 2.0;
    }
    f64 bottom = 0.0;
    if (top > 2.0) {
        bottom = static_cast<f64>(sea_level) - top - 7.0;
        top += static_cast<f64>(sea_level);
    } else {
        top = 0.0;
    }

    auto      draw        = random->at(x + shift, 0, z);
    const i32 snow_blocks = 2 + draw.next_int(4);
    const i32 snow_above  = sea_level + 18 + draw.next_int(10);
    i32       snowed      = 0;
    const auto top_y      = static_cast<i32>(top);
    const auto bottom_y   = static_cast<i32>(bottom);
    for (i32 y = std::max(height, top_y + 1); y >= min_surface_level; --y) {
        auto* here = slot(column, min_y, y);
        if (here == nullptr) {
            continue;
        }
        const auto block = blocks->block_of(*here);
        const bool air   = *here == registry::kAirState || blocks->is_air(block);
        // Both halves draw only when reached: `||` and `&&` short-circuit.
        const bool into_air   = air && y < top_y && draw.next_double() > 0.01;
        const bool into_water = !into_air && block == water && y > bottom_y && y < sea_level &&
                                bottom != 0.0 && draw.next_double() > 0.15;
        if (!into_air && !into_water) {
            continue;
        }
        if (snowed <= snow_blocks && y > snow_above) {
            *here = snow_block;
            ++snowed;
        } else {
            *here = packed_ice;
        }
    }
}

}  // namespace ov::worldgen
