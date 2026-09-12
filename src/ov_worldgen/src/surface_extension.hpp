// ── worldgen-3 ── The two passes of the surface stage that are not rules:
// the pillars of the eroded badlands and the icebergs of the frozen oceans.
//
// The game runs them from the same column loop as the rules, one before and
// one after: an eroded badlands column is raised into a pillar of stone before
// the rules see it (so the rules paint the pillar with terracotta), and a
// frozen ocean column has its ice stacked on afterwards (so the rules never
// see the ice). Private to the module: SurfaceSystem owns one of these.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/noise.hpp"

#include <span>

namespace ov::worldgen {

struct SurfaceExtension {
    const registry::BlockRegistry* blocks{nullptr};

    const NormalNoise* badlands_surface{nullptr};
    const NormalNoise* badlands_pillar{nullptr};
    const NormalNoise* badlands_pillar_roof{nullptr};
    const NormalNoise* iceberg_surface{nullptr};
    const NormalNoise* iceberg_pillar{nullptr};
    const NormalNoise* iceberg_pillar_roof{nullptr};

    /// The world's positional generator — the one the per-column surface
    /// depth jitter is drawn from. Null for a legacy dimension, which has
    /// neither biome.
    const math::XoroshiroPositionalFactory* random{nullptr};

    registry::BlockStateId default_block{};
    registry::BlockStateId packed_ice{};
    registry::BlockStateId snow_block{};
    registry::BlockId      water{0};

    i32 min_y{-64};
    i32 sea_level{63};
    /// An instrument (`OV_SURFACE_PASS_SHIFT`): the noises and the column
    /// random are read this many blocks east of the column — the shifted
    /// control of the measurement. Zero in any world.
    i32 shift{0};

    [[nodiscard]] bool ready() const noexcept {
        return blocks != nullptr && badlands_surface != nullptr && badlands_pillar != nullptr &&
               badlands_pillar_roof != nullptr && iceberg_surface != nullptr &&
               iceberg_pillar != nullptr && iceberg_pillar_roof != nullptr && random != nullptr;
    }

    /// Raise a pillar of the default block over an eroded badlands column.
    /// `height` is one above the column's top block; `column[0]` is `min_y`.
    void eroded_badlands(std::span<registry::BlockStateId> column, i32 x, i32 z, i32 height) const;

    /// Stack packed ice (and a snow cap) over a frozen ocean column.
    /// `base_temperature` is the biome's, before the frozen modifier.
    void frozen_ocean(std::span<registry::BlockStateId> column, i32 x, i32 z, i32 height,
                      i32 min_surface_level, f32 base_temperature) const;
};

}  // namespace ov::worldgen
