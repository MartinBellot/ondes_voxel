#include "ov/gameplay/goals.hpp"

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
    if (mine == nullptr) {
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
    // 1. Stop what can no longer continue, and collect what the survivors hold.
    u8 taken = 0;
    for (Entry& entry : entries_) {
        if (!entry.running) {
            continue;
        }
        if (!entry.goal->can_continue_to_use(context)) {
            entry.goal->stop(context);
            entry.running = false;
            continue;
        }
        taken |= static_cast<u8>(entry.goal->flags());
    }

    // 2. Offer a start, in priority order.
    for (Entry& entry : entries_) {
        if (entry.running) {
            continue;
        }
        const u8 wanted = static_cast<u8>(entry.goal->flags());
        if ((taken & wanted) != 0) {
            continue;
        }
        if (!entry.goal->can_use(context)) {
            continue;
        }
        entry.goal->start(context);
        entry.running = true;
        taken |= wanted;
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

bool GoalSelector::is_running(std::string_view name) const {
    for (const Entry& entry : entries_) {
        if (entry.running && entry.goal->name() == name) {
            return true;
        }
    }
    return false;
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
    const entity::EntityHandle found =
        nearest_entity(*context.entities, context.self, type_, radius_);
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
        context.brain->target = found_;
    }
}

void NearestAttackableTargetGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target             = entity::kNoEntity;
        context.brain->target_forgotten_at = context.tick;
    }
    found_ = entity::kNoEntity;
}

// ── MeleeAttackGoal ─────────────────────────────────────────────────────────

bool MeleeAttackGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || context.entities == nullptr) {
        return false;
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
    entity::EntityState*       self   = context.state();
    const entity::EntityState* target = context.entities->state(context.brain->target);
    if (self == nullptr || target == nullptr) {
        return;
    }
    MobBrain& brain = *context.brain;
    brain.look_at   = Vec3d{target->position.x,
                          target->position.y + static_cast<f64>(target->eye_height),
                          target->position.z};
    brain.has_look  = true;

    if (ticks_until_attack_ > 0) {
        --ticks_until_attack_;
    }

    const f64 reach = static_cast<f64>(self->width) + static_cast<f64>(target->width) + 1.0;
    const f64 gap   = distance_sq(self->position, target->position);
    if (gap <= reach * reach) {
        brain.wants_move = false;
        brain.follower.clear();
        if (ticks_until_attack_ == 0) {
            ticks_until_attack_ = cooldown_;
        }
        return;
    }
    if (context.tick >= next_repath_) {
        next_repath_ = context.tick + 10;
        (void)move_to(context, feet_block(*target), speed_, 64.0F);
    }
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

void AvoidSunGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

// ── FollowParentGoal ────────────────────────────────────────────────────────

bool FollowParentGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || !context.brain->baby || context.entities == nullptr) {
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

bool BreedGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || context.brain->love_ticks <= 0 || context.brain->baby) {
        return false;
    }
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.entities == nullptr) {
        return false;
    }
    // A mate is an animal of the same type, in love, and not this one. The
    // "in love" half cannot be seen from an EntityState, so the caller keeps
    // the brains and the goal is told about the mate through `set_mate`-shaped
    // discovery: the nearest same-type animal within the radius, which the
    // caller only puts in love when it has been fed.
    const entity::EntityHandle found =
        nearest_entity(*context.entities, context.self, self->type, radius_);
    if (found == entity::kNoEntity) {
        return false;
    }
    mate_ = found;
    return true;
}

bool BreedGoal::can_continue_to_use(GoalContext& context) {
    if (context.brain == nullptr || context.brain->love_ticks <= 0) {
        return false;
    }
    return context.entities != nullptr && context.entities->state(mate_) != nullptr;
}

void BreedGoal::start(GoalContext&) {
    loops_ = 0;
    bred_  = false;
}

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

    if (distance_sq(self->position, mate->position) > 9.0) {
        (void)move_to(context, feet_block(*mate), speed_, 32.0F);
        return;
    }
    // Close enough. Sixty ticks together and there is a calf, put down between
    // the two parents rather than inside either — a birth on top of the mother
    // makes the newborn's first act a collision resolution.
    ++loops_;
    if (loops_ < 60) {
        return;
    }
    birth_ = Vec3d{(self->position.x + mate->position.x) * 0.5,
                   (self->position.y + mate->position.y) * 0.5,
                   (self->position.z + mate->position.z) * 0.5};
    bred_                     = true;
    context.brain->love_ticks = 0;
    context.brain->breed_cooldown = 6000;
    loops_                    = 0;
}

}  // namespace ov::gameplay
