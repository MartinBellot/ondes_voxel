#include "end_travel.hpp"

#include "ov/gameplay/end_portal.hpp"

namespace ov::server {

EndArrival end_arrival() noexcept {
    const BlockPos spawn = gameplay::kEndSpawnPoint;
    return EndArrival{Vec3d{static_cast<f64>(spawn.x) + 0.5, static_cast<f64>(spawn.y),
                            static_cast<f64>(spawn.z) + 0.5},
                      gameplay::kEndArrivalYaw, 0.0F};
}

std::array<ChunkPos, 2> end_platform_chunks() noexcept {
    // x 98..102 is one chunk column (6); z -2..2 straddles chunks -1 and 0.
    const BlockPos spawn = gameplay::kEndSpawnPoint;
    return {ChunkPos{(spawn.x - 2) >> 4, (spawn.z - 2) >> 4},
            ChunkPos{(spawn.x + 2) >> 4, (spawn.z + 2) >> 4}};
}

}  // namespace ov::server
