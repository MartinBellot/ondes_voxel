#define OV_LOG_CATEGORY "worldgen"

#include "ruined_portal.hpp"

#include <array>

namespace ov::worldgen {

namespace {

/// A draw in [`low`, `high`].
[[nodiscard]] i32 inclusive(math::LegacyRandomSource& random, i32 low, i32 high) {
    return random.next_int(high - low + 1) + low;
}

/// A draw in [`low`, `high`] when that holds more than one value; `high`
/// otherwise, and then nothing is drawn. A portal under a low surface does not
/// draw, which is where the next draw of its start sits.
[[nodiscard]] i32 within(math::LegacyRandomSource& random, i32 low, i32 high) {
    return low < high ? inclusive(random, low, high) : high;
}

/// The middle column of a box, as the game rounds it: the lower half's last
/// block plus one, which is `min + size / 2`.
[[nodiscard]] i32 middle(i32 low, i32 high) {
    return low + (high - low + 1) / 2;
}

}  // namespace

std::expected<i32, std::string> ruined_portal_height(const StructureWorldSampler& sampler,
                                                     std::string_view placement, bool air_pocket,
                                                     const BoundingBox& box, i32 min_y,
                                                     math::LegacyRandomSource& random) {
    const bool ocean  = placement == "on_ocean_floor";
    const i32  height = box.max_y - box.min_y + 1;
    // The first *occupied* block of the middle column — one under the first
    // free one. The sea bed for a portal under the sea, the top of the water
    // or the ground otherwise.
    const auto surface = [&] {
        const i32 x = middle(box.min_x, box.max_x);
        const i32 z = middle(box.min_z, box.max_z);
        return (ocean ? sampler.ocean_floor_height(x, z) : sampler.surface_height(x, z)) - 1;
    };

    const i32 floor = min_y + kPortalFloorAboveBottom;
    i32       y     = 0;
    if (placement == "in_nether") {
        if (air_pocket) {
            y = inclusive(random, 32, 100);
        } else if (random.next_float() < 0.5F) {
            y = inclusive(random, 27, 29);
        } else {
            y = inclusive(random, 29, 100);
        }
    } else if (placement == "in_mountain") {
        y = within(random, 70, surface() - height);
    } else if (placement == "underground") {
        y = within(random, floor, surface() - height);
    } else if (placement == "partly_buried") {
        y = surface() - height + inclusive(random, 2, 8);
    } else if (placement == "on_land_surface" || ocean) {
        y = surface();
    } else {
        return std::unexpected("ruined portal: unknown placement " + std::string{placement});
    }

    // Down until three of the four corners stand on something.
    const std::array<std::array<i32, 2>, 4> corners{{{box.min_x, box.min_z},
                                                     {box.max_x, box.min_z},
                                                     {box.min_x, box.max_z},
                                                     {box.max_x, box.max_z}}};
    while (y > floor) {
        i32 filled = 0;
        for (const auto& [x, z] : corners) {
            const auto substance = sampler.base_substance(x, y, z);
            if (!substance) {
                return std::unexpected(
                    std::string{"ruined portal: the sampler has no base column"});
            }
            const bool held = ocean ? *substance == Substance::Solid : *substance != Substance::Air;
            filled += held ? 1 : 0;
        }
        if (filled >= 3) {
            break;
        }
        --y;
    }
    return y;
}

std::expected<bool, std::string> ruined_portal_cold(const StructureWorldSampler& sampler,
                                                    BlockPos                     origin) {
    const auto temperature = sampler.temperature_at(origin.x, origin.y, origin.z);
    if (!temperature) {
        return std::unexpected(std::string{"ruined portal: the sampler has no temperature"});
    }
    return *temperature < kPortalColdTemperature;
}

}  // namespace ov::worldgen
