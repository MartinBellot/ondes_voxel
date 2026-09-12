#include "buried_treasure.hpp"

#include "ov/world/heightmap.hpp"

#include <algorithm>

namespace ov::worldgen {

i32 buried_treasure_height(const FeatureLevel&                    level,
                           std::span<const registry::BlockStateId> support, i32 x, i32 z) {
    const auto supports = [&](registry::BlockStateId state) {
        return std::ranges::find(support, state) != support.end();
    };
    for (i32 y = level.height(world::HeightmapType::OceanFloorWG, x, z); y > level.min_y(); --y) {
        if (supports(level.block_at(x, y - 1, z))) {
            return y;
        }
    }
    return level.min_y();
}

}  // namespace ov::worldgen
