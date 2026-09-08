#include "ov/gameplay/experience.hpp"

#include <algorithm>
#include <array>

namespace ov::gameplay {
namespace {

// The denominations an orb comes in.
//
// Recovered by counting: a player killed at a known level scatters orbs, and
// the `Value` of each one was read back from the server. Forty-eight points
// came out as 37, 7, 3 and 1 — which is a greedy split over exactly this list
// and over no other list that also produces four orbs.
constexpr std::array<i32, 11> kOrbValues{2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3, 1};

/// Seven per level, capped at a hundred.
constexpr i32 kExperiencePerLevel = 7;
constexpr i32 kDeathExperienceCap = 100;

}  // namespace

i32 experience_to_next_level(i32 level, const ExperienceCurve& curve) noexcept {
    if (level < 0) {
        return 0;
    }
    if (level < curve.first_join) {
        return curve.low_slope * level + curve.low_offset;
    }
    if (level < curve.second_join) {
        return curve.mid_slope * level + curve.mid_offset;
    }
    return curve.high_slope * level + curve.high_offset;
}

i32 total_experience_for_level(i32 level, const ExperienceCurve& curve) noexcept {
    i32 total = 0;
    for (i32 i = 0; i < level; ++i) {
        total += experience_to_next_level(i, curve);
    }
    return total;
}

LevelProgress level_for_total(i32 total, const ExperienceCurve& curve) noexcept {
    LevelProgress progress;
    if (total <= 0) {
        return progress;
    }
    i32 remaining = total;
    while (true) {
        const i32 cost = experience_to_next_level(progress.level, curve);
        if (cost <= 0 || remaining < cost) {
            break;
        }
        remaining -= cost;
        ++progress.level;
    }
    progress.points_into_level = remaining;
    const i32 cost             = experience_to_next_level(progress.level, curve);
    progress.bar = cost > 0 ? static_cast<f32>(remaining) / static_cast<f32>(cost) : 0.0F;
    return progress;
}

i32 death_experience(i32 level) noexcept {
    if (level <= 0) {
        return 0;
    }
    return std::min(level * kExperiencePerLevel, kDeathExperienceCap);
}

std::span<const i32> orb_denominations() noexcept { return kOrbValues; }

i32 orb_value_for(i32 amount) noexcept {
    if (amount <= 0) {
        return 0;
    }
    for (const i32 value : kOrbValues) {
        if (amount >= value) {
            return value;
        }
    }
    return 1;
}

usize split_into_orbs(i32 amount, std::span<i32> out) noexcept {
    usize written = 0;
    while (amount > 0 && written < out.size()) {
        const i32 value = orb_value_for(amount);
        out[written]    = value;
        ++written;
        amount -= value;
    }
    return written;
}

bool orbs_can_merge(const OrbState& a, const OrbState& b, f64 distance_squared) noexcept {
    // Close enough, and close enough in age. The age test is not decoration: a
    // pile that keeps absorbing new orbs would otherwise inherit the newest
    // one's lifetime and never expire.
    constexpr f64 kMergeRangeSquared = 1.0;
    constexpr i32 kMergeAgeWindow    = 100;
    if (distance_squared > kMergeRangeSquared) {
        return false;
    }
    const i32 age_difference = a.age > b.age ? a.age - b.age : b.age - a.age;
    return age_difference <= kMergeAgeWindow;
}

}  // namespace ov::gameplay
