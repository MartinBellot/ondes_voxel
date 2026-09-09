#include "ov/gameplay/mob_logic.hpp"

#include <array>
#include <cmath>

namespace ov::gameplay {
namespace {

/// The eight species, and nothing about them that is not a number or a flag.
///
/// `walk_speed` is the measured `movement_speed` attribute halved. Only the
/// zombie's has been checked against the game — 0.11419 blocks a tick measured
/// against 0.115 predicted, which is 0.7 % — and the other seven are derived
/// from the same relation rather than measured. docs/provenance/mobs.md says so
/// there too, because a table that looks uniform is exactly how a derived
/// number gets mistaken for a measured one.
constexpr std::array<MobKind, 8> kKinds{{
    // type name                category                    speed    doors  sun   hostile panic breed
    {"minecraft:zombie",   MobCategory::Monster,  0.115,  true,  true,  true,  false, false},
    {"minecraft:skeleton",  MobCategory::Monster,  0.125,  false, true,  true,  false, false},
    {"minecraft:creeper",   MobCategory::Monster,  0.125,  false, false, true,  false, false},
    {"minecraft:spider",    MobCategory::Monster,  0.150,  false, false, true,  false, false},
    {"minecraft:cow",       MobCategory::Creature, 0.100,  false, false, false, true,  true},
    {"minecraft:pig",       MobCategory::Creature, 0.125,  false, false, false, true,  true},
    {"minecraft:sheep",     MobCategory::Creature, 0.115,  false, false, false, true,  true},
    {"minecraft:chicken",   MobCategory::Creature, 0.125,  false, false, false, true,  true},
}};

}  // namespace

std::span<const MobKind> mob_kinds() noexcept { return kKinds; }

const MobKind* mob_kind(std::string_view type_name) noexcept {
    for (const MobKind& kind : kKinds) {
        if (kind.type_name == type_name) {
            return &kind;
        }
    }
    return nullptr;
}

void FallingMob::tick(entity::EntityWorld& world, entity::EntityHandle self,
                      const entity::TickContext& context) {
    const MobContext* mob = mob_context(context);
    if (mob == nullptr || mob->world == nullptr) {
        // No world to fall through. Returning rather than falling anyway: an
        // entity that moves without collision walks out of the ground, and a
        // caller that forgot the context should see a mob that does not move
        // rather than one that sinks.
        return;
    }
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr) {
        return;
    }
    *state = step_entity(*state, constants_, *mob->world);
}

// ── The goal list ───────────────────────────────────────────────────────────
//
// The priorities are the interesting part of this file. They are what decides
// that a frightened cow stops grazing, that a chasing zombie stops wandering,
// and that both of them keep looking around while they do it — since Look is a
// different control from Move and the two never compete.

void install_goals(GoalSelector& selector, const MobKind& kind, i32 player_type) {
    // 0 — staying alive beats everything. A mob that drowns while deciding
    // where to wander is a mob nobody sees again.
    selector.add(0, std::make_unique<FloatGoal>());

    if (kind.panics) {
        selector.add(1, std::make_unique<PanicGoal>(kind.walk_speed * 1.25));
    }
    if (kind.avoids_sun) {
        selector.add(2, std::make_unique<AvoidSunGoal>(kind.walk_speed));
    }
    if (kind.hostile) {
        selector.add(3, std::make_unique<MeleeAttackGoal>(kind.walk_speed));
        // Target selection holds only the Target control, so it runs alongside
        // whatever is moving the body. That separation is the whole reason
        // Target is a flag of its own.
        selector.add(3, std::make_unique<NearestAttackableTargetGoal>(player_type, 35.0, true));
    }
    if (kind.breeds) {
        selector.add(4, std::make_unique<BreedGoal>(kind.walk_speed));
        selector.add(5, std::make_unique<FollowParentGoal>(kind.walk_speed * 1.1));
    }

    // 6 and 7 — what a mob does when nothing else is happening, which is most
    // of the time and therefore most of what anyone actually watches.
    selector.add(6, std::make_unique<RandomStrollGoal>(kind.walk_speed));
    selector.add(7, std::make_unique<LookAtEntityGoal>(player_type, 8.0, 0.02F));
    selector.add(8, std::make_unique<RandomLookGoal>());
}

// ── Mob ─────────────────────────────────────────────────────────────────────

Mob::Mob(const MobKind& kind, f32 width, f32 height, i64 seed)
    : kind_{&kind}, brain_{2048}, random_{seed} {
    brain_.size                 = MobSize::from_box(width, height);
    brain_.abilities.opens_doors = kind.opens_doors;
    brain_.abilities.avoids_sun  = kind.avoids_sun;
    brain_.abilities.enters_water = true;

    // The player's entity type id is not known here — ov_gameplay has the
    // registries but not the caller's idea of which entities are players — so
    // the goals are given -1, meaning "anything". A caller that wants a zombie
    // to chase only players installs its own list.
    install_goals(goals_, kind, -1);

    // Kept so `frighten` can reach it: a damage event is not something a goal
    // can see for itself. Borrowed — the selector owns it and outlives the
    // pointer.
    panic_ = kind.panics ? static_cast<PanicGoal*>(goals_.find("panic")) : nullptr;
}

void Mob::frighten(i32 ticks) noexcept {
    if (panic_ != nullptr) {
        panic_->frighten(ticks);
    }
}

void Mob::tick(entity::EntityWorld& world, entity::EntityHandle self,
               const entity::TickContext& context) {
    const MobContext* mob = mob_context(context);
    if (mob == nullptr || mob->world == nullptr) {
        return;
    }
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr) {
        return;
    }

    // The brain runs only when there is a world to read. Without a level a mob
    // still falls — which is the floor `FallingMob` established — but it does
    // not decide anything, because every decision here needs blocks.
    if (mob->level != nullptr) {
        brain_.wants_move = false;
        brain_.wants_jump = false;
        brain_.has_look   = false;

        GoalContext goal_context;
        goal_context.level      = mob->level;
        goal_context.collisions = mob->world;
        goal_context.entities   = &world;
        goal_context.self       = self;
        goal_context.brain      = &brain_;
        goal_context.tick       = context.tick;
        goal_context.random     = &random_;
        goals_.tick(goal_context);

        // Turn the brain's intent into velocity. The goals never touch
        // velocity themselves: two goals that both pushed would add up, and a
        // panicking cow would move at twice the speed of a calm one.
        if (brain_.wants_move) {
            Vec3d waypoint{};
            if (brain_.follower.next_waypoint(state->position, state->width, waypoint)) {
                const f64 dx     = waypoint.x - state->position.x;
                const f64 dz     = waypoint.z - state->position.z;
                const f64 length = std::sqrt(dx * dx + dz * dz);
                if (length > 1e-6) {
                    const f64 speed = brain_.speed > 0.0 ? brain_.speed : kind_->walk_speed;
                    // Divided by the friction the step is about to apply, so
                    // that what comes out is `speed` blocks of *displacement*.
                    // Without this the mob moves at 0.546 of the number in the
                    // table — and since the table's number is the measured one,
                    // every mob in the world would be 45 % too slow while the
                    // constant it was compared against still read correct.
                    const f64 friction =
                        state->on_ground ? motion_.air_drag * motion_.default_slipperiness
                                         : motion_.air_drag;
                    state->velocity.x = dx / length * speed / friction;
                    state->velocity.z = dz / length * speed / friction;
                    // Face where it is going. A mob that walks sideways is the
                    // most obvious possible sign that nothing is steering it.
                    state->yaw =
                        static_cast<f32>(std::atan2(-dx, dz) * 180.0 / 3.14159265358979323846);
                    state->head_yaw = state->yaw;
                }
                // A step up is a jump. The path already refused any rise it
                // could not make, so this is unconditional rather than a guess.
                if (waypoint.y > state->position.y + 0.1 && state->on_ground) {
                    brain_.wants_jump = true;
                }
            } else {
                brain_.follower.clear();
            }
        }
        if (brain_.has_look) {
            const f64 dx = brain_.look_at.x - state->position.x;
            const f64 dz = brain_.look_at.z - state->position.z;
            const f64 dy =
                brain_.look_at.y - (state->position.y + static_cast<f64>(state->eye_height));
            const f64 flat = std::sqrt(dx * dx + dz * dz);
            state->head_yaw =
                static_cast<f32>(std::atan2(-dx, dz) * 180.0 / 3.14159265358979323846);
            state->pitch =
                static_cast<f32>(-std::atan2(dy, flat) * 180.0 / 3.14159265358979323846);
        }
        if (brain_.wants_jump && state->on_ground) {
            // The measured jump impulse of a player. Not measured for a mob,
            // and said so here rather than in a table where it would look like
            // it had been: what it has to do is clear one block, and it does.
            state->velocity.y = 0.42;
            state->on_ground  = false;
        }
    }

    *state = step_entity(*state, motion_, *mob->world);
}

}  // namespace ov::gameplay
