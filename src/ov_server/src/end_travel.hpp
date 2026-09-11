// The End in the server: where a crossing lands, what it needs, how it leaves.
//
// The rules themselves — the frame, the eyes, the portal's slab, the platform —
// are ov_gameplay/end_portal.hpp's, over any `LevelWriter`. What is here is the
// part that belongs to the server: the chunks an arrival needs before it can be
// built, the packets a crossing and an exit send, and the numbers measured
// against the real server (scripts/measure_end_portal.py, docs/provenance/end.md).
//
// The End's storage is the Nether's type, opened for it
// (`NetherWorld::open(..., DimensionId::End)`: the "end" settings, DIM1/region);
// server.cpp holds it beside the Nether's in its `// ── end ──` blocks.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"

#include <array>

namespace ov::server {

/// Where a player crossing into the End lands.
struct EndArrival {
    Vec3d position{};
    f32   yaw{0.0F};
    f32   pitch{0.0F};
};

/// The spawn point's block centre on its floor, (100.5, 50, 0.5), facing
/// yaw 90 with a level gaze.
[[nodiscard]] EndArrival end_arrival() noexcept;

/// The chunks the 5 x 5 platform round the spawn point touches — they must be
/// resident before it is built.
[[nodiscard]] std::array<ChunkPos, 2> end_platform_chunks() noexcept;

/// Game Event 4, "win game": the credits. Its value is 1 the first time (the
/// credits roll) and 0 after (straight to the respawn).
inline constexpr u8 kGameEventWinGame = 4;

/// The Respawn a player coming home from the exit portal is sent keeps
/// everything — a respawn with the whole player copied, not a death's.
inline constexpr u8 kEndExitDataKept = 3;

/// The owner of the `Transient` ticket that brings the chunks round the
/// origin in before the fight builds its exit portal there.
inline constexpr u64 kEndFightTicket = 0x454E445F46494748ULL;

/// World events: an eye put into a frame, and an End portal opening (global).
inline constexpr i32 kWorldEventEyePlaced       = 1503;
inline constexpr i32 kWorldEventEndPortalOpened = 1038;

}  // namespace ov::server
