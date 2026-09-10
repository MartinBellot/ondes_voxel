// Nether portals: the frame, the fire, the travel and the portal at the other end.
//
// Everything here is a rule over a `LevelView` / `LevelWriter`, so the same code
// can run against the server's chunks and against a test map. What the server
// adds — which dimension a player is in, the per-player timers, the index of
// known portal blocks — lives in ov_server/src/nether_travel.{hpp,cpp}.
//
// The specification is the Minecraft Wiki's "Nether portal" page (Java
// Edition, 1.20): interior 2x3 to 21x21, corners optional; entry after 80
// ticks in survival and 1 in creative; 300 ticks of cooldown; the 1:8 scale;
// a search of the closest portal block, Euclidean with y, within 128 blocks in
// the overworld and 16 in the Nether; and when none is found, a new 4x5 frame
// placed at the closest spot within 16 blocks that has three rows of four
// solid blocks with four of air above them, else one row, else forced at the
// target with its y clamped to [70, height - 10] on a 2x3 obsidian platform.
// What the wiki does not say — the order candidates are visited in when two
// are equally close, the exact exit offset — is measured against the real
// server by scripts/measure_nether_portal.py; see docs/provenance/nether.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <span>
#include <vector>

namespace ov::gameplay {

/// The `axis` property of `minecraft:nether_portal`: the horizontal axis the
/// portal's plane runs along.
enum class PortalAxis : u8 { X, Z };

/// A frame's inside: the lowest, most negative interior block, and its size.
struct PortalFrame {
    BlockPos   bottom_left{};
    PortalAxis axis{PortalAxis::X};
    i32        width{0};
    i32        height{0};
    /// Portal blocks already inside. Zero for a frame that is waiting to be lit.
    i32 portal_blocks{0};

    [[nodiscard]] bool complete() const noexcept { return portal_blocks == width * height; }
};

/// A lit portal's rectangle, as the travel code measures positions against it.
struct PortalRect {
    BlockPos   min_corner{};
    PortalAxis axis{PortalAxis::X};
    i32        width{0};
    i32        height{0};
};

/// The portal limits the wiki gives: an inside of 2..21 by 3..21.
inline constexpr i32 kPortalMinWidth  = 2;
inline constexpr i32 kPortalMaxWidth  = 21;
inline constexpr i32 kPortalMinHeight = 3;
inline constexpr i32 kPortalMaxHeight = 21;

/// Ticks a player must stand in a portal: 80 in survival and adventure, 1 for
/// an invulnerable (creative) player.
inline constexpr i32 kPortalWaitSurvival = 80;
inline constexpr i32 kPortalWaitCreative = 1;

/// Ticks after a crossing before a portal will take the entity again — and
/// the timer is refreshed while it stands in one.
inline constexpr i32 kPortalCooldown = 300;

/// How far the destination search reaches, in blocks, square.
inline constexpr i32 kSearchRadiusOverworld = 128;
inline constexpr i32 kSearchRadiusNether    = 16;

/// How far a new portal may be placed from its target, square.
inline constexpr i32 kCreateRadius = 16;

class PortalRules {
public:
    /// With the registries: what a new portal may overwrite is the
    /// `#minecraft:replaceable` tag — the game's `canBeReplaced` — minus fluids.
    PortalRules(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// Without them — a test map — "replaceable" falls back to "has no
    /// collision box", which is wider than the tag: a fungus, a vine or a
    /// mushroom has no box and is not replaceable. Measured to move one of six
    /// new portals by a block; see docs/provenance/nether.md.
    explicit PortalRules(const registry::BlockRegistry& blocks);

    /// Can a portal fill this block? Air, fire of either kind, or portal.
    [[nodiscard]] bool is_empty(registry::BlockStateId state) const noexcept;

    [[nodiscard]] bool is_obsidian(registry::BlockStateId state) const noexcept {
        return blocks_->block_of(state) == obsidian_block_;
    }
    [[nodiscard]] bool is_portal(registry::BlockStateId state) const noexcept {
        return blocks_->block_of(state) == portal_block_;
    }

    /// The portal state for an axis.
    [[nodiscard]] registry::BlockStateId portal_state(PortalAxis axis) const noexcept {
        return axis == PortalAxis::X ? portal_x_ : portal_z_;
    }
    [[nodiscard]] std::optional<PortalAxis> axis_of(registry::BlockStateId state) const noexcept;

    /// The frame around `inside` in the plane of `axis`, if there is one of a
    /// legal size with an obsidian border and nothing but empty blocks inside.
    [[nodiscard]] std::optional<PortalFrame> frame_at(const world::LevelView& level,
                                                      BlockPos inside, PortalAxis axis) const;

    /// A frame with no portal in it yet, trying `preferred` first and then the
    /// other axis — what lighting a fire inside a frame looks for.
    [[nodiscard]] std::optional<PortalFrame> find_empty_frame(const world::LevelView& level,
                                                              BlockPos   inside,
                                                              PortalAxis preferred) const;

    /// Fill a frame with portal blocks.
    void fill(world::LevelWriter& level, const PortalFrame& frame) const;

    /// A fire appeared at `pos`: light the frame it is in, if any. True when a
    /// portal was made.
    bool light(world::LevelWriter& level, BlockPos pos) const;

    /// A block changed at `pos`. A portal block beside it, in its own plane,
    /// whose frame is no longer complete is removed — and with it every portal
    /// block it is joined to, which is the cascade the game shows. Returns how
    /// many portal blocks were removed.
    usize on_neighbour_changed(world::LevelWriter& level, BlockPos pos) const;

    /// Whatever a write at `pos` means for portals: a fire lights the frame it
    /// stands in, anything else may break the portal beside it. What the
    /// server's settle loop calls for every written position — so flint and
    /// steel, a fire charge, a dispenser and a broken frame block all go
    /// through the same door. Returns the portal blocks made or removed.
    usize on_block_changed(world::LevelWriter& level, BlockPos pos) const;

    /// The rectangle of portal blocks that contains `block`.
    [[nodiscard]] std::optional<PortalRect> rectangle_at(const world::LevelView& level,
                                                         BlockPos block) const;

    /// Build a portal for an arrival at `target`: the wiki's search, then its
    /// two fallbacks. `top` is the highest y a portal may reach — the logical
    /// height's top in the Nether (127), the build limit's in the overworld.
    [[nodiscard]] PortalRect create(world::LevelWriter& level, BlockPos target, PortalAxis axis,
                                    i32 top) const;

private:
    [[nodiscard]] bool can_replace(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool is_solid(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool can_host(const world::LevelView& level, BlockPos origin, PortalAxis axis,
                                i32 offset) const;

    const registry::BlockRegistry* blocks_;
    registry::BlockId              obsidian_block_{};
    registry::BlockId              portal_block_{};
    registry::BlockId              fire_block_{};
    registry::BlockId              soul_fire_block_{};
    registry::BlockStateId         obsidian_{};
    registry::BlockStateId         portal_x_{};
    registry::BlockStateId         portal_z_{};
    /// Membership of `#minecraft:replaceable`, by block id. Empty when built
    /// without registries.
    std::vector<bool> replaceable_;
};

/// Where an entity at `position` lands: the other dimension's coordinates,
/// `floor(x * scale)` and `floor(z * scale)`, y unchanged, clamped to the
/// world border's ±29 999 983.
[[nodiscard]] BlockPos scaled_target(Vec3d position, f64 scale) noexcept;

/// The closest candidate to `target` within `radius` (square, in x and z):
/// the smallest squared distance, y included, the lower one on a tie.
[[nodiscard]] std::optional<BlockPos> closest_portal(std::span<const BlockPos> candidates,
                                                     BlockPos target, i32 radius) noexcept;

/// An entity's place in a portal, as fractions of the part of the rectangle it
/// can occupy once its own width and height are taken off: along the axis,
/// up, and — raw, in blocks — across the portal's thickness.
[[nodiscard]] Vec3d relative_position(const PortalRect& rect, Vec3d position, f64 width,
                                      f64 height) noexcept;

/// The same fractions, put back into a destination rectangle. `yaw` turns by
/// 90 degrees when the two portals' axes differ.
[[nodiscard]] Vec3d exit_position(const PortalRect& destination, PortalAxis source_axis,
                                  Vec3d relative, f64 width, f64 height, f32& yaw) noexcept;

}  // namespace ov::gameplay
