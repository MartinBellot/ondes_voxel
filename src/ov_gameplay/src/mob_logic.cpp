#include "ov/gameplay/mob_logic.hpp"

#include "ov/gameplay/breeding.hpp"  // ── husbandry ──
#include "ov/gameplay/villager.hpp"  // ── villagers ──
#include "ov/gameplay/tame.hpp"      // ── tame ──

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::gameplay {

// ── mobs-2 ── The species table, `mob_kinds` and `mob_kind` live in
// mob_species.cpp: every speed there is an attribute and a measured modifier.

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

void install_goals(GoalSelector& selector, const MobKind& kind, i32 look_type,
                   i32 quarry_type) {
    // ── villagers ── a list of their own (villager.cpp)
    if (villager_mob_kind(kind.type_name) != nullptr) {
        install_villager_goals(selector, kind, look_type);
        return;
    }
    // ── tame ── a list of their own (tame.cpp)
    if (const TameKind* tame = tame_kind(kind.type_name); tame != nullptr && tame->own_goals) {
        install_tame_goals(selector, kind, *tame, look_type, quarry_type);
        return;
    }
    if (kind.type_name == "minecraft:creeper") {
        install_creeper_fear(selector, kind);  // it keeps away from cats
    }
    // 0 — staying alive beats everything. A mob that drowns while deciding
    // where to wander is a mob nobody sees again.
    selector.add(0, std::make_unique<FloatGoal>());

    // ── mobs-2 ── Every speed below is `kind.speed(modifier)`: the attribute
    // times the goal's measured modifier, through the walk law.
    if (kind.panics) {
        selector.add(1, std::make_unique<PanicGoal>(kind.speed(kind.panic)));
    }
    if (kind.avoids_sun) {
        selector.add(2, std::make_unique<AvoidSunGoal>(kind.speed(kind.avoid_sun)));
    }
    if (kind.hostile) {
        // ── mobs-3 ── `kind.melee`: a swing in reach becomes a MobAttack.
        selector.add(3, std::make_unique<MeleeAttackGoal>(kind.speed(kind.chase),
                                                          kMeleeCooldownTicks, kind.hold_at,
                                                          kind.melee));
        // Target selection holds only the Target control, so it runs alongside
        // whatever is moving the body. That separation is the whole reason
        // Target is a flag of its own.
        // 35 blocks: the measured `follow_range` of a zombie, and inside the
        // bracket the acquisition campaign put the real radius in — chases at
        // 32, does not at 40. See docs/provenance/mobs.md section 3.
        // ── mobs-3 ── priority 2, so a player seen takes the Target control
        // from a villager being hunted (priority 3, no line of sight needed).
        // The radius is the species' measured `follow_range`: 35 for a zombie
        // (the bracket above), 16 for a creeper, a spider or a skeleton.
        selector.add(2, std::make_unique<NearestAttackableTargetGoal>(quarry_type,
                                                                      kind.follow_range, true));
        if (kind.hunts_villagers) {
            selector.add(3, std::make_unique<NearestAttackableTargetGoal>(
                                kVillagerQuarry, kind.follow_range, false));
        }
    }
    if (kind.breeds) {
        // ── husbandry ── Breed before tempt before following a parent, which
        // is the order the game's animals show: a cow in love ignores the
        // wheat it has just eaten and walks to its mate.
        selector.add(3, std::make_unique<BreedGoal>(kind.speed(1.0), kMateReach));  // ── mobs-2 ──
        if (const AnimalKind* animal = animal_kind(kind.type_name)) {
            // Measured: cow 0.1347, sheep 0.1380, pig 0.1936, chicken 0.1349
            // blocks a tick, which the law gives from 1.25, 1.1, 1.2 and 1.0.
            selector.add(4, std::make_unique<TemptGoal>(*animal, kind.speed(animal->tempt_speed)));
        }
        selector.add(5, std::make_unique<FollowParentGoal>(kind.speed(kind.follow_parent)));
        if (const AnimalKind* animal = animal_kind(kind.type_name);
            animal != nullptr && animal->shearable) {
            selector.add(5, std::make_unique<EatGrassGoal>());
        }
        // ── end husbandry ──
    }

    // 6 and 7 — what a mob does when nothing else is happening, which is most
    // of the time and therefore most of what anyone actually watches.
    selector.add(6, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));  // ── mobs-2 ──
    selector.add(7, std::make_unique<LookAtEntityGoal>(look_type, 8.0, 0.02F));
    selector.add(8, std::make_unique<RandomLookGoal>());
}

// ── Mob ─────────────────────────────────────────────────────────────────────

Mob::Mob(const MobKind& kind, f32 width, f32 height, i64 seed, i32 quarry_type)
    : kind_{&kind}, brain_{2048}, random_{seed} {
    brain_.size                 = MobSize::from_box(width, height);
    brain_.abilities.opens_doors = kind.opens_doors;
    brain_.abilities.avoids_sun  = kind.avoids_sun;
    brain_.abilities.enters_water = true;

    // Looks at anything, hunts only what the caller named. Handing the hunt
    // "anything" as well is what made a skeleton and a spider two blocks apart
    // melee each other forever instead of wandering off.
    install_goals(goals_, kind, -1, quarry_type);

    // Kept so `frighten` can reach it: a damage event is not something a goal
    // can see for itself. Borrowed — the selector owns it and outlives the
    // pointer.
    panic_ = kind.panics ? static_cast<PanicGoal*>(goals_.find("panic")) : nullptr;

    // ── husbandry ──
    adult_width_  = width;
    adult_height_ = height;
    // A chicken's first egg: drawn at birth, as the game does — measured on
    // 200 fresh chickens, 6004 to 11992 ticks.
    if (const AnimalKind* animal = animal_kind(kind.type_name);
        animal != nullptr && animal->lays_eggs) {
        brain_.animal.egg_time = egg_interval(random_);
    }
    // ── villagers ── the state switched on, its generator seeded from the id
    if (villager_mob_kind(kind.type_name) != nullptr) {
        init_villager(brain_.villager, seed);
    }
    // ── tame ── the family, and what is drawn at birth: stats, variant
    if (const TameKind* tame = tame_kind(kind.type_name)) {
        init_tame(brain_.tame, *tame, random_);
    }
}

void Mob::frighten(i32 ticks) noexcept {
    if (panic_ != nullptr) {
        panic_->frighten(ticks);
    }
    // ── villagers ── a hurt villager runs (VillagerPanicGoal), for its own
    // measured span rather than the caller's: a villager's panic is not the
    // goal-driven one (docs/provenance/villageois.md).
    if (brain_.villager.active) {
        (void)ticks;
        brain_.villager.hurt_ticks = std::max(brain_.villager.hurt_ticks, kHurtPanicTicks);
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

    // ── husbandry ── Age, love and eggs tick whether or not the brain does:
    // measured, a NoAI calf grows and a NoAI chicken lays.
    tick_husbandry(*state, self, *mob);

    // ── villagers ── claims, a job lost, the level-up timer: before the goals
    if (brain_.villager.active) {
        tick_villager(brain_.villager, *state, self, world, mob->level, mob->villagers,
                      brain_.animal.baby(), context.tick);
    }

    // ── tame ── the drawn body, anger running down; a horse under a rider's
    // control has no brain of its own: the rider's client moves it.
    if (brain_.tame.active()) {
        apply_tame_body(brain_.tame, *state);
        tick_tame(brain_.tame, *state);
    }

    // The brain runs only when there is a world to read. Without a level a mob
    // still falls — which is the floor `FallingMob` established — but it does
    // not decide anything, because every decision here needs blocks.
    if (mob->level != nullptr && !rider_controls(brain_.tame) && !no_ai_) {  // ── noai ──
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
        // ── husbandry ──
        goal_context.tempters      = mob->tempters;
        goal_context.animal_events = mob->animal_events;
        goal_context.brain_of      = &mob_brain_of;
        // ── villagers ──
        goal_context.villagers = mob->villagers;
        // ── mobs-3 ──
        goal_context.quarries      = mob->quarries;
        goal_context.attacks       = mob->attacks;
        goal_context.villager_type = mob->villager_type;
        goal_context.tame_world    = mob->tame_world;  // ── tame ──
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
                    // ── tame ── a horse walks at its own drawn attribute
                    // ── mobs-4 ── times what Speed or Slowness make of the walk
                    const f64 speed =
                        (brain_.speed > 0.0 ? brain_.speed : kind_->speed(kind_->stroll)) *  // ── mobs-2 ──
                        tame_speed_factor(brain_.tame, kind_->movement_speed) *
                        brain_.effect_walk;
                    // Divided by the friction the step is about to apply, so
                    // that what comes out is `speed` blocks of *displacement*.
                    // Without this the mob moves at 0.546 of the number in the
                    // table — and since the table's number is the measured one,
                    // every mob in the world would be 45 % too slow while the
                    // constant it was compared against still read correct.
                    // ── movement physics ── the floor's own friction, and the
                    // walk law's ratio for it (ice, soul sand).
                    const f64 friction = entity_friction(*state, motion_, *mob->world);
                    const f64 floor    = walk_floor_scale(*state, motion_, *mob->world);
                    state->velocity.x  = dx / length * speed * floor / friction;
                    state->velocity.z  = dz / length * speed * floor / friction;
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

    // ── noai ── no physics either: the position stays and the stored velocity
    // decays by 0.98 a tick, with no gravity added (apprivoisement.md § 8.3).
    if (no_ai_) {
        state->velocity.x *= 0.98;
        state->velocity.y *= 0.98;
        state->velocity.z *= 0.98;
        return;
    }
    *state = step_entity(*state, motion_, *mob->world);
}

}  // namespace ov::gameplay
