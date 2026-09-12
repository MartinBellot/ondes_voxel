#include "ov/gameplay/village_mobs.hpp"

#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/villager.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

// `movement_speed` 0.25 and `follow_range` 16: the golem's attributes
// (normalized/entities.json). Stroll 0.6 and chase 1.0: the wiki's goals.
const MobKind kIronGolem{
    .type_name      = "minecraft:iron_golem",
    .category       = MobCategory::Misc,
    .movement_speed = 0.25,
    .stroll         = 0.6,
    .chase          = 1.0,
    .melee          = true,
    .follow_range   = 16.0,
};

[[nodiscard]] bool in_box(const Vec3d& a, const Vec3d& b) noexcept {
    return std::abs(a.x - b.x) <= kDefendReachHorizontal &&
           std::abs(a.y - b.y) <= kDefendReachVertical &&
           std::abs(a.z - b.z) <= kDefendReachHorizontal;
}

}  // namespace

const MobKind* village_mob_kind(std::string_view type_name) noexcept {
    return type_name == kIronGolem.type_name ? &kIronGolem : nullptr;
}

void install_village_goals(GoalSelector& selector, const MobKind& kind, i32 look_type) {
    selector.add(1, std::make_unique<MeleeAttackGoal>(kind.speed(kind.chase), kMeleeCooldownTicks,
                                                      0.0, true));
    selector.add(6, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
    selector.add(7, std::make_unique<LookAtEntityGoal>(look_type, 6.0, 0.02F));
    selector.add(8, std::make_unique<RandomLookGoal>());
    selector.add(1, std::make_unique<DefendVillageGoal>());
    selector.add(3, std::make_unique<GolemTargetGoal>(kind.follow_range));
}

// ── Defending the village ───────────────────────────────────────────────────

bool DefendVillageGoal::can_use(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.villagers == nullptr || context.entities == nullptr) {
        return false;
    }
    for (const ReputationSubject& player : context.villagers->players) {
        // Only a player that may be attacked at all (survival, not Peaceful):
        // the quarries are exactly those.
        if (!in_box(player.feet, self->position) ||
            find_quarry(context.quarries, player.network_id) == nullptr) {
            continue;
        }
        for (const entity::EntityHandle h : context.entities->handles()) {
            const entity::EntityState* s = context.entities->state(h);
            MobBrain*                  b = mob_brain_of(*context.entities, h);
            if (s == nullptr || b == nullptr || !b->villager.active || b->villager.wandering ||
                s->removed || !in_box(s->position, self->position)) {
                continue;
            }
            if (b->villager.gossips.reputation(player.uuid) <= kDefendReputation) {
                found_ = player.network_id;
                return true;
            }
        }
    }
    return false;
}

bool DefendVillageGoal::can_continue_to_use(GoalContext& context) {
    const entity::EntityState* self   = context.state();
    const Quarry*              quarry = find_quarry(context.quarries, found_);
    if (self == nullptr || quarry == nullptr || context.brain == nullptr ||
        context.brain->target_player != found_) {
        return false;
    }
    const f64 dx = quarry->feet.x - self->position.x;
    const f64 dz = quarry->feet.z - self->position.z;
    return dx * dx + dz * dz <= kIronGolem.follow_range * kIronGolem.follow_range;
}

void DefendVillageGoal::start(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target        = entity::kNoEntity;
        context.brain->target_player = found_;
    }
}

void DefendVillageGoal::stop(GoalContext& context) {
    if (context.brain != nullptr && context.brain->target_player == found_) {
        context.brain->target_player = 0;
    }
    found_ = 0;
}

// ── Enemies ─────────────────────────────────────────────────────────────────

bool GolemTargetGoal::can_use(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.villagers == nullptr || context.entities == nullptr ||
        context.random == nullptr || context.villagers->golem_quarries.empty() ||
        context.random->next_int(kGolemTargetInterval) != 0) {
        return false;
    }
    const std::span<const i32> enemies = context.villagers->golem_quarries;
    f64                        best    = radius_ * radius_;
    found_                             = entity::kNoEntity;
    for (const entity::EntityHandle h : context.entities->handles()) {
        const entity::EntityState* s = context.entities->state(h);
        if (s == nullptr || s->removed || s->health <= 0.0F || h == context.self ||
            std::ranges::find(enemies, s->type) == enemies.end()) {
            continue;
        }
        const f64 dx = s->position.x - self->position.x;
        const f64 dy = s->position.y - self->position.y;
        const f64 dz = s->position.z - self->position.z;
        const f64 d  = dx * dx + dy * dy + dz * dz;
        if (d <= best) {
            best   = d;
            found_ = h;
        }
    }
    return found_ != entity::kNoEntity;
}

bool GolemTargetGoal::can_continue_to_use(GoalContext& context) {
    const entity::EntityState* self = context.state();
    const entity::EntityState* s    = context.entities != nullptr ? context.entities->state(found_)
                                                                  : nullptr;
    if (self == nullptr || s == nullptr || s->removed || s->health <= 0.0F ||
        context.brain == nullptr || context.brain->target != found_) {
        return false;
    }
    const f64 dx = s->position.x - self->position.x;
    const f64 dz = s->position.z - self->position.z;
    return dx * dx + dz * dz <= radius_ * radius_;
}

void GolemTargetGoal::start(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target        = found_;
        context.brain->target_player = 0;
    }
}

void GolemTargetGoal::stop(GoalContext& context) {
    if (context.brain != nullptr && context.brain->target == found_) {
        context.brain->target = entity::kNoEntity;
    }
    found_ = entity::kNoEntity;
}

}  // namespace ov::gameplay
