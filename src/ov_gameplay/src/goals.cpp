#include "ov/gameplay/goals.hpp"

#include "ov/gameplay/breeding.hpp"  // ── husbandry ──

#include <algorithm>
#include <cmath>

namespace ov::gameplay {

entity::EntityState* GoalContext::state() noexcept {
    return entities == nullptr ? nullptr : entities->mutable_state(self);
}

const entity::EntityState* GoalContext::state() const noexcept {
    return entities == nullptr ? nullptr : entities->state(self);
}

namespace {

[[nodiscard]] BlockPos feet_block(const entity::EntityState& state) noexcept {
    return BlockPos{static_cast<i32>(std::floor(state.position.x)),
                    static_cast<i32>(std::floor(state.position.y)),
                    static_cast<i32>(std::floor(state.position.z))};
}

[[nodiscard]] f64 distance_sq(const Vec3d& a, const Vec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    const f64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

/// A block near `origin`, chosen at random within a box.
///
/// Drawn as three independent integers rather than a random point in a sphere:
/// the box is what the goal wants, and rejection-sampling a sphere would burn a
/// variable number of draws and make the stream depend on the geometry.
[[nodiscard]] BlockPos random_nearby(math::LegacyRandomSource& random, BlockPos origin,
                                     i32 radius, i32 vertical) {
    const i32 dx = random.next_int(radius * 2 + 1) - radius;
    const i32 dy = random.next_int(vertical * 2 + 1) - vertical;
    const i32 dz = random.next_int(radius * 2 + 1) - radius;
    return BlockPos{origin.x + dx, origin.y + dy, origin.z + dz};
}

}  // namespace

bool move_to(GoalContext& context, BlockPos destination, f64 speed, f32 max_range) {
    entity::EntityState* self = context.state();
    if (self == nullptr || context.brain == nullptr || context.level == nullptr) {
        return false;
    }
    MobBrain& brain = *context.brain;
    // At most one search every four ticks, and never twice in one. Vanilla
    // re-paths on a timer for the same reason: A* is not cheap and a mob that
    // recomputed every tick would spend the whole server budget deciding to
    // keep walking in the direction it was already walking.
    if (context.tick < brain.next_path_tick) {
        return !brain.follower.done();
    }
    brain.next_path_tick = context.tick + 4;

    WalkNodeEvaluator walk;
    SwimNodeEvaluator swim;
    FlyNodeEvaluator  fly;
    const NodeEvaluator& evaluator = brain.abilities.enters_water ? static_cast<NodeEvaluator&>(walk)
                                                                  : static_cast<NodeEvaluator&>(walk);
    (void)swim;
    (void)fly;

    brain.finder.find(evaluator, *context.level, feet_block(*self), destination, brain.size,
                      brain.abilities, max_range, brain.path);
    if (brain.path.steps.size() < 2) {
        // One step is where the mob already is: not a route.
        brain.follower.clear();
        return false;
    }
    brain.follower.set(brain.path);
    brain.speed      = speed;
    brain.wants_move = true;
    return true;
}

entity::EntityHandle nearest_entity(const entity::EntityWorld& world, entity::EntityHandle self,
                                    i32 type, f64 radius) {
    const entity::EntityState* mine = world.state(self);
    if (mine == nullptr || type < -1) {
        return entity::kNoEntity;
    }
    const f64            limit = radius * radius;
    entity::EntityHandle best  = entity::kNoEntity;
    f64                  best_distance = limit;
    for (const entity::EntityHandle handle : world.handles()) {
        if (handle == self) {
            continue;
        }
        const entity::EntityState* other = world.state(handle);
        if (other == nullptr || other->removed || other->health <= 0.0F) {
            continue;
        }
        if (type >= 0 && other->type != type) {
            continue;
        }
        const f64 d = distance_sq(mine->position, other->position);
        // Strictly closer, so the earliest-spawned of two equidistant entities
        // wins and the choice does not depend on storage order.
        if (d < best_distance) {
            best_distance = d;
            best          = handle;
        }
    }
    return best;
}

bool has_line_of_sight(const CollisionWorld& collisions, const entity::EntityState& from,
                       const entity::EntityState& to) {
    const Vec3d eye{from.position.x, from.position.y + static_cast<f64>(from.eye_height),
                    from.position.z};
    const Vec3d aim{to.position.x, to.position.y + static_cast<f64>(to.eye_height) * 0.5,
                    to.position.z};
    return has_clear_line(collisions, eye, aim);
}

bool has_clear_line(const CollisionWorld& collisions, const Vec3d& eye, const Vec3d& aim) {
    const Vec3d delta = aim - eye;
    const f64   length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (length < 1e-9) {
        return true;
    }
    // Sampled rather than swept. A DDA would be exact, and this is called for
    // every mob against every candidate target every tick: a step of a quarter
    // block cannot miss a full block and costs four tests per block of range.
    constexpr f64 kStep = 0.25;
    const i32     steps = static_cast<i32>(length / kStep);
    for (i32 i = 1; i < steps; ++i) {
        const f64   t = static_cast<f64>(i) * kStep / length;
        const Vec3d point{eye.x + delta.x * t, eye.y + delta.y * t, eye.z + delta.z * t};
        const AABB  probe{Vec3d{point.x - 1e-4, point.y - 1e-4, point.z - 1e-4},
                         Vec3d{point.x + 1e-4, point.y + 1e-4, point.z + 1e-4}};
        if (collisions.overlaps(probe)) {
            return false;
        }
    }
    return true;
}

// ── GoalSelector ────────────────────────────────────────────────────────────

void GoalSelector::add(i32 priority, std::unique_ptr<Goal> goal) {
    entries_.push_back(Entry{priority, next_order_++, std::move(goal), false});
    // Sorted once per add rather than searched every tick. `stable_sort` is not
    // needed because `order` breaks every tie explicitly — which is what makes
    // the tick deterministic without the caller having to give every goal a
    // different priority.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.order < b.order;
    });
}

void GoalSelector::tick(GoalContext& context) {
    const usize count = entries_.size();

    // 1. Stop what can no longer continue.
    for (Entry& entry : entries_) {
        if (entry.running && !entry.goal->can_continue_to_use(context)) {
            entry.goal->stop(context);
            entry.running = false;
        }
    }

    // Who holds which control. An index into `entries_`, which is sorted by
    // priority, so a smaller index *is* a higher priority.
    i32 holder[kFlagCount];
    for (usize flag = 0; flag < kFlagCount; ++flag) {
        holder[flag] = -1;
    }
    for (usize index = 0; index < count; ++index) {
        if (!entries_[index].running) {
            continue;
        }
        const u8 held = static_cast<u8>(entries_[index].goal->flags());
        for (usize flag = 0; flag < kFlagCount; ++flag) {
            if ((held & (1U << flag)) != 0) {
                holder[flag] = static_cast<i32>(index);
            }
        }
    }

    // 2. Offer a start, in priority order, evicting where allowed.
    for (usize index = 0; index < count; ++index) {
        Entry& entry = entries_[index];
        if (entry.running) {
            continue;
        }
        const u8 wanted = static_cast<u8>(entry.goal->flags());

        bool available = true;
        for (usize flag = 0; flag < kFlagCount && available; ++flag) {
            if ((wanted & (1U << flag)) == 0) {
                continue;
            }
            const i32 held_by = holder[flag];
            if (held_by < 0) {
                continue;  // free
            }
            // Held by something at least as important, or by something that
            // has said it must finish: leave it alone.
            if (static_cast<usize>(held_by) < index ||
                !entries_[static_cast<usize>(held_by)].goal->interruptible()) {
                available = false;
            }
        }
        if (!available) {
            continue;
        }
        // Flags first, then `can_use`. See the note in the header: several
        // goals draw random numbers here, and asking one that could not have
        // started would make the mob's stream depend on what else was running.
        if (!entry.goal->can_use(context)) {
            continue;
        }

        // Evict every holder of a control this goal wants. A goal that loses
        // one control loses all of them — it is stopped, not partially stopped.
        for (usize flag = 0; flag < kFlagCount; ++flag) {
            if ((wanted & (1U << flag)) == 0 || holder[flag] < 0) {
                continue;
            }
            const usize victim = static_cast<usize>(holder[flag]);
            entries_[victim].goal->stop(context);
            entries_[victim].running = false;
            for (usize other = 0; other < kFlagCount; ++other) {
                if (holder[other] == static_cast<i32>(victim)) {
                    holder[other] = -1;
                }
            }
        }

        entry.goal->start(context);
        entry.running = true;
        for (usize flag = 0; flag < kFlagCount; ++flag) {
            if ((wanted & (1U << flag)) != 0) {
                holder[flag] = static_cast<i32>(index);
            }
        }
    }

    // 3. Tick whatever is running, in the same order.
    for (Entry& entry : entries_) {
        if (entry.running) {
            entry.goal->tick(context);
        }
    }
}

void GoalSelector::running(std::vector<std::string_view>& out) const {
    out.clear();
    for (const Entry& entry : entries_) {
        if (entry.running) {
            out.push_back(entry.goal->name());
        }
    }
}

Goal* GoalSelector::find(std::string_view name) noexcept {
    for (Entry& entry : entries_) {
        if (entry.goal->name() == name) {
            return entry.goal.get();
        }
    }
    return nullptr;
}

bool GoalSelector::is_running(std::string_view name) const {
    for (const Entry& entry : entries_) {
        if (entry.running && entry.goal->name() == name) {
            return true;
        }
    }
    return false;
}


/// Re-assert the intent to walk.
///
/// Necessary every tick, not once at `start`: the mob clears `wants_move` at
/// the top of each of its own ticks so that a goal which has quietly stopped
/// cannot leave the body coasting. A move goal that only set the flag when it
/// started produced a mob that walked for exactly one tick per decision —
/// seven ticks of movement in six hundred, which reads as a pathfinder that
/// cannot find anything rather than as a flag that was cleared.
static void keep_walking(GoalContext& context, f64 speed) {
    if (context.brain == nullptr || context.brain->follower.done()) {
        return;
    }
    context.brain->wants_move = true;
    context.brain->speed      = speed;
}

// ── FloatGoal ───────────────────────────────────────────────────────────────

bool FloatGoal::can_use(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.level == nullptr) {
        return false;
    }
    const BlockPos                 feet   = feet_block(*self);
    const registry::BlockRegistry& blocks = context.level->blocks();
    const registry::BlockStateId   state  = context.level->block_at(feet);
    if (!blocks.holds_fluid(state)) {
        return false;
    }
    return blocks.block_name(blocks.block_of(state)) != "minecraft:lava";
}

void FloatGoal::tick(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->wants_jump = true;
    }
}

// ── RandomStrollGoal ────────────────────────────────────────────────────────

bool RandomStrollGoal::can_use(GoalContext& context) {
    entity::EntityState* self = context.state();
    if (self == nullptr || context.random == nullptr || context.brain == nullptr) {
        return false;
    }
    // A draw per tick rather than a countdown: the countdown version makes a
    // whole field of cows step off together, because they were all created on
    // the same tick and their timers never diverge.
    if (context.random->next_int(interval_) != 0) {
        return false;
    }
    const BlockPos here = feet_block(*self);
    for (i32 attempt = 0; attempt < 10; ++attempt) {
        const BlockPos candidate = random_nearby(*context.random, here, radius_, 3);
        if (candidate == here) {
            continue;
        }
        wanted_ = candidate;
        return true;
    }
    return false;
}

bool RandomStrollGoal::can_continue_to_use(GoalContext& context) {
    return context.brain != nullptr && !context.brain->follower.done();
}

void RandomStrollGoal::start(GoalContext& context) {
    (void)move_to(context, wanted_, speed_, 32.0F);
}

void RandomStrollGoal::tick(GoalContext& context) { keep_walking(context, speed_); }

void RandomStrollGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

// ── RandomLookGoal ──────────────────────────────────────────────────────────

bool RandomLookGoal::can_use(GoalContext& context) {
    return context.random != nullptr && context.random->next_float() < 0.02F;
}

bool RandomLookGoal::can_continue_to_use(GoalContext&) { return remaining_ > 0; }

void RandomLookGoal::start(GoalContext& context) {
    // The angle is drawn once and the direction derived from it, rather than
    // drawing dx and dz independently — two independent draws concentrate the
    // directions on the diagonals of the square, and a mob that looks mostly
    // north-east is a bug nobody ever reports.
    const f64 angle = 6.283185307179586 * context.random->next_double();
    dx_             = std::cos(angle);
    dz_             = std::sin(angle);
    remaining_      = 20 + context.random->next_int(20);
}

void RandomLookGoal::tick(GoalContext& context) {
    --remaining_;
    entity::EntityState* self = context.state();
    if (self == nullptr || context.brain == nullptr) {
        return;
    }
    context.brain->look_at  = Vec3d{self->position.x + dx_,
                                   self->position.y + static_cast<f64>(self->eye_height),
                                   self->position.z + dz_};
    context.brain->has_look = true;
}

// ── LookAtEntityGoal ────────────────────────────────────────────────────────

bool LookAtEntityGoal::can_use(GoalContext& context) {
    if (context.entities == nullptr || context.random == nullptr) {
        return false;
    }
    if (context.random->next_float() >= probability_) {
        return false;
    }
    watched_ = nearest_entity(*context.entities, context.self, type_, radius_);
    return watched_ != entity::kNoEntity;
}

bool LookAtEntityGoal::can_continue_to_use(GoalContext& context) {
    if (remaining_ <= 0 || context.entities == nullptr) {
        return false;
    }
    const entity::EntityState* other = context.entities->state(watched_);
    const entity::EntityState* self  = context.state();
    if (other == nullptr || self == nullptr) {
        return false;
    }
    return distance_sq(self->position, other->position) <= radius_ * radius_;
}

void LookAtEntityGoal::start(GoalContext& context) {
    remaining_ = 40 + context.random->next_int(40);
}

void LookAtEntityGoal::tick(GoalContext& context) {
    --remaining_;
    const entity::EntityState* other = context.entities->state(watched_);
    if (other == nullptr || context.brain == nullptr) {
        return;
    }
    context.brain->look_at =
        Vec3d{other->position.x, other->position.y + static_cast<f64>(other->eye_height),
              other->position.z};
    context.brain->has_look = true;
}

void LookAtEntityGoal::stop(GoalContext& context) {
    watched_ = entity::kNoEntity;
    if (context.brain != nullptr) {
        context.brain->has_look = false;
    }
}

// ── NearestAttackableTargetGoal ─────────────────────────────────────────────

bool NearestAttackableTargetGoal::can_use(GoalContext& context) {
    if (context.entities == nullptr) {
        return false;
    }
    // ── mobs-3 ── The villager sentinel is the id the caller supplies, and the
    // players are searched beside the entity world: a quarry of the goal's
    // type, in sight when sight is asked for, nearer than any entity found.
    found_player_  = 0;
    const i32 type = type_ == kVillagerQuarry ? context.villager_type : type_;
    if (type_ == kVillagerQuarry && type < 0) {
        return false;
    }
    const entity::EntityHandle found =
        nearest_entity(*context.entities, context.self, type, radius_);
    if (const entity::EntityState* self = context.state(); self != nullptr && type >= 0) {
        f64 best = radius_ * radius_;
        if (const entity::EntityState* other = context.entities->state(found)) {
            best = distance_sq(self->position, other->position);
        }
        for (const Quarry& quarry : context.quarries) {
            if (quarry.type != type) {
                continue;
            }
            const f64 d = distance_sq(self->position, quarry.feet);
            if (d >= best) {
                continue;
            }
            if (must_see_) {
                if (context.collisions == nullptr) {
                    continue;
                }
                entity::EntityState stand_in;
                stand_in.position   = quarry.feet;
                stand_in.width      = quarry.width;
                stand_in.height     = quarry.height;
                stand_in.eye_height = quarry.eye_height;
                if (!has_line_of_sight(*context.collisions, *self, stand_in)) {
                    continue;
                }
            }
            best          = d;
            found_player_ = quarry.network_id;
        }
        if (found_player_ != 0) {
            found_ = entity::kNoEntity;
            return true;
        }
    }
    // ── end mobs-3 ──
    if (found == entity::kNoEntity) {
        return false;
    }
    if (must_see_) {
        // The gate the maze oracle ran into: a target behind a wall is never
        // acquired, however close it is. See scripts/measure_mobs.py.
        const entity::EntityState* self  = context.state();
        const entity::EntityState* other = context.entities->state(found);
        if (self == nullptr || other == nullptr || context.collisions == nullptr) {
            return false;
        }
        if (!has_line_of_sight(*context.collisions, *self, *other)) {
            return false;
        }
    }
    found_ = found;
    return true;
}

bool NearestAttackableTargetGoal::can_continue_to_use(GoalContext& context) {
    if (context.entities == nullptr || context.brain == nullptr) {
        return false;
    }
    // ── mobs-3 ── A player quarry: still listed (alive, in survival), and in range.
    if (context.brain->target_player != 0) {
        const Quarry*              quarry = find_quarry(context.quarries, context.brain->target_player);
        const entity::EntityState* me     = context.state();
        return quarry != nullptr && me != nullptr &&
               distance_sq(me->position, quarry->feet) <= radius_ * radius_;
    }
    const entity::EntityState* other = context.entities->state(context.brain->target);
    const entity::EntityState* self  = context.state();
    if (other == nullptr || self == nullptr || other->removed || other->health <= 0.0F) {
        return false;
    }
    // Distance only. Line of sight is *not* rechecked here, and that is the
    // difference between our mobs and vanilla's: measured, a vanilla zombie
    // whose target was set by being hurt drops it the first tick it cannot see
    // its attacker. Keeping a target once acquired is a deliberate divergence,
    // recorded in docs/provenance/mobs.md, because the alternative is a mob
    // that gives up the moment it turns a corner.
    return distance_sq(self->position, other->position) <= radius_ * radius_;
}

void NearestAttackableTargetGoal::start(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target        = found_;
        context.brain->target_player = found_player_;  // ── mobs-3 ──
    }
}

void NearestAttackableTargetGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target             = entity::kNoEntity;
        context.brain->target_player      = 0;  // ── mobs-3 ──
        context.brain->target_forgotten_at = context.tick;
    }
    found_        = entity::kNoEntity;
    found_player_ = 0;
}

// ── mobs-3 ──
const Quarry* find_quarry(std::span<const Quarry> quarries, i32 network_id) noexcept {
    for (const Quarry& quarry : quarries) {
        if (quarry.network_id == network_id) {
            return &quarry;
        }
    }
    return nullptr;
}

// ── MeleeAttackGoal ─────────────────────────────────────────────────────────

bool MeleeAttackGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || context.entities == nullptr) {
        return false;
    }
    if (context.brain->target_player != 0) {  // ── mobs-3 ──
        return find_quarry(context.quarries, context.brain->target_player) != nullptr;
    }
    const entity::EntityState* target = context.entities->state(context.brain->target);
    return target != nullptr && !target->removed && target->health > 0.0F;
}

bool MeleeAttackGoal::can_continue_to_use(GoalContext& context) { return can_use(context); }

void MeleeAttackGoal::start(GoalContext& context) {
    ticks_until_attack_ = 0;
    next_repath_        = context.tick;
}

void MeleeAttackGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
        context.brain->has_look   = false;
    }
}

void MeleeAttackGoal::tick(GoalContext& context) {
    entity::EntityState* self = context.state();
    if (self == nullptr) {
        return;
    }
    // ── mobs-3 ── The target is a player quarry or an entity; the goal needs
    // only where it stands, how wide it is, where its eyes are and its id.
    Vec3d where{};
    f32   width     = 0.0F;
    f32   eye       = 0.0F;
    i32   target_id = 0;
    bool  is_player = context.brain->target_player != 0;
    if (is_player) {
        const Quarry* quarry = find_quarry(context.quarries, context.brain->target_player);
        if (quarry == nullptr) {
            return;
        }
        where     = quarry->feet;
        width     = quarry->width;
        eye       = quarry->eye_height;
        target_id = quarry->network_id;
    } else {
        const entity::EntityState* target = context.entities->state(context.brain->target);
        if (target == nullptr) {
            return;
        }
        where     = target->position;
        width     = target->width;
        eye       = target->eye_height;
        target_id = target->network_id;
    }
    MobBrain& brain = *context.brain;
    brain.look_at   = Vec3d{where.x, where.y + static_cast<f64>(eye), where.z};
    brain.has_look  = true;

    if (ticks_until_attack_ > 0) {
        --ticks_until_attack_;
    }

    // ── mobs-3 ── The game's reach, `(2·w)² + w_target` squared, feet to feet;
    // a mob that does not swing holds off at `hold_at` instead (mobs-2: a
    // skeleton at 15, a witch at 10; mobs-3: a creeper at its swell's 3).
    const f64 reach_sq = hold_at_ > 0.0 ? hold_at_ * hold_at_ : melee_reach_sq(self->width, width);
    const f64 gap      = distance_sq(self->position, where);
    if (gap <= reach_sq) {
        brain.wants_move = false;
        brain.follower.clear();
        if (ticks_until_attack_ == 0) {
            ticks_until_attack_ = cooldown_;
            if (strikes_ && context.attacks != nullptr) {  // ── mobs-3 ──
                context.attacks->push_back(MobAttack{context.self, target_id, is_player});
            }
        }
        return;
    }
    if (context.tick >= next_repath_) {
        next_repath_ = context.tick + 10;
        const BlockPos goal{static_cast<i32>(std::floor(where.x)),
                            static_cast<i32>(std::floor(where.y)),
                            static_cast<i32>(std::floor(where.z))};
        (void)move_to(context, goal, speed_, 64.0F);
    }
    // ── mobs-3 ── Keep walking between two re-paths. The mob clears its intent
    // at the top of every tick, and this goal used to set it only on the tick
    // it re-pathed: a chasing zombie was pushed one tick in ten and friction
    // stopped it after 0.4 block — found by the first test that let a zombie
    // reach a player (mobs-3.md § 1.5).
    keep_walking(context, speed_);
}

// ── PanicGoal ───────────────────────────────────────────────────────────────

bool PanicGoal::can_use(GoalContext& context) {
    if (frightened_ <= 0 || context.state() == nullptr || context.random == nullptr) {
        return false;
    }
    const BlockPos here = feet_block(*context.state());
    for (i32 attempt = 0; attempt < 10; ++attempt) {
        const BlockPos candidate = random_nearby(*context.random, here, radius_, 4);
        if (candidate != here) {
            away_ = candidate;
            return true;
        }
    }
    return false;
}

bool PanicGoal::can_continue_to_use(GoalContext& context) {
    if (frightened_ > 0) {
        --frightened_;
    }
    return frightened_ > 0 && context.brain != nullptr && !context.brain->follower.done();
}

void PanicGoal::start(GoalContext& context) { (void)move_to(context, away_, speed_, 32.0F); }

void PanicGoal::tick(GoalContext& context) { keep_walking(context, speed_); }

void PanicGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

// ── AvoidSunGoal ────────────────────────────────────────────────────────────

bool AvoidSunGoal::can_use(GoalContext& context) {
    if (!daylight_ || context.state() == nullptr || context.level == nullptr ||
        context.random == nullptr) {
        return false;
    }
    const BlockPos here = feet_block(*context.state());
    // Somewhere with a roof: a column whose highest solid block is above the
    // mob's head. Asked of the level rather than of a light array, because a
    // LevelView answers blocks and this module may not reach past it.
    for (i32 attempt = 0; attempt < 12; ++attempt) {
        const BlockPos candidate = random_nearby(*context.random, here, 8, 2);
        bool           covered   = false;
        const registry::BlockRegistry& blocks = context.level->blocks();
        for (i32 up = 2; up < 12 && !covered; ++up) {
            const registry::BlockStateId state =
                context.level->block_at(candidate.above(up));
            covered = !blocks.is_air(blocks.block_of(state));
        }
        if (covered) {
            shade_ = candidate;
            return true;
        }
    }
    return false;
}

bool AvoidSunGoal::can_continue_to_use(GoalContext& context) {
    return daylight_ && context.brain != nullptr && !context.brain->follower.done();
}

void AvoidSunGoal::start(GoalContext& context) { (void)move_to(context, shade_, speed_, 32.0F); }

void AvoidSunGoal::tick(GoalContext& context) { keep_walking(context, speed_); }

void AvoidSunGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

// ── FollowParentGoal ────────────────────────────────────────────────────────

bool FollowParentGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || !context.brain->animal.baby() || context.entities == nullptr) {
        return false;
    }
    const entity::EntityState* self = context.state();
    if (self == nullptr) {
        return false;
    }
    if (context.brain->parent == entity::kNoEntity) {
        // The nearest adult of the same type. Chosen once and remembered, so a
        // calf does not swap mothers every time two cows cross.
        const entity::EntityHandle found =
            nearest_entity(*context.entities, context.self, self->type, radius_);
        if (found == entity::kNoEntity) {
            return false;
        }
        context.brain->parent = found;
    }
    const entity::EntityState* parent = context.entities->state(context.brain->parent);
    if (parent == nullptr) {
        context.brain->parent = entity::kNoEntity;
        return false;
    }
    const f64 gap = distance_sq(self->position, parent->position);
    return gap > 9.0 && gap < radius_ * radius_;
}

bool FollowParentGoal::can_continue_to_use(GoalContext& context) { return can_use(context); }

void FollowParentGoal::tick(GoalContext& context) {
    const entity::EntityState* parent = context.entities->state(context.brain->parent);
    if (parent == nullptr) {
        return;
    }
    (void)move_to(context, feet_block(*parent), speed_, 32.0F);
}

void FollowParentGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

// ── BreedGoal ───────────────────────────────────────────────────────────────

// ── husbandry ──
namespace {

/// A mate for `self`: same type, adult, in love, not `self`, and whose box
/// meets `self`'s grown by `reach` on every axis. The nearest wins; ties go to
/// the earliest spawned, so the choice does not depend on storage order.
[[nodiscard]] entity::EntityHandle find_mate(GoalContext& context, f64 reach) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.entities == nullptr || context.brain_of == nullptr) {
        return entity::kNoEntity;
    }
    entity::EntityHandle best          = entity::kNoEntity;
    f64                  best_distance = 0.0;
    for (const entity::EntityHandle handle : context.entities->handles()) {
        if (handle == context.self) {
            continue;
        }
        const entity::EntityState* other = context.entities->state(handle);
        if (other == nullptr || other->removed || other->health <= 0.0F ||
            other->type != self->type) {
            continue;
        }
        const f64 gap_x = std::abs(other->position.x - self->position.x);
        const f64 gap_y = other->position.y + static_cast<f64>(other->height) * 0.5 -
                          (self->position.y + static_cast<f64>(self->height) * 0.5);
        const f64 gap_z = std::abs(other->position.z - self->position.z);
        const f64 reach_xz =
            reach + (static_cast<f64>(self->width) + static_cast<f64>(other->width)) * 0.5;
        const f64 reach_y =
            reach + (static_cast<f64>(self->height) + static_cast<f64>(other->height)) * 0.5;
        if (gap_x >= reach_xz || gap_z >= reach_xz || std::abs(gap_y) >= reach_y) {
            continue;
        }
        const MobBrain* mate = context.brain_of(*context.entities, handle);
        if (mate == nullptr || !mate->animal.in_love() || mate->animal.age != 0) {
            continue;
        }
        const f64 d = distance_sq(self->position, other->position);
        if (best == entity::kNoEntity || d < best_distance) {
            best          = handle;
            best_distance = d;
        }
    }
    return best;
}

}  // namespace

bool BreedGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || !context.brain->animal.in_love() ||
        context.brain->animal.age != 0) {
        return false;
    }
    mate_ = find_mate(context, reach_);
    return mate_ != entity::kNoEntity;
}

bool BreedGoal::can_continue_to_use(GoalContext& context) {
    if (context.brain == nullptr || !context.brain->animal.in_love() ||
        context.entities == nullptr || context.brain_of == nullptr) {
        return false;
    }
    const entity::EntityState* mate = context.entities->state(mate_);
    if (mate == nullptr || mate->removed || mate->health <= 0.0F) {
        return false;
    }
    const MobBrain* brain = context.brain_of(*context.entities, mate_);
    return brain != nullptr && brain->animal.in_love() && loops_ < kMateTicks;
}

void BreedGoal::start(GoalContext&) { loops_ = 0; }

void BreedGoal::stop(GoalContext& context) {
    mate_  = entity::kNoEntity;
    loops_ = 0;
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

void BreedGoal::tick(GoalContext& context) {
    entity::EntityState*       self = context.state();
    const entity::EntityState* mate = context.entities->state(mate_);
    if (self == nullptr || mate == nullptr || context.brain == nullptr) {
        return;
    }
    context.brain->look_at  = Vec3d{mate->position.x,
                                   mate->position.y + static_cast<f64>(mate->eye_height),
                                   mate->position.z};
    context.brain->has_look = true;

    // Walk to the mate, and count: sixty ticks of the goal running and the two
    // within three blocks is a birth. Measured: fed side by side, the calf
    // came 59 to 60 ticks later, five trials out of five. The three-block
    // condition is not measured separately — an adjacent pair cannot test it.
    (void)move_to(context, feet_block(*mate), speed_, 32.0F);
    keep_walking(context, speed_);
    ++loops_;
    if (loops_ < kMateTicks || distance_sq(self->position, mate->position) >= 9.0) {
        return;
    }
    MobBrain* other = context.brain_of == nullptr
                          ? nullptr
                          : context.brain_of(*context.entities, mate_);
    if (other == nullptr) {
        return;
    }
    // Both parents: love spent, six thousand ticks before either can again.
    context.brain->animal.love = 0;
    context.brain->animal.age  = kParentCooldown;
    other->animal.love         = 0;
    other->animal.age          = kParentCooldown;
    if (context.animal_events != nullptr) {
        AnimalEvent birth;
        birth.kind  = AnimalEventKind::Birth;
        birth.self  = context.self;
        birth.other = mate_;
        // Where this parent stands. Which parent, and whether vanilla offsets
        // it, is not measured.
        birth.at = self->position;
        context.animal_events->push_back(birth);
    }
    loops_ = 0;
}
// ── end husbandry ──

}  // namespace ov::gameplay
