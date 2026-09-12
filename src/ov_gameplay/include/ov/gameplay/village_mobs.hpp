// The mobs of a village that are not villagers: the iron golem.
//
// Vanilla's golem runs goals, not a brain: it hits enemies (every monster but
// the creeper), defends the village against a player the villagers dislike —
// reputation -100 or worse with a villager near the golem — strolls, and
// looks at players. The shape is the Minecraft Wiki's ("Iron Golem"); the
// summoning that makes one is the villagers' (brain/villager_brain.hpp),
// measured by scripts/measure_villager_life.py `golem` and `golem_meet`.
// docs/provenance/cerveaux.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/gameplay/mob_logic.hpp"

#include <string_view>

namespace ov::gameplay {

/// A golem defends against a player whose reputation with a villager near it
/// is at most this.
inline constexpr i32 kDefendReputation = -100;
/// The box round the golem the villagers and the players are sought in.
inline constexpr f64 kDefendReachHorizontal = 10.0;
inline constexpr f64 kDefendReachVertical   = 8.0;
/// One chance in this many a tick to look for an enemy (vanilla's random
/// interval of 5 for this target goal).
inline constexpr i32 kGolemTargetInterval = 5;

/// The kind of `minecraft:iron_golem`, or null for any other type.
[[nodiscard]] const MobKind* village_mob_kind(std::string_view type_name) noexcept;

/// The golem's goals.
void install_village_goals(GoalSelector& selector, const MobKind& kind, i32 look_type);

/// Take a disliked player as the target.
class DefendVillageGoal final : public Goal {
public:
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Target; }
    [[nodiscard]] std::string_view name() const noexcept override { return "defend_village"; }

private:
    i32 found_{0};
};

/// Take the nearest enemy within follow range as the target, sight not needed.
class GolemTargetGoal final : public Goal {
public:
    explicit GolemTargetGoal(f64 radius) noexcept : radius_{radius} {}
    [[nodiscard]] bool     can_use(GoalContext& context) override;
    [[nodiscard]] bool     can_continue_to_use(GoalContext& context) override;
    void                   start(GoalContext& context) override;
    void                   stop(GoalContext& context) override;
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Target; }
    [[nodiscard]] std::string_view name() const noexcept override { return "golem_target"; }

private:
    f64                  radius_{16.0};
    entity::EntityHandle found_{entity::kNoEntity};
};

}  // namespace ov::gameplay
