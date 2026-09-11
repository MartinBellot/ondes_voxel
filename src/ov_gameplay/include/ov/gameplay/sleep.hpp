// Beds and sleep, as rules: which bed a click means, whether its sleeper may
// lie down, where they stand up, and what the night skip does to the clock.
//
// Who is asleep, who else is in the world and which monsters stand where are
// the server's (ov_server/src/weather_session.hpp); this file is the part a
// test can hold still. Every rule was specified from the Minecraft Wiki's Bed
// and Sleep articles and then put against the 1.20.1 server by
// scripts/measure_weather.py `sleep` — docs/provenance/meteo-sommeil.md has
// what the capture said, including where it disagreed with the wiki.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <string_view>

namespace ov::gameplay {

/// Why a player did not lie down.
enum class BedProblem : u8 {
    None,
    /// Not a natural dimension. Never reached through a bed: a bed there
    /// explodes first.
    NotPossibleHere,
    /// Day, as the sky darkening says it (under four).
    NotPossibleNow,
    TooFarAway,
    /// Something suffocating above either half.
    Obstructed,
    /// A monster in the box around the bed.
    NotSafe,
    /// Someone else is in it.
    Occupied,
    /// Already asleep, or dead.
    Other,
};

/// The translation key the action bar shows, or empty when the game shows
/// nothing for this problem.
[[nodiscard]] std::string_view bed_message_key(BedProblem problem) noexcept;

/// Everything the verdict depends on, gathered by the caller.
struct SleepCheck {
    bool bed_works{true};  // the dimension lets beds set a spawn; otherwise it explodes
    bool occupied{false};
    bool sleeping_or_dead{false};
    bool natural{true};
    bool in_range{true};
    bool obstructed{false};
    bool is_day{false};
    bool creative{false};
    bool monsters_near{false};
};

struct SleepVerdict {
    BedProblem problem{BedProblem::None};
    /// The respawn point moves to this bed. True as soon as range and
    /// obstruction pass — before the time of day is looked at, which is why
    /// "you can only sleep at night" follows "respawn point set".
    bool sets_spawn{false};
    /// The bed is in a dimension where beds do not work, and blows up.
    bool explodes{false};
};

/// The game's order: the dimension (explosion), occupied, then the player's
/// own checks — asleep, natural, range, obstruction, spawn, day, monsters.
[[nodiscard]] SleepVerdict judge_sleep(const SleepCheck& check) noexcept;

/// Sleeping needs a darkened sky: four or more. Clear sky: 12542..23459.
[[nodiscard]] constexpr bool is_day(u8 sky_darken) noexcept { return sky_darken < 4; }

/// The next morning: the day time rounded up to the next multiple of 24000.
[[nodiscard]] i64 wake_day_time(i64 day_time) noexcept;

/// How many must sleep: at least one, else ceil(players * percentage / 100).
[[nodiscard]] i32 sleepers_needed(i32 active_players, i32 percentage) noexcept;

/// A player who has lain this long counts for the skip.
inline constexpr i32 kSleepTicksNeeded = 100;
/// A bed in the Nether or the End.
inline constexpr f32 kBedExplosionPower = 5.0F;

/// Does a monster of this type keep a player awake? Zombified piglins only
/// when angry at that player — never on this server, which has no anger yet.
[[nodiscard]] bool prevents_rest(std::string_view entity_type) noexcept;

/// One bed, both halves resolved.
struct Bed {
    BlockPos head{};
    BlockPos foot{};
    /// The direction from foot to head.
    Direction facing{Direction::North};
    bool      occupied{false};
};

class BedRules {
public:
    explicit BedRules(const registry::BlockRegistry& blocks);

    [[nodiscard]] bool is_bed(registry::BlockStateId state) const noexcept;

    /// The bed a click on either half means. Nothing when the state is not a
    /// bed, or when its other half is missing.
    [[nodiscard]] std::optional<Bed> bed_at(const world::LevelView& level, BlockPos clicked) const;

    /// Within three blocks horizontally and two vertically of the bottom
    /// centre of either half.
    [[nodiscard]] static bool in_range(const Vec3d& feet, const Bed& bed) noexcept;

    /// A suffocating block above either half.
    [[nodiscard]] bool obstructed(const world::LevelView& level, const Bed& bed) const;

    /// Does this state suffocate? A full collision cube that blocks motion,
    /// minus the blocks the game says never suffocate (glass, leaves, ice…).
    [[nodiscard]] bool suffocates(registry::BlockStateId state) const;

    /// Monsters are looked for in this box: 8 blocks around the head's bottom
    /// centre, 5 up and down.
    [[nodiscard]] static AABB monster_box(const Bed& bed) noexcept;

    /// Where a sleeper lies: the head block, 0.6875 up.
    [[nodiscard]] static Vec3d sleeping_position(const Bed& bed) noexcept;

    /// The same bed state with `occupied` set.
    [[nodiscard]] registry::BlockStateId with_occupied(registry::BlockStateId state,
                                                       bool                   occupied) const;

    /// Where a sleeper stands up: the first of twelve cells around the bed —
    /// beside it on the side away from where they look, then round it, then on
    /// it — with a floor under 1 block high and room for a player. Nothing
    /// when none fits; the caller then puts them above the head.
    [[nodiscard]] std::optional<Vec3d> stand_up_position(const world::LevelView& level,
                                                         const Bed& bed, f32 yaw) const;

    /// The yaw a player who stood up at `stand` faces: towards the bed.
    [[nodiscard]] static f32 stand_up_yaw(const Bed& bed, const Vec3d& stand) noexcept;

private:
    [[nodiscard]] bool passable(registry::BlockStateId state) const;
    [[nodiscard]] std::optional<f64> floor_height(const world::LevelView& level, BlockPos cell) const;
    [[nodiscard]] bool fits(const world::LevelView& level, const Vec3d& feet) const;

    const registry::BlockRegistry* blocks_;
};

}  // namespace ov::gameplay
