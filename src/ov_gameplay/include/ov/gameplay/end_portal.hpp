// End portals: the frame, the eyes, the portal, and the platform at the other end.
//
// Rules over a `LevelView` / `LevelWriter`, like nether_portal.hpp, so the same
// code runs against the server's chunks and against a test map. What the server
// adds — which dimension a player is in, the End's chunks, the Respawn packet —
// is in ov_server/src/end_travel.{hpp,cpp}.
//
// The specification is the Minecraft Wiki's "End portal" page (Java Edition,
// 1.20) and the game's own block states (`end_portal_frame`: `eye`, `facing`):
//
//   * twelve frames round a 3 x 3 square, corners empty, each **facing the
//     inside** — the north side faces south, the west side east — and each
//     holding an eye; what the square holds does not matter;
//   * an eye used on a frame without one puts it in (and is consumed outside
//     creative); when that completes a ring, the 3 x 3 inside becomes
//     `end_portal`, at the frames' height;
//   * an entity whose box meets a portal block's shape — the slab from 6/16 to
//     12/16 of the block — goes to the End at once: no wait, no cooldown;
//   * it arrives at (100.5, 50, 0.5), facing yaw 90, over a 5 x 5 obsidian
//     platform at y = 48 — two below the spawn point, measured — whose three
//     layers above (49..51) are cleared to air; rebuilt on every arrival.
//
// The arrival and the platform are measured against the real server by
// scripts/measure_end_portal.py (docs/provenance/end.md).
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/level.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace ov::gameplay {

/// Where an entity crossing into the End is put: the block it stands in.
inline constexpr BlockPos kEndSpawnPoint{100, 50, 0};

/// The yaw a player arrives with.
inline constexpr f32 kEndArrivalYaw = 90.0F;

/// The portal block's shape: a slab from 6/16 to 12/16 of its height.
inline constexpr f64 kEndPortalShapeLow  = 6.0 / 16.0;
inline constexpr f64 kEndPortalShapeHigh = 12.0 / 16.0;

/// What using an eye on a block did.
enum class EyeUse : u8 {
    /// Not a frame, or a frame that already has an eye: the eye is not used
    /// here (the item's own use — throwing it — is the caller's business).
    Pass,
    /// An eye went in, and the ring is not complete.
    Inserted,
    /// An eye went in and completed a ring: the portal is open.
    Activated,
};

struct EyeOutcome {
    EyeUse result{EyeUse::Pass};
    /// With `Activated`: the portal's centre block.
    BlockPos portal_centre{};
};

class EndPortalRules {
public:
    explicit EndPortalRules(const registry::BlockRegistry& blocks);

    /// Every block these rules need was found in the registry.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] bool is_frame(registry::BlockStateId state) const noexcept {
        return valid_ && blocks_->block_of(state) == frame_block_;
    }
    [[nodiscard]] bool has_eye(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool is_portal(registry::BlockStateId state) const noexcept {
        return valid_ && blocks_->block_of(state) == portal_block_;
    }
    [[nodiscard]] registry::BlockStateId portal_state() const noexcept { return portal_; }

    /// A frame facing `facing` ("north", "south", "west", "east"), with or
    /// without its eye.
    [[nodiscard]] registry::BlockStateId frame_state(std::string_view facing, bool eye) const;

    /// An eye of ender used on `pos`.
    EyeOutcome use_eye(world::LevelWriter& level, BlockPos pos) const;

    /// The centre of the complete ring the frame at `frame` belongs to, if
    /// there is one: twelve frames, each with an eye, each facing the inside.
    [[nodiscard]] std::optional<BlockPos> complete_ring(const world::LevelView& level,
                                                        BlockPos frame) const;

    /// Open the 3 x 3 portal round `centre`.
    void open(world::LevelWriter& level, BlockPos centre) const;

    /// Does a box meet the shape of an end portal block in `level`?
    [[nodiscard]] bool box_in_portal(const world::LevelView& level, const AABB& box) const;

    /// The arrival platform round `spawn`: obsidian two below it, three layers
    /// of air over that, five by five.
    void build_platform(world::LevelWriter& level, BlockPos spawn = kEndSpawnPoint) const;

    /// The exit portal — the game's `end_podium` — with its top ring at
    /// `origin`: a bedrock bowl of radius 3.5 whose inside (radius 2.5) is
    /// `end_portal` when `active` and air when not, end stone under its rim,
    /// air above it to 32 blocks, a four-block bedrock pillar in the middle
    /// and a wall torch on each side of the pillar two blocks up. Distances
    /// are between block corners, y included.
    void build_exit_portal(world::LevelWriter& level, BlockPos origin, bool active) const;

    /// Where the dragon fight puts the exit portal: the first free block of
    /// the column at (0, 0) and one down — then further down while that is
    /// bedrock and above the sea level (0 in the End). `top` is the heightmap
    /// (`MOTION_BLOCKING_NO_LEAVES`) value of the column: one above the
    /// highest block.
    [[nodiscard]] BlockPos exit_portal_origin(const world::LevelView& level, i32 top) const;

    [[nodiscard]] registry::BlockStateId dragon_egg() const noexcept { return egg_; }

    /// An End gateway at `pos`: the block, bedrock above and below it and
    /// down the four sides of its column, air round it on its own level — the
    /// shape the `end_gateway` feature builds (end_features.cpp).
    void build_gateway(world::LevelWriter& level, BlockPos pos) const;

private:
    [[nodiscard]] bool frame_faces(registry::BlockStateId state, std::string_view facing) const;

    const registry::BlockRegistry* blocks_;
    bool                           valid_{false};
    registry::BlockId              frame_block_{};
    registry::BlockId              portal_block_{};
    registry::BlockStateId         frame_{};
    registry::BlockStateId         portal_{};
    registry::BlockStateId         obsidian_{};
    registry::BlockStateId         bedrock_{};
    registry::BlockStateId         end_stone_{};
    registry::BlockStateId         egg_{};
    registry::BlockStateId         gateway_{};
    /// `wall_torch`, facing north, east, south, west.
    std::array<registry::BlockStateId, 4> torches_{};
};

/// The twenty End gateways the dragon fight opens, one per kill, in the order
/// it opens them: twenty positions on a circle of radius 96 at y = 75, shuffled
/// by `new Random(seed)`, taken from the back of the shuffled list.
[[nodiscard]] std::array<BlockPos, 20> end_gateway_order(i64 seed);

}  // namespace ov::gameplay
