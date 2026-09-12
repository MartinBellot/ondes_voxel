// ── mobs-5 ── The enderman's rules: what makes it angry, where it lands when
// it teleports, what it may carry.
//
// Every number is measured on the real 1.20.1 server by
// scripts/measure_hostile.py (campaigns `enderman`, `enderman2`,
// `enderman3`) or, where a campaign could not tell, taken from the wiki's
// Enderman page and named as such — docs/provenance/mobs-5.md.
//
// Rules only: the session that runs them (the server's endermen.cpp) owns the
// anger timers, the carried block and the packets.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"

#include <optional>

namespace ov::gameplay {

// ── The stare ───────────────────────────────────────────────────────────────

/// How far off the eyes a gaze may be and still provoke: the look provokes when
/// `cos(angle) > 1 − kStareSlack / distance`. Measured (`enderman2`, cone): at
/// 8 blocks anger up to 4° above the eyes and not at 5°, at 16 blocks up to 3°
/// and not at 4° — which brackets the slack in [0.022, 0.030] and rules out a
/// fixed angle. 0.025 is inside the bracket.
inline constexpr f64 kStareSlack = 0.025;

/// Beyond this a stare provokes nothing: the wiki's 64 blocks, and the
/// enderman's measured `follow_range` (entities.json).
inline constexpr f64 kStareRange = 64.0;

/// A player's view vector from their yaw and pitch, in degrees, as the
/// protocol carries them (yaw 0 faces +z, pitch 90 faces down).
[[nodiscard]] Vec3d view_vector(f32 yaw, f32 pitch) noexcept;

/// Does a look from `eye` along `view` meet the eyes at `target_eye`? The
/// angle alone: whether blocks are in the way is `has_clear_line`'s business.
[[nodiscard]] bool looks_at(const Vec3d& eye, const Vec3d& view, const Vec3d& target_eye,
                            f64 slack = kStareSlack) noexcept;

// ── Anger ───────────────────────────────────────────────────────────────────

/// How long a provoked enderman stays angry, drawn once per provocation:
/// 20 to 39 seconds (the wiki's neutral-mob anger). Measured: `AngerTime` read
/// 709 about 50 ticks after the stare, inside the draw.
inline constexpr i32 kAngerMinTicks = 400;
inline constexpr i32 kAngerMaxTicks = 780;
[[nodiscard]] i32 draw_anger_ticks(math::LegacyRandomSource& random) noexcept;

// ── Teleporting ─────────────────────────────────────────────────────────────

/// A random teleport lands within this many blocks of the start on each axis.
/// The wiki's 64 × 64 × 64 cube; measured (`enderman2`, sixteen hurt hops): the
/// single hops land within ±32 per axis, flat ground keeping y.
inline constexpr f64 kTeleportHalfSpan = 32.0;

/// Attempts before a teleport gives up — the wiki's 64.
inline constexpr i32 kTeleportAttempts = 64;

/// One attempt: a point drawn in the cube, lowered onto the first block below
/// that stops motion. Nullopt when it finds no ground above `min_y`, lands its
/// feet in a liquid, or the box does not fit there.
[[nodiscard]] std::optional<Vec3d> teleport_attempt(const CollisionWorld& collisions,
                                                    const Vec3d& from, f64 width, f64 height,
                                                    i32 min_y, math::LegacyRandomSource& random);

/// Up to `kTeleportAttempts` attempts; the first that lands.
[[nodiscard]] std::optional<Vec3d> random_teleport(const CollisionWorld& collisions,
                                                   const Vec3d& from, f64 width, f64 height,
                                                   i32 min_y, math::LegacyRandomSource& random);

// ── Water and rain ──────────────────────────────────────────────────────────

/// What water and rain do to an enderman each tick it is wet: 1 damage of the
/// `drown` type, which its damage window lets through once every ten ticks.
/// Measured (`enderman`, water: 40 → 39 then a teleport; `enderman2`, rain:
/// 40 → 29 in 105 ticks, teleporting all the while).
inline constexpr f32 kWetDamage = 1.0F;

// ── Carrying ────────────────────────────────────────────────────────────────

/// The metadata an enderman carries, measured on the wire (`enderman2`):
/// 16 the carried block (optional block state, 0 for none), 17 screaming (the
/// anger shown), 18 stared at.
inline constexpr u8 kCarriedBlockIndex = 16;
inline constexpr u8 kScreamingIndex    = 17;
inline constexpr u8 kStaredAtIndex     = 18;

/// The chance per tick that an empty-handed enderman tries to take a block at
/// `pick_target`, and that a carrying one tries to put its block down at
/// `place_target`. **Calibrated**, not read: the wiki's 1/20 and 1/2000 do not
/// reproduce `enderman3` — 7 takes of 8 in 1 179 ticks from a field of
/// dandelions at the feet (≈ 0.0021 a tick, a third of the take region being
/// flowers), 4 puts of 8 in 1 989 ticks on bare grass (≈ 0.00037 a tick, half
/// the put region being free ground). Eight endermen each: a wide interval.
inline constexpr f32 kPickChance  = 1.0F / 160.0F;
inline constexpr f32 kPlaceChance = 1.0F / 1340.0F;

/// Where a take looks: x and z within [−2, 2) of the feet, y the feet's block
/// and the two above — the wiki's 4 × 3 × 4, starting at the feet. Measured
/// (`enderman2`): on bare flat grass nothing is taken in 80 s, the one
/// holdable block being under the feet.
[[nodiscard]] BlockPos pick_target(const Vec3d& feet, math::LegacyRandomSource& random) noexcept;

/// Where a put looks: x and z within [−1, 1), y the feet's block or the one
/// above. The block there must be air and the one below sturdy on top.
[[nodiscard]] BlockPos place_target(const Vec3d& feet, math::LegacyRandomSource& random) noexcept;

}  // namespace ov::gameplay
