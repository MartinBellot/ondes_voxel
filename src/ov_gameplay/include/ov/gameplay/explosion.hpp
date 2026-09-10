// Explosions: the rays, what they eat, and what is left standing.
//
// An explosion is not a sphere. It is 1352 straight rays fired from one point
// towards the outer cells of a 16×16×16 grid, each carrying an energy of its
// own, each spending that energy on every block it passes through until it runs
// out. What that leaves behind is a shape with a jagged edge that no radius
// reproduces — and a shape that changes from one shot to the next, because the
// energy of a ray is rolled.
//
//   * **1352 rays, not 4096.** Only the *outer* cells of the grid count:
//     16³ − 14³. Every one of the 4096 would fire the interior directions
//     several times over and make the crater denser along the axes.
//
//   * **The energy is `power × (0.7 .. 1.3)`, drawn per ray.** So the same
//     charge in the same box does not make the same crater twice. Nothing that
//     compares an explosion to vanilla can compare one shot to one shot: what
//     is reproducible is the shape over many shots — see
//     docs/provenance/explosions.md, which compares the union and the
//     intersection of thirty shots on each side.
//
//   * **A step is 0.3 blocks and costs 0.22500001.** Not 0.225. The extra bit
//     is in the game's own constant and it moves the last cell of a ray in and
//     out of the crater.
//
//   * **A block eats `(resistance + 0.3) × 0.3` per step spent inside it**, and
//     a block is a bit over three steps wide, so a block of resistance R costs a
//     ray about `R + 1.05`. That is why obsidian at 1200 stops every ray dead
//     and why glass at 0.3 lets one through six blocks of it.
//
//   * **A fluid answers with its own resistance, and it is 100.** Water and
//     lava are not destroyed and they are not passed through: one block of
//     water is worth sixteen of stone. A waterlogged block answers with the
//     larger of the two, which is why every block on the measuring bench was
//     placed dry.
//
// Nothing here takes a server, a tick or an entity. A charge is a point, a
// power and a world to read: that is what lets the same code run inside a
// client's prediction, and it is why this is layer 9 and stays there.
#pragma once

#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/level.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The constants an explosion is made of, each named so a wrong one shows up in
/// a diff rather than hiding inside an expression.
struct ExplosionConstants {
    /// The grid whose outer cells the rays are aimed at. 16 gives 1352 rays.
    i32 grid{16};

    /// How far a ray advances between samples, in blocks.
    f32 step{0.3F};

    /// What one step costs a ray on its own, before any block is charged for.
    ///
    /// `0.22500001`, not `0.225`. The game's constant carries that last bit and
    /// dropping it shortens every ray by a hair — which is invisible on a
    /// crater's middle and visible on its rim.
    f32 step_cost{0.22500001F};

    /// A ray's energy is `power * (energy_low + energy_span * random)`.
    f32 energy_low{0.7F};
    f32 energy_span{0.6F};

    /// A block spends `(resistance + bias) * scale` of a ray, per step inside.
    f32 resistance_bias{0.3F};
    f32 resistance_scale{0.3F};

    /// The radius entities are looked for in, as a multiple of the power, and
    /// the same number the damage falls off over.
    f32 entity_radius_factor{2.0F};

    /// `damage = floor((impact² + impact) / 2 * damage_scale * 2 * power + 1)`.
    f32 damage_scale{7.0F};

    /// What a fluid answers when asked for its resistance. Water and lava both
    /// give this, which is what makes a pond a wall.
    f32 fluid_resistance{100.0F};
};

/// What an explosion does to the blocks it reaches.
enum class BlockInteraction : u8 {
    /// Blocks are left alone. A creeper in a world with mobGriefing off.
    Keep,
    /// Blocks break and drop everything they would drop when mined.
    Destroy,
    /// Blocks break and each drops with probability `1 / power`.
    ///
    /// **No source in this campaign was measured using it.** A TNT charge in a
    /// solid box of dirt broke 1754 blocks over ten shots and dropped 1754
    /// items — a yield of exactly one, ten times out of ten. The variant stays
    /// because the game has it; attributing it to a source would be a guess.
    DestroyWithDecay,
};

/// One charge.
struct ExplosionSpec {
    /// The exact point the rays start from. Not a block: a primed TNT explodes
    /// from its own body, a little above the block it stands on, and that
    /// fraction of a block is visible in the crater's asymmetry.
    Vec3d centre{};

    f32 power{4.0F};

    /// Measured for TNT, and it is `Destroy`: everything it breaks drops.
    BlockInteraction interaction{BlockInteraction::Destroy};

    /// Beds, respawn anchors and ghast fireballs leave fire behind.
    bool fire{false};
};

/// What one entity takes.
struct ExplosionHit {
    /// The fraction of the sample rays from the entity's box that reach the
    /// centre without meeting a collision shape.
    f64 exposure{0.0};

    /// `(1 - distance / (2 * power)) * exposure`. Everything else is this.
    f64 impact{0.0};

    /// Already floored, as the game floors it: an explosion never deals a
    /// fractional amount.
    f32 damage{0.0F};

    /// Added to the entity's velocity, in blocks per tick.
    Vec3d impulse{};

    /// False when the entity is out of range or the maths degenerates — an
    /// entity standing exactly on the centre has no direction to be thrown in,
    /// and the game leaves it alone rather than dividing by zero.
    bool touched{false};
};

/// The rules of an explosion, over a registry.
class Explosions {
public:
    explicit Explosions(const registry::BlockRegistry& blocks,
                        ExplosionConstants               constants = {}) noexcept;

    [[nodiscard]] const ExplosionConstants& constants() const noexcept { return constants_; }

    /// How many rays a charge fires. 1352 for the game's 16-cell grid.
    [[nodiscard]] i32 ray_count() const noexcept;

    /// What one state costs a ray, per step spent inside it.
    ///
    /// The larger of the block's own resistance and the fluid it holds. A
    /// waterlogged fence answers 100, not 2 — which is the whole reason a
    /// flooded corridor survives a charge that flattens a dry one.
    [[nodiscard]] f32 resistance_of(registry::BlockStateId state) const noexcept;

    /// True for a state the pack has no measurement for.
    ///
    /// Named rather than folded into a zero: a block whose resistance is
    /// missing would otherwise be the most fragile in the game, and this
    /// project refuses that kind of default.
    [[nodiscard]] bool resistance_measured(registry::BlockStateId state) const noexcept;

    /// The blocks a charge takes, appended to `out`, sorted and without
    /// duplicates.
    ///
    /// **Blocks only.** Vanilla's own list carries the air cells the rays
    /// passed through as well, and throws them away when it finalises — except
    /// for one use: a charge that leaves fire puts it in exactly those air
    /// cells. Anything implementing that needs a list this does not return.
    ///
    /// `rng` is drawn from exactly once per ray, in the grid's own traversal
    /// order, so the same generator in the same state gives the same crater.
    /// The generator to hand it is `LegacyRandomSource` — `java.util.Random` —
    /// because that is what a 1.20.1 level rolls its explosions on.
    void collect_blocks(const world::LevelView& level, const ExplosionSpec& spec,
                        math::LegacyRandomSource& rng, std::vector<BlockPos>& out) const;

    /// Does this state give up its drops when an explosion takes it?
    ///
    /// Air never does, and neither does anything the pack could not measure.
    [[nodiscard]] bool can_drop_from_explosion(registry::BlockStateId state) const noexcept;

    /// The chance one broken block has of dropping, for this charge.
    ///
    /// `1 / power` for a decaying explosion, 1 for a plain one.
    ///
    /// Which one a TNT charge uses is measured, not assumed: 1754 blocks broken
    /// and 1754 items dropped over ten shots, so a plain one. See
    /// docs/provenance/explosions.md § 5.
    [[nodiscard]] f32 drop_chance(const ExplosionSpec& spec) const noexcept;

    /// Roll `drop_chance` for one block.
    [[nodiscard]] bool rolls_drop(const ExplosionSpec& spec, math::LegacyRandomSource& rng) const;

    /// The box a caller has to look for entities in.
    ///
    /// `2 * power` on every side **plus one**. The extra block is not slack: an
    /// entity is found by its own box overlapping this one, and an entity whose
    /// feet are just outside still has a body inside. Dropping it makes the
    /// outermost ring of victims immune.
    [[nodiscard]] AABB entity_search_box(const ExplosionSpec& spec) const noexcept;

    /// How much of the way from an entity's box to the centre is unobstructed.
    ///
    /// The game samples the box on a grid whose spacing depends on the box's
    /// own size — `1 / (2 * side + 1)` per axis — and clips each sample against
    /// the collision shapes between it and the centre. A grid that ignores the
    /// box size gives a player and a ghast the same exposure.
    [[nodiscard]] f64 seen_percent(const world::LevelView& level, const Vec3d& centre,
                                   const AABB& box) const;

    /// Does anything's collision shape stand between these two points?
    ///
    /// The game's `clip` in COLLIDER mode: the shapes, not the cells. A ray
    /// that passes over a slab or through an open trapdoor is not blocked, and
    /// a version of this that stops at any non-air cell makes a player behind a
    /// carpet take nothing.
    [[nodiscard]] bool clipped(const world::LevelView& level, const Vec3d& from,
                               const Vec3d& to) const;

    /// What one entity takes from a charge.
    ///
    /// `position` is the entity's feet — the point the game measures distance
    /// and knockback direction from — and `eye_y` is the height the vertical
    /// component of the impulse is taken at, which is *not* the box's centre.
    /// It answers for **any** entity, not only a living one: a dropped item is
    /// an entity with a box and five health, and an explosion is what turns a
    /// floor covered in loot into an empty floor. What this does not decide is
    /// whether the entity is immune at all — an ender dragon and an area effect
    /// cloud ignore explosions outright — because that is a property of the
    /// type, and the type is the caller's business, not this module's.
    ///
    /// `knockback_dampener` is what Blast Protection takes off the impulse,
    /// from 0 to 1. It is a parameter rather than a lookup because enchantments
    /// are not this module's business, and passing 0 is the unenchanted case
    /// rather than a stand-in for a missing one.
    [[nodiscard]] ExplosionHit hit_entity(const world::LevelView& level,
                                          const ExplosionSpec& spec, const AABB& box,
                                          const Vec3d& position, f64 eye_y,
                                          f64 knockback_dampener = 0.0) const;

    /// Does this broken cell catch fire, for a charge that leaves fire behind?
    ///
    /// One roll in three, per cell of the crater. ⚠ **Not measured here**: the
    /// one in three is what the wiki's Explosion article states, and no bench
    /// in this campaign put a bed in the Nether. It is named so the next
    /// campaign knows what is owed rather than finding a plausible constant.
    /// A caller still has to check the cell is air with something solid under
    /// it — that part is the world's business, not this module's.
    [[nodiscard]] bool rolls_fire(const ExplosionSpec& spec,
                                  math::LegacyRandomSource& rng) const;

    /// How long a TNT block waits when another explosion sets it off.
    ///
    /// Not the full fuse: a chain of TNT staggers, which is what makes a stack
    /// of it throw blocks instead of vanishing in one flash. `fuse/8 +
    /// random(fuse/4)`, so 10 to 29 ticks for the 80-tick fuse.
    [[nodiscard]] i32 chained_fuse(i32 full_fuse, math::LegacyRandomSource& rng) const;

private:
    const registry::BlockRegistry* blocks_{nullptr};
    ExplosionConstants             constants_{};
};

/// The fuse of a TNT block lit by a player, redstone or fire, in ticks.
///
/// Measured on a real 1.20.1 server: the `Fuse` of the primed entity, read as
/// early as the console can reach it, tops out at 80 and converges from below
/// exactly the way every other delay in this repo does.
inline constexpr i32 kTntFuseTicks = 80;

/// The countdown a creeper runs once it starts swelling, in ticks.
inline constexpr i32 kCreeperFuseTicks = 30;

/// The powers, one per source.
inline constexpr f32 kTntPower            = 4.0F;
inline constexpr f32 kCreeperPower        = 3.0F;
inline constexpr f32 kChargedCreeperPower = 6.0F;
inline constexpr f32 kBedPower            = 5.0F;
inline constexpr f32 kRespawnAnchorPower  = 5.0F;
inline constexpr f32 kEndCrystalPower     = 6.0F;
inline constexpr f32 kGhastFireballPower  = 1.0F;
inline constexpr f32 kWitherSkullPower    = 1.0F;

/// The charge a named source makes, or nothing for a name that makes none.
///
/// A name this does not know is **refused**, not given a default power: an
/// unknown source that quietly exploded like TNT would be a bug nobody sees.
[[nodiscard]] std::span<const std::pair<std::string_view, f32>> explosion_sources() noexcept;

}  // namespace ov::gameplay
