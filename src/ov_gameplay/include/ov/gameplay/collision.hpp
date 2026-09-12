// Where a box may go, given the blocks around it.
//
// The maths lives in ov_math: boxes, sweeps, and the per-axis clipping that
// makes a player slide along a wall instead of stopping dead. What is here is
// the bridge — turning a block state into the boxes it occupies at a position,
// and asking the world rather than a list.
//
// The shapes come from the registry, in units of a thirty-second of a block,
// and they are per state: a bottom slab is half a block, a bottom stair is two
// boxes, an extended piston head leaves the cube. Anything reading a single box
// per block gets stairs wrong the first time someone climbs one.
#pragma once

#include "ov/math/aabb.hpp"
#include "ov/registry/block_states.hpp"

#include <vector>

namespace ov::gameplay {

/// How the caller reads the world.
///
/// A function pointer and a context rather than a template: this header is
/// public, and a template here would drag the caller's world type into every
/// translation unit that includes it.
using BlockLookup = registry::BlockStateId (*)(void* context, i32 x, i32 y, i32 z);

class BlockMotionTable;

class CollisionWorld {
public:
    /// `motion` is optional: without it every block is ordinary ground — no
    /// ice, no ladders, no cobwebs — which is what a test about walls wants.
    CollisionWorld(const registry::BlockRegistry& blocks, BlockLookup lookup, void* context,
                   const BlockMotionTable* motion = nullptr) noexcept
        : blocks_{&blocks}, lookup_{lookup}, context_{context}, motion_{motion} {}

    /// The state at a position, through the caller's lookup.
    [[nodiscard]] registry::BlockStateId state_at(i32 x, i32 y, i32 z) const {
        return lookup_(context_, x, y, z);
    }
    [[nodiscard]] const registry::BlockRegistry& blocks() const noexcept { return *blocks_; }
    /// What blocks do to movement, or null when the caller did not say.
    [[nodiscard]] const BlockMotionTable* motion() const noexcept { return motion_; }

    /// The boxes one block state occupies, in world space. Appends.
    void boxes_at(i32 x, i32 y, i32 z, std::vector<AABB>& out) const;

    /// Does anything solid overlap this box?
    ///
    /// Used to refuse a placement that would land inside a player, which is
    /// what vanilla calls an obstructed position.
    [[nodiscard]] bool overlaps(const AABB& box) const;

    /// Move a box by `delta`, stopping against whatever is in the way.
    ///
    /// Resolved one axis at a time in the order Y, X, Z — vanilla's order.
    /// Testing the whole displacement at once makes a player walking into a
    /// wall stop dead instead of sliding along it.
    [[nodiscard]] Vec3d slide(const AABB& box, const Vec3d& delta) const;

private:
    const registry::BlockRegistry* blocks_{nullptr};
    BlockLookup                    lookup_{nullptr};
    void*                          context_{nullptr};
    const BlockMotionTable*        motion_{nullptr};
};

/// A player's box: 0.6 wide, 1.8 tall, standing on the given position.
[[nodiscard]] inline AABB player_box(const Vec3d& position) noexcept {
    return AABB::from_entity(position, 0.6, 1.8);
}

}  // namespace ov::gameplay
