// How a mob decides where to put its feet.
//
// Vanilla's navigation is an A* over block positions with a *node evaluator*
// in front of it. The evaluator is the whole design: the search itself is
// textbook, and everything that makes a zombie behave like a zombie — that it
// steps up one block but not two, that it walks around lava and through water
// only reluctantly, that it fits through a one-wide gap only if it is one wide
// — lives in how a block is turned into a node and what that node costs. Three
// evaluators exist because three kinds of mob move: on the ground, in water,
// and in the air.
//
// ── What is measured and what is not ────────────────────────────────────────
//
// The *shape* here is checked against the game: scripts/measure_mobs.py walks a
// real 1.20.1 zombie through a maze and records where it goes, and
// tests/test_pathfinding.cpp replays the same maze through the code below and
// compares the route. That measurement covers the search, the neighbour
// generation, the step-up and the mob's width — everything a dry corridor
// exercises.
//
// It does **not** cover the cost of a node that is unpleasant rather than
// impassable: water, fire, a closed door. Those maluses are ours, shaped like
// the game's and not read off it, and they are named as such in
// docs/provenance/mobs.md rather than presented as measurements. What *is*
// measured about them is an ordering: a zombie offered a straight route through
// four blocks of water and a dry detour prefers the dry one until the detour
// grows past a threshold, and the threshold is recorded there.
//
// ── No allocation in the tick ───────────────────────────────────────────────
//
// A path is found many times a second across every mob in the world, so the
// finder owns its open set, its node pool and its visited table for the life of
// the object and clears them by stamping a generation rather than by freeing.
// A std::unordered_map here would allocate on every insert and trip
// NoAllocScope on the first debug tick.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/level.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// What one block position is, as far as walking on it is concerned.
///
/// Vanilla calls these `PathType`. The set below is the subset this project
/// classifies; a block that matches none of them is `Blocked`, which is the
/// safe direction — a mob refuses to path through something it does not
/// understand rather than walking into it.
enum class PathNodeType : u8 {
    /// Solid, or otherwise not somewhere a body may be.
    Blocked,
    /// Empty space with nothing to stand on.
    Open,
    /// Empty space with a solid floor under it. The ordinary node.
    Walkable,
    /// A fence, a wall, or a closed fence gate: taller than the block it sits
    /// in, so a mob may neither walk through it nor jump it.
    Fence,
    /// Standing water.
    Water,
    /// A water block next to air — where a swimmer surfaces and a walker may
    /// climb out.
    WaterBorder,
    /// Lava. Impassable to everything here.
    Lava,
    /// A door that is open: passable, at a small cost.
    DoorOpen,
    /// A wooden door that is shut. Passable — vanilla's zombies open them — at
    /// a cost that makes a mob prefer the way round if there is one.
    DoorWoodClosed,
    /// An iron door that is shut. Nothing here can open one.
    DoorIronClosed,
    /// Something that hurts on contact: fire, lava's edge, a cactus, a
    /// campfire, magma. Passable but expensive.
    DamageFire,
    /// Adjacent to something that hurts. Passable, cheaper than the thing
    /// itself, so a path skirts a fire rather than hugging it.
    DangerFire,
    /// A rail. Passable, and cheap enough that a mob does not detour round it.
    Rail,
    /// Leaves. Solid to a path but not to a fall.
    Leaves,
    /// A pressure plate, a tripwire, a button: passable, and worth avoiding
    /// because stepping on one is an event.
    Trapdoor,
    /// Honey, soul sand and anything else that slows a body down.
    StickyHoney,
};

[[nodiscard]] std::string_view to_string(PathNodeType type) noexcept;

/// The extra cost of ending a step on a node of this type.
///
/// Added to the geometric length of the step, so a malus of 8 means "worth an
/// eight-block detour to avoid". A negative value means impassable: the finder
/// never expands such a node, which is how `Blocked` and `Lava` are expressed
/// without a second predicate that could disagree with this one.
///
/// **These numbers are ours.** See the note at the top of this file, and
/// docs/provenance/mobs.md for what was measured about them and what was not.
[[nodiscard]] f32 path_malus(PathNodeType type) noexcept;

[[nodiscard]] inline bool is_passable(PathNodeType type) noexcept {
    return path_malus(type) >= 0.0F;
}

/// How much of the world a mob's body takes up, in whole blocks.
///
/// Vanilla rounds a hitbox up: a 0.6-wide zombie needs one column, a 1.4-wide
/// ravager needs two. The rounding is what makes a spider — 1.4 wide and 0.9
/// tall — fit under a one-block ceiling that a zombie does not.
struct MobSize {
    i32 width{1};
    i32 height{2};

    /// From a measured hitbox. `std::ceil`, then at least one, because a
    /// zero-width mob would be given a body of no blocks and could path
    /// through a wall.
    [[nodiscard]] static MobSize from_box(f32 box_width, f32 box_height) noexcept;
};

/// What a mob may do while pathing, beyond putting one foot in front of the
/// other. Held apart from `MobSize` because two mobs of the same size differ
/// here: a zombie opens doors and a skeleton does not.
struct PathAbilities {
    /// How far it may step up without jumping, in blocks. One for everything
    /// that walks.
    i32 step_up{1};

    /// How far it may drop and still consider the landing a path node.
    i32 max_fall{3};

    /// May it open a wooden door? Changes `DoorWoodClosed` from impassable to
    /// merely expensive.
    bool opens_doors{false};

    /// May it path through water at all? A walking mob may; one that drowns
    /// instantly should not.
    bool enters_water{true};

    /// Does it avoid sunlight? Not used by the finder — it is here because the
    /// goals that build a path need to carry it alongside the rest.
    bool avoids_sun{false};
};

/// One step of a finished route: a block position and what it is.
struct PathStep {
    BlockPos     pos{};
    PathNodeType type{PathNodeType::Walkable};
};

/// A route, and whether it actually gets there.
///
/// `reached` is the difference between "here is the way" and "here is as close
/// as I could get". Vanilla returns the partial path either way and a mob
/// walks it; a caller that treated a partial path as a failure would make mobs
/// stand still whenever the target is walled in, which is not what the game
/// does.
struct Path {
    std::vector<PathStep> steps;
    bool                  reached{false};
    /// How many nodes the search expanded. For tests and for the budget.
    i32 visited{0};

    void clear() noexcept {
        steps.clear();
        reached = false;
        visited = 0;
    }

    [[nodiscard]] bool empty() const noexcept { return steps.empty(); }
};

/// Turning blocks into nodes. One implementation per way of moving.
///
/// Stateless with respect to the search: the finder owns the open set and calls
/// back into here, so one evaluator serves every mob of its kind.
class NodeEvaluator {
public:
    NodeEvaluator()                                = default;
    NodeEvaluator(const NodeEvaluator&)            = delete;
    NodeEvaluator& operator=(const NodeEvaluator&) = delete;
    NodeEvaluator(NodeEvaluator&&)                 = delete;
    NodeEvaluator& operator=(NodeEvaluator&&)      = delete;
    virtual ~NodeEvaluator()                       = default;

    /// What a mob of this size standing at `pos` would be standing in.
    [[nodiscard]] virtual PathNodeType type_at(const world::LevelView& level, BlockPos pos,
                                               MobSize size,
                                               const PathAbilities& abilities) const = 0;

    /// The most a neighbour enumeration can produce, so the finder can size its
    /// scratch buffer once.
    static constexpr usize kMaxNeighbours = 12;

    /// Every node reachable in one move from `from`. Returns how many were
    /// written into `out`, which is at least `kMaxNeighbours` long.
    [[nodiscard]] virtual usize neighbours(const world::LevelView& level, BlockPos from,
                                           MobSize size, const PathAbilities& abilities,
                                           std::span<PathStep> out) const = 0;

    /// Where a mob of this size actually starts, given where its feet are.
    ///
    /// A swimmer standing on the sea floor starts from the water above it, not
    /// from the sand. Returning the position unchanged is right for a walker.
    [[nodiscard]] virtual BlockPos start_node(const world::LevelView& level, BlockPos feet,
                                              MobSize              size,
                                              const PathAbilities& abilities) const;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

/// The one that matters: a mob with feet.
class WalkNodeEvaluator final : public NodeEvaluator {
public:
    [[nodiscard]] PathNodeType type_at(const world::LevelView& level, BlockPos pos, MobSize size,
                                       const PathAbilities& abilities) const override;

    [[nodiscard]] usize neighbours(const world::LevelView& level, BlockPos from, MobSize size,
                                   const PathAbilities& abilities,
                                   std::span<PathStep>  out) const override;

    [[nodiscard]] std::string_view name() const noexcept override { return "walk"; }
};

/// A mob that lives in water: squid, fish, a drowned below the surface.
///
/// Moves in all three dimensions and treats air as the wall.
class SwimNodeEvaluator final : public NodeEvaluator {
public:
    /// `allow_breaching` lets a dolphin leave the water; a cod may not.
    explicit SwimNodeEvaluator(bool allow_breaching = false) noexcept
        : allow_breaching_{allow_breaching} {}

    [[nodiscard]] PathNodeType type_at(const world::LevelView& level, BlockPos pos, MobSize size,
                                       const PathAbilities& abilities) const override;

    [[nodiscard]] usize neighbours(const world::LevelView& level, BlockPos from, MobSize size,
                                   const PathAbilities& abilities,
                                   std::span<PathStep>  out) const override;

    [[nodiscard]] BlockPos start_node(const world::LevelView& level, BlockPos feet, MobSize size,
                                      const PathAbilities& abilities) const override;

    [[nodiscard]] std::string_view name() const noexcept override { return "swim"; }

private:
    bool allow_breaching_{false};
};

/// A mob that flies: bat, bee, parrot, ghast.
///
/// The same six directions as the swimmer, but empty air is the medium and
/// water is the wall.
class FlyNodeEvaluator final : public NodeEvaluator {
public:
    [[nodiscard]] PathNodeType type_at(const world::LevelView& level, BlockPos pos, MobSize size,
                                       const PathAbilities& abilities) const override;

    [[nodiscard]] usize neighbours(const world::LevelView& level, BlockPos from, MobSize size,
                                   const PathAbilities& abilities,
                                   std::span<PathStep>  out) const override;

    [[nodiscard]] std::string_view name() const noexcept override { return "fly"; }
};

/// A* over whichever evaluator it is handed.
///
/// One per mob, or one shared by a level that paths mobs one at a time — the
/// finder holds only scratch, and `find` overwrites all of it. Not thread-safe,
/// which is the same rule as everything else on the tick thread.
class PathFinder {
public:
    /// `capacity` is the node budget: the search stops after expanding this
    /// many and returns the best partial route. Vanilla scales its own budget
    /// with the mob's follow range; the default here is large enough for a
    /// sixty-block route through a maze, which is what the oracle exercises.
    explicit PathFinder(usize capacity = 4096);

    PathFinder(const PathFinder&)            = delete;
    PathFinder& operator=(const PathFinder&) = delete;
    PathFinder(PathFinder&&)                 = default;
    PathFinder& operator=(PathFinder&&)      = default;
    ~PathFinder()                            = default;

    /// Find a route from `from` to `to`.
    ///
    /// Returns true when the target itself was reached. `out` is filled either
    /// way: with the route on success, and with the route to the expanded node
    /// closest to the target otherwise — which is what makes a mob walk *at* a
    /// wall rather than stand still in front of an unreachable player.
    ///
    /// `max_range` is a hard cut in blocks from `from`; a node further than
    /// that is never expanded, so a mob does not search the whole world for a
    /// target it cannot reach.
    bool find(const NodeEvaluator& evaluator, const world::LevelView& level, BlockPos from,
              BlockPos to, MobSize size, const PathAbilities& abilities, f32 max_range,
              Path& out);

    /// How many nodes the last search expanded.
    [[nodiscard]] i32 visited() const noexcept { return visited_; }

private:
    struct Node {
        BlockPos     pos{};
        f32          g{0.0F};
        f32          f{0.0F};
        i32          parent{-1};
        PathNodeType type{PathNodeType::Walkable};
        bool         closed{false};
    };

    /// Open-addressed, power-of-two, linear-probing, stamped.
    ///
    /// The stamp is what makes `clear` free: a slot whose generation is not the
    /// current one is empty no matter what it holds. A std::unordered_map here
    /// would allocate a node per insert, every search, on the tick thread.
    struct Slot {
        u64 key{0};
        u32 generation{0};
        i32 node{-1};
    };

    [[nodiscard]] i32  lookup(u64 key) const noexcept;
    void               insert(u64 key, i32 node) noexcept;
    void               push(i32 node);
    [[nodiscard]] i32  pop();

    std::vector<Node> nodes_;
    std::vector<i32>  heap_;
    std::vector<Slot> table_;
    u32               generation_{0};
    usize             capacity_{0};
    i32               visited_{0};
};

/// Follow a path: the position the mob should be steering at right now.
///
/// Returns false when the path is finished or empty. `advance` is what turns a
/// list of blocks into movement, and it is separate from the finder because a
/// mob re-uses one path for many ticks — recomputing A* every tick is the
/// single easiest way to make a server with two hundred mobs unplayable.
class PathFollower {
public:
    void   set(const Path& path);
    void   clear() noexcept { steps_.clear(); index_ = 0; }
    [[nodiscard]] bool done() const noexcept { return index_ >= steps_.size(); }
    [[nodiscard]] usize index() const noexcept { return index_; }
    [[nodiscard]] std::span<const PathStep> steps() const noexcept { return steps_; }

    /// Advance past any step the mob has already reached, and return the one it
    /// should walk towards. `position` is the mob's feet.
    ///
    /// The horizontal tolerance is what stops a mob dithering on a corner: a
    /// step counts as reached once the body is over its column, not once the
    /// feet are on its exact centre.
    [[nodiscard]] bool next_waypoint(const Vec3d& position, f32 width, Vec3d& out);

private:
    std::vector<PathStep> steps_;
    usize                 index_{0};
};

}  // namespace ov::gameplay
