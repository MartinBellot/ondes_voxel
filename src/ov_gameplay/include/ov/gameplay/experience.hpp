// Experience: a curve in three pieces, and the orbs that walk it.
//
// The level cost is not one formula. It is three, joined at 16 and 31, and the
// joins are what make the early game quick and enchanting expensive. Getting
// the pieces right matters more than getting any one of them right: a single
// quadratic fitted through the whole range is close everywhere and wrong
// everywhere, and the error only shows up at the anvil.
//
// The costs below were measured, level by level, on a real 1.20.1 server:
// `experience set <n> levels` followed by `experience add 1 points` leaves the
// experience bar at exactly one over that level's cost, and reading it back
// gives the cost as an integer. The cumulative totals the three formulas
// predict were then fed back as raw points and the resulting level checked,
// which tests the curve as a whole rather than one entry at a time. See
// docs/provenance/survie.md.
#pragma once

#include "ov/base/types.hpp"

#include <span>

namespace ov::gameplay {

/// The joins, and the three lines that meet at them.
///
/// Cost from `level` to `level + 1` is `a * level + b`, with a different pair
/// on each side of the joins. Every one of the 41 costs from 0 to 40 was
/// measured and all 41 fall on these lines.
struct ExperienceCurve {
    i32 first_join{16};
    i32 second_join{31};

    i32 low_slope{2};
    i32 low_offset{7};

    i32 mid_slope{5};
    i32 mid_offset{-38};

    i32 high_slope{9};
    i32 high_offset{-158};
};

/// Points needed to go from `level` to `level + 1`.
[[nodiscard]] i32 experience_to_next_level(i32 level, const ExperienceCurve& curve) noexcept;

/// Total points to reach `level` from nothing.
///
/// Summed from the per-level costs rather than from a closed form. The closed
/// forms exist and are quadratics, but they are three quadratics that have to
/// agree with the three lines at both joins, and an implementation that gets
/// one constant wrong is off by a fixed amount at every level above it — a bug
/// that never fails a small test.
[[nodiscard]] i32 total_experience_for_level(i32 level, const ExperienceCurve& curve) noexcept;

/// Where a raw point total sits on the curve.
struct LevelProgress {
    i32 level{0};
    /// Points into the current level.
    i32 points_into_level{0};
    /// The bar the client draws, 0..1.
    f32 bar{0.0F};
};

[[nodiscard]] LevelProgress level_for_total(i32 total, const ExperienceCurve& curve) noexcept;

/// What a player drops on death: seven per level, never more than a hundred.
///
/// The cap is the reason dying at level 60 and dying at level 15 cost almost
/// the same in practice, and it is the single most consequential number in the
/// system.
[[nodiscard]] i32 death_experience(i32 level) noexcept;

/// The denominations an orb comes in.
///
/// A lump of experience is not one orb: it is split greedily into the largest
/// denominations that fit, which is why a death scatters four or five orbs
/// rather than a hundred. The list is the game's own, confirmed by counting the
/// orbs a death produced and reading the `Value` of each.
[[nodiscard]] std::span<const i32> orb_denominations() noexcept;

/// The value of the single orb the game would make for `amount`.
[[nodiscard]] i32 orb_value_for(i32 amount) noexcept;

/// Split `amount` into orbs, largest first.
///
/// Writes into `out` and returns how many were written. Nothing allocates, so
/// this is callable from the tick; a caller whose buffer is too small gets the
/// orbs that fit and a count that says so.
[[nodiscard]] usize split_into_orbs(i32 amount, std::span<i32> out) noexcept;

/// How an orb behaves once it exists.
struct OrbConstants {
    /// How close a player must be before an orb starts moving toward them.
    f64 follow_range{8.0};

    /// How hard it accelerates. Applied along the unit vector to the player,
    /// scaled by how close it already is.
    f64 follow_speed{0.1};

    /// Within this, the player has it.
    f64 pickup_range{1.0};

    /// Ticks before a player who just dropped it may pick it up again.
    i32 pickup_delay{0};

    /// Ticks before it disappears on its own. Five minutes, the same as a
    /// dropped item.
    i32 lifetime_ticks{6000};

    /// Ticks between one player picking orbs up. Two, which is what makes a
    /// pile of orbs take a moment to collect rather than vanishing at once.
    i32 collection_interval{2};
};

/// One orb, as far as this layer is concerned.
struct OrbState {
    i32 value{1};
    i32 age{0};
    /// Counts down; while it is above zero this orb ignores players.
    i32 delay{0};
};

/// Whether two orbs may become one.
///
/// Vanilla merges orbs that are close enough and not too different in age, so
/// that a pile does not cost a thousand entities. Both halves matter: merging
/// on distance alone eventually welds together orbs that were dropped minutes
/// apart, and their combined lifetime becomes the younger one's.
[[nodiscard]] bool orbs_can_merge(const OrbState& a, const OrbState& b,
                                  f64 distance_squared) noexcept;

}  // namespace ov::gameplay
