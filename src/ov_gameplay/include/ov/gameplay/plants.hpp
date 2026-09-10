// Things that grow: the random tick's answers, and the plants that live on it.
//
// A plant in Minecraft is not a timer. Nothing counts down under a wheat crop;
// a crop grows because, every tick, the world picks a few positions per
// 16x16x16 section **at random** and asks whatever is there to do something.
// A crop asked often enough, with good enough luck, climbs its eight stages.
// So what this file holds is the answer each block gives when it is asked —
// the random tick — plus the two other ways a plant is woken: a scheduled tick
// it asked for itself (leaves recomputing their distance, a cactus checking
// whether it may still stand) and a neighbour changing next to it (a crop whose
// farmland was dug out from under it).
//
// The picking itself is not here. Which sections are ticked, how many
// positions, from what generator — that is the server's, because it depends on
// where the players are. This file only ever sees "this position was picked".
//
// ── Layer 9, and what that forbids ──────────────────────────────────────────
//
// Every rule takes a `world::LevelWriter&`, never a server. Three things a
// plant needs are not on that interface, and they are deliberately gathered
// into `PlantEnvironment` rather than bolted onto the level:
//
//   * **light** — a crop needs 9, a sapling 9 in the block above, and ice melts
//     above 11 of block light. The level interface is kept to what a client
//     replica can answer about blocks; light is a separate store;
//   * **drops** — a crop uprooted by its farmland vanishing drops its seeds,
//     and item entities are the server's;
//   * **trees** — a sapling becomes a tree through the worldgen tree placers,
//     which are layer 10. This layer cannot include them, so it asks.
//
// ── Where the numbers come from ─────────────────────────────────────────────
//
// The rules are specified from the Minecraft Wiki (docs/provenance/
// agriculture.md cites each page) and the parts that have an oracle were then
// **measured** against a real 1.20.1 server: the hydration map, the leaf decay
// map, the growth-time distributions and the bone meal distributions. Where a
// number is carried over from documentation and not measured, the comment next
// to it says so.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/item_use.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The generator every plant rule draws from.
///
/// Vanilla's own is seeded from the wall clock and is therefore not a
/// reference anyone can reproduce; what matters is that ours is explicit and
/// seeded, so that a replay of the same world grows the same wheat
/// (CLAUDE.md § 2, principle 5).
using PlantRandom = math::XoroshiroRandomSource;

/// What a plant asks the world that a `LevelWriter` cannot answer.
class PlantEnvironment {
public:
    PlantEnvironment()                                   = default;
    PlantEnvironment(const PlantEnvironment&)            = delete;
    PlantEnvironment& operator=(const PlantEnvironment&) = delete;
    PlantEnvironment(PlantEnvironment&&)                 = delete;
    PlantEnvironment& operator=(PlantEnvironment&&)      = delete;
    virtual ~PlantEnvironment()                          = default;

    /// Stored block light, 0..15.
    [[nodiscard]] virtual u8 block_light(BlockPos pos) const = 0;

    /// Stored sky light, 0..15, **before** the time of day dims it.
    [[nodiscard]] virtual u8 sky_light(BlockPos pos) const = 0;

    /// What the time of day takes off the sky: 0 at noon, 11 at midnight.
    [[nodiscard]] virtual u8 sky_darken() const = 0;

    /// Is rain falling on this position? Farmland counts rain as water.
    [[nodiscard]] virtual bool is_raining_at(BlockPos pos) const = 0;

    /// A rule is about to remove this block as though it had been broken: spawn
    /// whatever its loot table gives. Called **before** the block is replaced,
    /// because the table reads the state.
    virtual void drop_block(BlockPos pos, registry::BlockStateId state) = 0;

    /// Replace a sapling by a tree. True when a tree was placed.
    ///
    /// The tree placers are worldgen, layer 10; this layer can only ask.
    virtual bool grow_tree(world::LevelWriter& level, BlockPos pos,
                           registry::BlockStateId sapling, PlantRandom& random) = 0;

    /// `getRawBrightness(pos, 0)`: the brighter of the two stores, the sky
    /// **undimmed**. What a crop compares — which is why wheat grows at night.
    [[nodiscard]] u8 raw_brightness(BlockPos pos) const {
        const u8 sky   = sky_light(pos);
        const u8 block = block_light(pos);
        return sky > block ? sky : block;
    }

    /// The sky dimmed by the time of day, then the brighter of the two. What a
    /// sapling and a spreading grass block compare.
    [[nodiscard]] u8 local_brightness(BlockPos pos) const {
        const u8 sky    = sky_light(pos);
        const u8 dimmed = sky > sky_darken() ? static_cast<u8>(sky - sky_darken()) : u8{0};
        const u8 block  = block_light(pos);
        return dimmed > block ? dimmed : block;
    }
};

/// A block the random tick reaches and this project does not answer.
///
/// Named, one per entry, rather than silently ticked as nothing: a world full
/// of vines that never grow looks exactly like one where they grow slowly.
struct UnansweredTicker {
    std::string_view block;
    std::string_view why;
};

/// What planting from the hand did.
struct PlantOutcome {
    /// Something was planted; the caller consumes one from the hand.
    bool planted{false};
    /// Where, so the caller can notify around it.
    BlockPos at{};
};

/// The plant rules, with every registry lookup they need resolved once.
///
/// Stateless between calls: everything a plant remembers is in its block
/// state, as in the game. One instance serves any number of levels.
class Plants {
public:
    Plants(const registry::BlockRegistry& blocks, const registry::Registries& registries);
    ~Plants();
    Plants(const Plants&)            = delete;
    Plants& operator=(const Plants&) = delete;
    Plants(Plants&&) noexcept;
    Plants& operator=(Plants&&) noexcept;

    /// Could a random tick on this state do anything? A table lookup, asked once
    /// per picked position, so it has to be one.
    ///
    /// Vanilla's own `isRandomlyTicking` is per state for the same reason: a
    /// section full of mature wheat and persistent leaves costs nothing.
    [[nodiscard]] bool ticks_randomly(registry::BlockStateId state) const noexcept;

    /// The random tick itself.
    void random_tick(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                     registry::BlockStateId state, PlantRandom& random) const;

    /// A block tick this file scheduled. False when the block is not one of
    /// ours, so the caller can offer it to the next engine.
    bool scheduled_tick(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                        registry::BlockId block) const;

    /// `from` changed next to `pos`. Vanilla's `updateShape`, for plants: a
    /// crop that lost its farmland breaks, leaves ask to recompute their
    /// distance, farmland under a new solid block turns back to dirt.
    void neighbour_changed(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                           BlockPos from) const;

    /// Bone meal on the block at `pos`. Pass when the block does not take it,
    /// Success (consume one) when it did — **including** when the 45 % sapling
    /// roll failed, which still uses the bone meal up.
    [[nodiscard]] UseOutcome bone_meal(world::LevelWriter& level, PlantEnvironment& env,
                                       BlockPos pos, PlantRandom& random) const;

    /// Plant `item` against the clicked face. Seeds on farmland, a nether wart
    /// on soul sand, a sapling on dirt, cocoa on the side of a jungle log.
    [[nodiscard]] PlantOutcome plant(world::LevelWriter& level, BlockPos clicked, i32 face,
                                     std::string_view item) const;

    /// Does this item plant something? Lets the caller decide whether to go
    /// through `plant` before the generic block placement path.
    [[nodiscard]] bool is_plantable(std::string_view item) const noexcept;

    /// The crop growth speed at `pos`, the number the growth chance is
    /// `1 / (floor(25 / speed) + 1)` of. Exposed because it is the formula the
    /// measurement checks, and a test can check it without rolling dice.
    [[nodiscard]] f32 growth_speed(const world::LevelView& level, BlockPos pos) const;

    /// The distance a leaf at `pos` would have from its six neighbours.
    [[nodiscard]] i32 leaf_distance(const world::LevelView& level, BlockPos pos) const;

    /// Is there water within the farmland's reach — four blocks horizontally,
    /// at the farmland's level or one above?
    [[nodiscard]] bool farmland_near_water(const world::LevelView& level, BlockPos pos) const;

    /// Every vanilla random ticker this file does not answer, with the reason.
    [[nodiscard]] static std::span<const UnansweredTicker> unanswered() noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

}  // namespace ov::gameplay
