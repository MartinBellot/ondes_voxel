#include "ov/gameplay/brain/villager_brain.hpp"

#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/sleep.hpp"
#include "ov/gameplay/trading.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/gameplay/walk_speed.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::gameplay::brain {
namespace {

using Face = registry::BlockRegistry::Face;

// ── Food and pockets ────────────────────────────────────────────────────────

struct Food {
    std::string_view item;
    i32              points;
};
// The wiki's food points. Measured: 3 bread (12) and 12 carrots breed, 2 bread
// (8) does not; 6 bread leave 3, 12 potatoes + 12 beetroots leave the beetroots.
constexpr std::array<Food, 4> kFoods{{
    {"minecraft:bread", 4},
    {"minecraft:potato", 1},
    {"minecraft:carrot", 1},
    {"minecraft:beetroot", 1},
}};

constexpr std::array<std::string_view, 9> kWanted{
    "minecraft:bread",          "minecraft:potato",       "minecraft:carrot",
    "minecraft:beetroot",       "minecraft:wheat",        "minecraft:wheat_seeds",
    "minecraft:beetroot_seeds", "minecraft:torchflower_seeds", "minecraft:pitcher_pod",
};

// What a farmer plants, first found first planted.
constexpr std::array<std::string_view, 6> kSeeds{
    "minecraft:wheat_seeds", "minecraft:beetroot_seeds", "minecraft:carrot",
    "minecraft:potato",      "minecraft:torchflower_seeds", "minecraft:pitcher_pod",
};

// ── Helpers ─────────────────────────────────────────────────────────────────

[[nodiscard]] VillagerState* villager_of(BrainContext& c) noexcept {
    MobBrain* b = c.goal.brain;
    return b != nullptr && b->villager.active ? &b->villager : nullptr;
}

[[nodiscard]] bool baby(const BrainContext& c) noexcept {
    return c.goal.brain != nullptr && c.goal.brain->animal.baby();
}

[[nodiscard]] BlockPos feet_of(const Vec3d& p) noexcept {
    return BlockPos{static_cast<i32>(std::floor(p.x)), static_cast<i32>(std::floor(p.y)),
                    static_cast<i32>(std::floor(p.z))};
}

[[nodiscard]] Vec3d centre(BlockPos b) noexcept {
    return Vec3d{static_cast<f64>(b.x) + 0.5, static_cast<f64>(b.y) + 0.5,
                 static_cast<f64>(b.z) + 0.5};
}

[[nodiscard]] f64 dist_sq(const Vec3d& a, const Vec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    const f64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] i32 manhattan(BlockPos a, BlockPos b) noexcept {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
}

/// Blocks a tick at a behaviour's speed modifier: the walk law over the
/// villager's measured attribute (0.5).
[[nodiscard]] f32 walk(f64 modifier) noexcept {
    return static_cast<f32>(walk_blocks_per_tick(kVillagerSpeedAttribute * modifier));
}

[[nodiscard]] MobBrain* villager_brain(entity::EntityWorld& world, entity::EntityHandle h) {
    MobBrain* b = mob_brain_of(world, h);
    return b != nullptr && b->villager.active && !b->villager.wandering ? b : nullptr;
}

/// The entity a memory names, alive, or null.
[[nodiscard]] const entity::EntityState* remembered(BrainContext& c, MemoryType type,
                                                    entity::EntityHandle* handle = nullptr) {
    const MemoryValue* v = c.memories().get(type);
    if (v == nullptr || v->kind != MemoryKind::Entity || c.goal.entities == nullptr) {
        return nullptr;
    }
    const entity::EntityHandle h = c.goal.entities->find(static_cast<i32>(v->number));
    const entity::EntityState* s = c.goal.entities->state(h);
    if (s == nullptr || s->removed || s->health <= 0.0F) {
        return nullptr;
    }
    if (handle != nullptr) {
        *handle = h;
    }
    return s;
}

void walk_to(BrainContext& c, BlockPos to, f32 speed, i32 close_enough) {
    c.memories().set(MemoryType::WalkTarget, MemoryValue::walk_to(to, speed, close_enough));
}

void halt(BrainContext& c) {
    if (c.goal.brain != nullptr) {
        c.goal.brain->follower.clear();
        c.goal.brain->wants_move = false;
    }
}

void push_event(BrainContext& c, const VillagerEvent& e) {
    if (c.goal.villagers != nullptr && c.goal.villagers->events != nullptr) {
        c.goal.villagers->events->push_back(e);
    }
}

/// Where a villager stands to reach a block: the first horizontal neighbour
/// with room for its feet, else the block itself.
[[nodiscard]] BlockPos approach(const world::LevelView* level, BlockPos block) {
    if (level == nullptr) {
        return block;
    }
    constexpr std::array<std::array<i32, 2>, 4> kSides{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    for (const auto& side : kSides) {
        const BlockPos at{block.x + side[0], block.y, block.z + side[1]};
        if (level->block_at(at) == registry::kAirState &&
            level->block_at(BlockPos{at.x, at.y + 1, at.z}) == registry::kAirState) {
            return at;
        }
    }
    return block;
}

void start_sleeping(BrainContext& c, VillagerState& v) {
    v.sleeping = true;
    ++v.revision;
    c.memories().set(MemoryType::LastSlept, MemoryValue::of_number(c.game_time));
    c.memories().erase(MemoryType::WalkTarget);
    c.memories().erase(MemoryType::CantReachWalkTargetSince);
    halt(c);
}

void stop_sleeping(BrainContext& c, VillagerState& v) {
    if (!v.sleeping) {
        return;
    }
    v.sleeping = false;
    ++v.revision;
    c.memories().set(MemoryType::LastWoken, MemoryValue::of_number(c.game_time));
}

[[nodiscard]] bool frightened(const Memories& m) noexcept {
    return m.has(MemoryType::HurtBy) || m.has(MemoryType::NearestHostile);
}

/// The nearest live adult villager within `reach`, not this one and not
/// asleep, for which `ok` holds; or no entity.
template <typename Pred>
[[nodiscard]] entity::EntityHandle nearest_villager(BrainContext& c, f64 reach, Pred ok) {
    const entity::EntityState* self = c.goal.state();
    if (self == nullptr || c.goal.entities == nullptr) {
        return entity::kNoEntity;
    }
    entity::EntityHandle best = entity::kNoEntity;
    f64                  best_d = reach * reach;
    for (const entity::EntityHandle h : c.goal.entities->handles()) {
        if (h == c.goal.self) {
            continue;
        }
        const entity::EntityState* s = c.goal.entities->state(h);
        MobBrain*                  b = villager_brain(*c.goal.entities, h);
        if (s == nullptr || b == nullptr || s->removed || s->health <= 0.0F ||
            b->animal.baby() || b->villager.sleeping) {
            continue;
        }
        const f64 d = dist_sq(s->position, self->position);
        if (d <= best_d && ok(*b)) {
            best   = h;
            best_d = d;
        }
    }
    return best;
}

[[nodiscard]] bool ripe(const registry::BlockRegistry& blocks, registry::BlockStateId state) {
    const registry::BlockId block = blocks.block_of(state);
    const std::string_view  name  = blocks.block_name(block);
    const bool four  = name == "minecraft:beetroots";
    const bool eight = name == "minecraft:wheat" || name == "minecraft:carrots" ||
                       name == "minecraft:potatoes";
    if (!four && !eight) {
        return false;
    }
    const auto age = blocks.find_property(block, "age");
    return age && blocks.property_value(state, *age) == (four ? "3" : "7");
}

[[nodiscard]] bool farmland(const registry::BlockRegistry& blocks, registry::BlockStateId state) {
    return blocks.block_name(blocks.block_of(state)) == "minecraft:farmland";
}

// ── Sensors ─────────────────────────────────────────────────────────────────

enum : u8 { kSenseHostile, kSenseHurt, kSenseGolem };

void sense(BrainContext& c, u8 sensor) {
    VillagerState*             v    = villager_of(c);
    const entity::EntityState* self = c.goal.state();
    if (v == nullptr || self == nullptr || c.goal.entities == nullptr) {
        return;
    }
    Memories& m = c.memories();
    switch (sensor) {
        case kSenseHostile: {
            // The nearest hostile within its own sight distance (the flee
            // campaign's table: 8 for a zombie).
            const VillagerWorld* w = c.goal.villagers;
            i32                  best = -1;
            f64                  best_d = 0.0;
            if (w != nullptr) {
                for (const entity::EntityHandle h : c.goal.entities->handles()) {
                    const entity::EntityState* o = c.goal.entities->state(h);
                    if (o == nullptr || o->removed || o->health <= 0.0F || h == c.goal.self) {
                        continue;
                    }
                    for (const HostileSight& sight : w->hostiles) {
                        if (sight.type != o->type) {
                            continue;
                        }
                        const f64 d = dist_sq(o->position, self->position);
                        const f64 r = static_cast<f64>(sight.distance);
                        if (d <= r * r && (best < 0 || d < best_d)) {
                            best   = o->network_id;
                            best_d = d;
                        }
                    }
                }
            }
            if (best >= 0) {
                m.set(MemoryType::NearestHostile, MemoryValue::of_entity(best));
            } else {
                m.erase(MemoryType::NearestHostile);
            }
            break;
        }
        case kSenseHurt:
            if (v->hurt_ticks > 0) {
                m.set(MemoryType::HurtBy, MemoryValue::unit());
            } else {
                m.erase(MemoryType::HurtBy);
            }
            break;
        case kSenseGolem: {
            const VillagerWorld* w = c.goal.villagers;
            if (w == nullptr || w->iron_golem_type < 0) {
                break;
            }
            for (const entity::EntityHandle h : c.goal.entities->handles()) {
                const entity::EntityState* o = c.goal.entities->state(h);
                if (o != nullptr && !o->removed && o->type == w->iron_golem_type &&
                    o->health > 0.0F &&
                    dist_sq(o->position, self->position) <= kGolemDetectReach * kGolemDetectReach) {
                    m.set(MemoryType::GolemDetectedRecently, MemoryValue::unit(),
                          kGolemDetectedTicks);
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
}

// ── Requirements ────────────────────────────────────────────────────────────

using Req = MemoryRequirement;
using MT  = MemoryType;
using MS  = MemoryStatus;
constexpr std::array<Req, 1> kNeedWalk{{{MT::WalkTarget, MS::Present}}};
constexpr std::array<Req, 1> kNoWalk{{{MT::WalkTarget, MS::Absent}}};
constexpr std::array<Req, 1> kNeedLook{{{MT::LookTarget, MS::Present}}};
constexpr std::array<Req, 2> kAcquireJob{{{MT::PotentialJobSite, MS::Present},
                                          {MT::JobSite, MS::Absent}}};
constexpr std::array<Req, 1> kNeedJob{{{MT::JobSite, MS::Present}}};
constexpr std::array<Req, 1> kNeedHome{{{MT::Home, MS::Present}}};
constexpr std::array<Req, 2> kNoInteraction{{{MT::InteractionTarget, MS::Absent},
                                             {MT::WalkTarget, MS::Absent}}};
constexpr std::array<Req, 1> kNeedInteraction{{{MT::InteractionTarget, MS::Present}}};
constexpr std::array<Req, 1> kNoBreed{{{MT::BreedTarget, MS::Absent}}};
constexpr std::array<Req, 1> kNeedBreed{{{MT::BreedTarget, MS::Present}}};

// ── Core ────────────────────────────────────────────────────────────────────

/// The claims (villager.cpp's scan) as memories, every tick.
class RememberPois final : public Behavior {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "remember_pois"; }

protected:
    void start(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        if (v == nullptr) {
            return;
        }
        Memories&  m    = c.memories();
        const auto keep = [&](MT type, const std::optional<BlockPos>& pos) {
            if (!pos) {
                m.erase(type);
            } else if (m.pos(type) != pos) {
                m.set(type, MemoryValue::of_pos(*pos));
            }
        };
        keep(MT::Home, v->claims.home);
        keep(MT::JobSite, v->claims.job_site);
        keep(MT::PotentialJobSite, v->claims.potential_job_site);
        keep(MT::MeetingPoint, v->claims.meeting_point);
    }
};

class PanicTrigger final : public Behavior {
public:
    PanicTrigger() : Behavior{{}, kForeverDuration, kForeverDuration} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "panic_trigger"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override { return frightened(c.memories()); }
    bool can_still_use(BrainContext& c) override { return frightened(c.memories()); }
    void start(BrainContext& c) override {
        if (!c.brain.is_active(Activity::Panic)) {
            c.memories().erase(MT::WalkTarget);
            c.memories().erase(MT::LookTarget);
            c.memories().erase(MT::BreedTarget);
            c.memories().erase(MT::InteractionTarget);
        }
        c.brain.set_active_if_possible(Activity::Panic, c);
    }
    void tick(BrainContext& c) override {
        // One chance in a hundred a tick: three frightened villagers who
        // slept recently call a golem (the wiki; the golem campaign).
        if (c.goal.random != nullptr && c.goal.random->next_int(100) == 0) {
            (void)spawn_golem_if_needed(c, kGolemPanicVillagers);
        }
    }
};

class WakeUp final : public Behavior {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "wake_up"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        return v != nullptr && v->sleeping && !c.brain.is_active(Activity::Rest);
    }
    void start(BrainContext& c) override {
        if (VillagerState* v = villager_of(c)) {
            stop_sleeping(c, *v);
        }
    }
};

class LookAtTarget final : public Behavior {
public:
    LookAtTarget() : Behavior{kNeedLook, 45, 90} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "look_at_target"; }

protected:
    bool can_still_use(BrainContext& c) override { return aim(c).has_value(); }
    void tick(BrainContext& c) override {
        if (const auto at = aim(c); at && c.goal.brain != nullptr) {
            c.goal.brain->look_at  = *at;
            c.goal.brain->has_look = true;
        }
    }
    void stop(BrainContext& c) override { c.memories().erase(MT::LookTarget); }

private:
    [[nodiscard]] static std::optional<Vec3d> aim(BrainContext& c) {
        const MemoryValue* v = c.memories().get(MT::LookTarget);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (v->kind == MemoryKind::Pos) {
            return centre(v->pos);
        }
        if (const entity::EntityState* s = remembered(c, MT::LookTarget)) {
            return s->position + Vec3d{0.0, static_cast<f64>(s->eye_height), 0.0};
        }
        return std::nullopt;
    }
};

class MoveToTarget final : public Behavior {
public:
    MoveToTarget() : Behavior{kNeedWalk, 150, 250} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "move_to_target"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        if (reached(c)) {
            c.memories().erase(MT::WalkTarget);
            return false;
        }
        return true;
    }
    bool can_still_use(BrainContext& c) override {
        return c.memories().has(MT::WalkTarget) && !reached(c);
    }
    void start(BrainContext& c) override { route(c); }
    void tick(BrainContext& c) override { route(c); }
    void stop(BrainContext& c) override {
        if (reached(c)) {
            c.memories().erase(MT::WalkTarget);
        }
        halt(c);
    }

private:
    [[nodiscard]] static bool reached(BrainContext& c) {
        const MemoryValue*         target = c.memories().get(MT::WalkTarget);
        const entity::EntityState* self   = c.goal.state();
        return target == nullptr || self == nullptr ||
               manhattan(feet_of(self->position), target->pos) <= target->close_enough;
    }
    static void route(BrainContext& c) {
        const MemoryValue* target = c.memories().get(MT::WalkTarget);
        if (target == nullptr || c.goal.brain == nullptr) {
            return;
        }
        if (c.goal.brain->follower.done() &&
            !move_to(c.goal, target->pos, static_cast<f64>(target->speed), 64.0F) &&
            c.goal.brain->follower.done() && c.goal.tick >= c.goal.brain->next_path_tick - 1) {
            // No route at all: remembered, and the target given up.
            c.memories().set(MT::CantReachWalkTargetSince, MemoryValue::of_number(c.game_time));
            c.memories().erase(MT::WalkTarget);
            return;
        }
        if (!c.goal.brain->follower.done()) {
            c.goal.brain->wants_move = true;
            c.goal.brain->speed      = static_cast<f64>(target->speed);
        }
    }
};

/// Face the trading player and stay put.
class TradeSink final : public Behavior {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "trade"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override { return trading(c); }
    bool can_still_use(BrainContext& c) override { return trading(c); }
    void start(BrainContext& c) override { tick(c); }
    void tick(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        if (v == nullptr || c.goal.brain == nullptr) {
            return;
        }
        c.memories().erase(MT::WalkTarget);
        halt(c);
        c.goal.brain->look_at  = v->trading_with;
        c.goal.brain->has_look = true;
    }

private:
    [[nodiscard]] static bool trading(BrainContext& c) {
        const VillagerState* v = villager_of(c);
        return v != nullptr && v->trading_player >= 0;
    }
};

class AcquireJob final : public Behavior {
public:
    AcquireJob() : Behavior{kAcquireJob, kForeverDuration, kForeverDuration} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "acquire_job"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        return v != nullptr && !v->sleeping && v->trading_player < 0;
    }
    bool can_still_use(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        return v != nullptr && v->claims.potential_job_site && !v->claims.job_site &&
               !c.brain.is_active(Activity::Panic);
    }
    void start(BrainContext& c) override { tick(c); }
    void tick(BrainContext& c) override {
        VillagerState*             v    = villager_of(c);
        const entity::EntityState* self = c.goal.state();
        if (v == nullptr || self == nullptr || !v->claims.potential_job_site) {
            return;
        }
        const BlockPos site = *v->claims.potential_job_site;
        if (dist_sq(self->position, centre(site)) <= kJobSiteReach * kJobSiteReach) {
            v->claims.job_site = site;
            v->claims.potential_job_site.reset();
            if (v->profession == Profession::None && c.goal.level != nullptr) {
                const registry::BlockRegistry& blocks = c.goal.level->blocks();
                if (const auto job = profession_of_job_block(
                        blocks.block_name(blocks.block_of(c.goal.level->block_at(site))))) {
                    set_profession(*v, *job);
                }
            }
            c.memories().erase(MT::WalkTarget);
            return;
        }
        if (!c.memories().has(MT::WalkTarget)) {
            walk_to(c, approach(c.goal.level, site), walk(0.5), 1);
        }
    }
};

class UpdateSchedule final : public Behavior {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "schedule"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        return !c.brain.is_active(Activity::Panic);
    }
    void start(BrainContext& c) override {
        c.brain.set_schedule(baby(c) ? &villager_baby_schedule() : &villager_schedule());
        c.brain.update_from_schedule(c);
    }
};

// ── Idle, work, meet: walking about ─────────────────────────────────────────

/// Somewhere at random within `radius` of where it stands (or of a remembered
/// place), now and then.
class Stroll final : public Behavior {
public:
    Stroll(i32 radius, std::optional<MT> around, i32 odds)
        : Behavior{kNoWalk}, radius_{radius}, around_{around}, odds_{odds} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "stroll"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        return v != nullptr && !v->sleeping && v->trading_player < 0 &&
               c.goal.random != nullptr && c.goal.random->next_int(odds_) == 0 &&
               (!around_ || c.memories().has(*around_));
    }
    void start(BrainContext& c) override {
        const entity::EntityState* self = c.goal.state();
        if (self == nullptr) {
            return;
        }
        const BlockPos from = around_ ? *c.memories().pos(*around_) : feet_of(self->position);
        const i32      span = 2 * radius_ + 1;
        const i32      dx   = c.goal.random->next_int(span) - radius_;
        const i32      dz   = c.goal.random->next_int(span) - radius_;
        walk_to(c, BlockPos{from.x + dx, feet_of(self->position).y, from.z + dz}, walk(0.5), 1);
    }

private:
    i32               radius_{10};
    std::optional<MT> around_;
    i32               odds_{40};
};

/// Walk to a remembered place when farther than `close` (vanilla's StrollToPoi).
class GoToPoi final : public Behavior {
public:
    GoToPoi(MT memory, i32 close, std::string_view label)
        : Behavior{kNoWalk}, memory_{memory}, close_{close}, label_{label} {}
    [[nodiscard]] std::string_view name() const noexcept override { return label_; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState*       v    = villager_of(c);
        const entity::EntityState* self = c.goal.state();
        const auto                 poi  = c.memories().pos(memory_);
        if (v == nullptr || self == nullptr || !poi || v->sleeping || v->trading_player >= 0 ||
            c.game_time < next_) {
            return false;
        }
        return manhattan(feet_of(self->position), *poi) > close_;
    }
    void start(BrainContext& c) override {
        next_ = c.game_time + 20;
        const BlockPos poi = *c.memories().pos(memory_);
        walk_to(c, close_ <= 1 ? approach(c.goal.level, poi) : poi, walk(0.5), close_);
    }

private:
    MT               memory_;
    i32              close_{1};
    std::string_view label_;
    i64              next_{0};
};

// ── Idle and meet: other villagers ──────────────────────────────────────────

/// Pick a villager near by to talk to (idle: anyone within 8; meet: anyone at
/// the bell).
class MeetVillager final : public Behavior {
public:
    MeetVillager(i32 odds, bool at_bell) : Behavior{kNoInteraction}, odds_{odds}, at_bell_{at_bell} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "meet_villager"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState*       v    = villager_of(c);
        const entity::EntityState* self = c.goal.state();
        if (v == nullptr || self == nullptr || baby(c) || v->sleeping || v->trading_player >= 0 ||
            c.goal.random == nullptr || c.goal.random->next_int(odds_) != 0) {
            return false;
        }
        if (at_bell_) {
            const auto bell = c.memories().pos(MT::MeetingPoint);
            if (!bell || dist_sq(self->position, centre(*bell)) > 8.0 * 8.0) {
                return false;
            }
        }
        found_ = nearest_villager(c, 8.0, [](const MobBrain&) { return true; });
        return found_ != entity::kNoEntity;
    }
    void start(BrainContext& c) override {
        const entity::EntityState* other = c.goal.entities->state(found_);
        if (other == nullptr) {
            return;
        }
        c.memories().set(MT::InteractionTarget, MemoryValue::of_entity(other->network_id));
        c.memories().set(MT::LookTarget, MemoryValue::of_entity(other->network_id));
    }

private:
    i32                  odds_{20};
    bool                 at_bell_{false};
    entity::EntityHandle found_{entity::kNoEntity};
};

/// Walk up to the villager one is interacting with and, close enough, gossip
/// (vanilla's TradeWithVillager).
class GossipWith final : public Behavior {
public:
    GossipWith() : Behavior{kNeedInteraction, 60, 60} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "gossip"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override { return other(c) != nullptr; }
    bool can_still_use(BrainContext& c) override {
        const entity::EntityState* o    = other(c);
        const entity::EntityState* self = c.goal.state();
        return o != nullptr && self != nullptr && dist_sq(o->position, self->position) <= 64.0;
    }
    void start(BrainContext& c) override {
        if (const entity::EntityState* o = other(c)) {
            walk_to(c, feet_of(o->position), walk(0.5), 2);
        }
    }
    void tick(BrainContext& c) override {
        entity::EntityHandle       handle = entity::kNoEntity;
        const entity::EntityState* o      = other(c, &handle);
        const entity::EntityState* self   = c.goal.state();
        VillagerState*             v      = villager_of(c);
        if (o == nullptr || self == nullptr || v == nullptr) {
            return;
        }
        c.memories().set(MT::LookTarget, MemoryValue::of_entity(o->network_id));
        if (dist_sq(o->position, self->position) > kGossipReachSq) {
            return;
        }
        MobBrain* ob = villager_brain(*c.goal.entities, handle);
        if (ob == nullptr) {
            return;
        }
        VillagerState& w   = ob->villager;
        const i64      now = c.game_time;
        const auto     free = [&](i64 last) { return last < 0 || now < last || now >= last + kGossipCooldown; };
        if (free(v->last_gossip_time) && free(w.last_gossip_time) && c.goal.random != nullptr) {
            v->gossips.transfer_from(w.gossips, *c.goal.random, 10);
            v->last_gossip_time = now;
            w.last_gossip_time  = now;
            (void)spawn_golem_if_needed(c, kGolemGossipVillagers);
        }
    }
    void stop(BrainContext& c) override { c.memories().erase(MT::InteractionTarget); }

private:
    [[nodiscard]] static const entity::EntityState* other(BrainContext& c,
                                                          entity::EntityHandle* h = nullptr) {
        entity::EntityHandle       handle = entity::kNoEntity;
        const entity::EntityState* s      = remembered(c, MT::InteractionTarget, &handle);
        if (s == nullptr || villager_brain(*c.goal.entities, handle) == nullptr) {
            return nullptr;
        }
        if (h != nullptr) {
            *h = handle;
        }
        return s;
    }
};

// ── Breeding ────────────────────────────────────────────────────────────────

class FindMate final : public Behavior {
public:
    FindMate() : Behavior{kNoBreed} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "find_mate"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        if (v == nullptr || !can_breed(*v, baby(c)) || v->sleeping || v->trading_player >= 0 ||
            c.goal.random == nullptr || c.goal.random->next_int(20) != 0) {
            return false;
        }
        found_ = nearest_villager(c, kMateReach, [](const MobBrain& b) {
            return can_breed(b.villager, b.animal.baby()) && !b.memories.has(MT::BreedTarget) &&
                   b.villager.trading_player < 0;
        });
        return found_ != entity::kNoEntity;
    }
    void start(BrainContext& c) override {
        const entity::EntityState* other = c.goal.entities->state(found_);
        const entity::EntityState* self  = c.goal.state();
        MobBrain*                  ob    = villager_brain(*c.goal.entities, found_);
        if (other == nullptr || self == nullptr || ob == nullptr) {
            return;
        }
        c.memories().set(MT::BreedTarget, MemoryValue::of_entity(other->network_id));
        ob->memories.set(MT::BreedTarget, MemoryValue::of_entity(self->network_id));
    }

private:
    entity::EntityHandle found_{entity::kNoEntity};
};

/// The couple walks together and, 275 to 324 ticks later, if a free bed is
/// within 48 blocks, a baby is born. Both eat their 12 food points whether or
/// not there is a bed (measured: the pair with two beds ate its bread and had
/// no baby).
class MakeLove final : public Behavior {
public:
    MakeLove() : Behavior{kNeedBreed, 350, 350} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "make_love"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        MobBrain*            ob = partner(c);
        return v != nullptr && ob != nullptr && can_breed(*v, baby(c)) &&
               can_breed(ob->villager, ob->animal.baby());
    }
    bool can_still_use(BrainContext& c) override {
        MobBrain* ob = partner(c);
        return ob != nullptr && c.memories().has(MT::BreedTarget) && !done_;
    }
    void start(BrainContext& c) override {
        done_     = false;
        birth_at_ = c.game_time + 275 + (c.goal.random != nullptr ? c.goal.random->next_int(50) : 0);
        tick(c);
    }
    void tick(BrainContext& c) override {
        entity::EntityHandle       handle = entity::kNoEntity;
        const entity::EntityState* o      = remembered(c, MT::BreedTarget, &handle);
        const entity::EntityState* self   = c.goal.state();
        VillagerState*             v      = villager_of(c);
        MobBrain*                  ob     = partner(c);
        if (o == nullptr || self == nullptr || v == nullptr || ob == nullptr) {
            done_ = true;
            return;
        }
        c.memories().set(MT::LookTarget, MemoryValue::of_entity(o->network_id));
        if (!c.memories().has(MT::WalkTarget) && dist_sq(o->position, self->position) > 2.0) {
            walk_to(c, feet_of(o->position), walk(0.5), 1);
        }
        if (dist_sq(o->position, self->position) > 25.0 || c.game_time < birth_at_) {
            return;
        }
        // The lower id carries the birth out, so a couple has one baby.
        if (self->network_id > o->network_id) {
            return;
        }
        eat_for_breeding(*v);
        eat_for_breeding(ob->villager);
        const std::optional<BlockPos> bed = free_bed(c, feet_of(self->position));
        if (bed) {
            push_event(c, VillagerEvent{VillagerEventKind::Birth, c.goal.self, handle, *bed,
                                        self->position, {}});
            c.goal.brain->animal.age = kParentCooldown;
            ob->animal.age           = kParentCooldown;
        } else {
            push_event(c, VillagerEvent{VillagerEventKind::NoBed, c.goal.self, handle, {},
                                        self->position, {}});
        }
        ob->memories.erase(MT::BreedTarget);
        done_ = true;
    }
    void stop(BrainContext& c) override { c.memories().erase(MT::BreedTarget); }

private:
    [[nodiscard]] static MobBrain* partner(BrainContext& c) {
        entity::EntityHandle h = entity::kNoEntity;
        if (remembered(c, MT::BreedTarget, &h) == nullptr) {
            return nullptr;
        }
        return villager_brain(*c.goal.entities, h);
    }
    /// A bed within 48 blocks (±8 up and down, as the claim scan) that no
    /// villager has as its home and nobody lies in.
    [[nodiscard]] static std::optional<BlockPos> free_bed(BrainContext& c, BlockPos from) {
        if (c.goal.level == nullptr || c.goal.entities == nullptr) {
            return std::nullopt;
        }
        const world::LevelView&        level = *c.goal.level;
        const registry::BlockRegistry& blocks = level.blocks();
        const BedRules                 beds{blocks};
        std::optional<BlockPos>        best;
        i64                            best_d = 0;
        const i32                      r      = kBabyBedReach;
        for (i32 dy = -kScanHalfHeight; dy <= kScanHalfHeight; ++dy) {
            for (i32 dz = -r; dz <= r; ++dz) {
                for (i32 dx = -r; dx <= r; ++dx) {
                    const i64 d = static_cast<i64>(dx) * dx + static_cast<i64>(dy) * dy +
                                  static_cast<i64>(dz) * dz;
                    if (d > static_cast<i64>(r) * r || (best && d >= best_d)) {
                        continue;
                    }
                    const BlockPos pos{from.x + dx, from.y + dy, from.z + dz};
                    const registry::BlockStateId state = level.block_at(pos);
                    if (state == registry::kAirState || !beds.is_bed(state)) {
                        continue;
                    }
                    const auto bed = beds.bed_at(level, pos);
                    if (bed && bed->head == pos && !bed->occupied &&
                        !claimed_by_other(*c.goal.entities, entity::kNoEntity, pos)) {
                        best   = pos;
                        best_d = d;
                    }
                }
            }
        }
        return best;
    }

    i64  birth_at_{0};
    bool done_{false};
};

// ── Work ────────────────────────────────────────────────────────────────────

/// Every 300 ticks, one chance in two: standing within 1.73 of the job
/// site's centre, work — `last_worked_at_poi`, and a restock when one is due.
class WorkAtPoi final : public Behavior {
public:
    WorkAtPoi() : Behavior{kNeedJob} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "work_at_poi"; }
    [[nodiscard]] i32 restocks() const noexcept { return restocks_; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const entity::EntityState* self = c.goal.state();
        const auto                 job  = c.memories().pos(MT::JobSite);
        if (self == nullptr || !job || c.game_time - last_check_ < 300 ||
            c.goal.random == nullptr || c.goal.random->next_int(2) != 0) {
            return false;
        }
        last_check_ = c.game_time;
        return dist_sq(self->position, centre(*job)) < 1.73 * 1.73;
    }
    void start(BrainContext& c) override {
        c.memories().set(MT::LastWorkedAtPoi, MemoryValue::of_number(c.game_time));
        VillagerState* v = villager_of(c);
        if (v != nullptr && needs_restock(*v) && allowed_to_restock(*v, c.game_time)) {
            restock(*v, c.game_time);
            ++restocks_;
        }
    }

private:
    i64 last_check_{-300};
    i32 restocks_{0};
};

/// A farmer breaks the ripe crops round its feet and plants its seeds on bare
/// farmland (the wiki's farmer; the search is the block round it, ±1).
class Harvest final : public Behavior {
public:
    Harvest() : Behavior{kNeedJob, 200, 200} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "harvest"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const VillagerState* v = villager_of(c);
        if (v == nullptr || v->profession != Profession::Farmer || v->trading_player >= 0 ||
            c.goal.level == nullptr) {
            return false;
        }
        target_ = pick(c);
        return target_.has_value();
    }
    bool can_still_use(BrainContext&) override { return worked_ < 200; }
    void start(BrainContext& c) override {
        worked_ = 0;
        if (target_) {
            walk_to(c, *target_, walk(0.5), 1);
        }
    }
    void tick(BrainContext& c) override {
        ++worked_;
        const entity::EntityState* self = c.goal.state();
        VillagerState*             v    = villager_of(c);
        if (self == nullptr || v == nullptr) {
            return;
        }
        if (target_ && dist_sq(self->position, centre(*target_)) > 1.0 &&
            manhattan(feet_of(self->position), *target_) > 1) {
            return;
        }
        if (target_ && c.game_time > next_ok_) {
            const world::LevelView&        level  = *c.goal.level;
            const registry::BlockRegistry& blocks = level.blocks();
            const registry::BlockStateId   state  = level.block_at(*target_);
            const registry::BlockStateId   below  =
                level.block_at(BlockPos{target_->x, target_->y - 1, target_->z});
            if (ripe(blocks, state)) {
                push_event(c, VillagerEvent{VillagerEventKind::Harvest, c.goal.self,
                                            entity::kNoEntity, *target_, self->position, {}});
            } else if (state == registry::kAirState && farmland(blocks, below)) {
                for (const std::string_view seed : kSeeds) {
                    if (take_from_pocket(*v, seed)) {
                        push_event(c, VillagerEvent{VillagerEventKind::Plant, c.goal.self,
                                                    entity::kNoEntity, *target_, self->position,
                                                    seed});
                        break;
                    }
                }
            }
            next_ok_ = c.game_time + 20;
            target_  = pick(c);
            if (target_) {
                walk_to(c, *target_, walk(0.5), 1);
            }
        }
    }

private:
    [[nodiscard]] std::optional<BlockPos> pick(BrainContext& c) {
        const entity::EntityState* self = c.goal.state();
        const VillagerState*       v    = villager_of(c);
        if (self == nullptr || v == nullptr || c.goal.random == nullptr) {
            return std::nullopt;
        }
        const world::LevelView&        level  = *c.goal.level;
        const registry::BlockRegistry& blocks = level.blocks();
        const bool has_seeds = std::ranges::any_of(v->inventory, [](const VillagerState::Slot& s) {
            return s.count > 0 && std::ranges::find(kSeeds, s.item) != kSeeds.end();
        });
        std::array<BlockPos, 27> found{};
        usize                    n    = 0;
        const BlockPos           feet = feet_of(self->position);
        for (i32 dx = -1; dx <= 1; ++dx) {
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    const BlockPos at{feet.x + dx, feet.y + dy, feet.z + dz};
                    const registry::BlockStateId state = level.block_at(at);
                    const bool plantable =
                        has_seeds && state == registry::kAirState &&
                        farmland(blocks, level.block_at(BlockPos{at.x, at.y - 1, at.z}));
                    if (ripe(blocks, state) || plantable) {
                        found[n++] = at;
                    }
                }
            }
        }
        if (n == 0) {
            return std::nullopt;
        }
        return found[static_cast<usize>(c.goal.random->next_int(static_cast<i32>(n)))];
    }

    std::optional<BlockPos> target_;
    i32                     worked_{0};
    i64                     next_ok_{0};
};

// ── Rest ────────────────────────────────────────────────────────────────────

/// Within 2 blocks of the bed's centre, a bed nobody lies in: sleep. Held
/// until the rest activity ends (vanilla's SleepInBed, which never times out).
class Sleep final : public Behavior {
public:
    Sleep() : Behavior{kNeedHome, kForeverDuration, kForeverDuration} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "sleep"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override {
        const entity::EntityState* self = c.goal.state();
        const auto                 home = c.memories().pos(MT::Home);
        const VillagerState*       v    = villager_of(c);
        if (self == nullptr || !home || v == nullptr || c.game_time < next_ok_ ||
            v->trading_player >= 0 || c.goal.level == nullptr) {
            return false;
        }
        const BedRules beds{c.goal.level->blocks()};
        const auto     bed = beds.bed_at(*c.goal.level, *home);
        return bed && !bed->occupied && dist_sq(self->position, centre(*home)) <= 4.0;
    }
    bool can_still_use(BrainContext& c) override {
        return c.brain.is_active(Activity::Rest) && c.memories().has(MT::Home);
    }
    void start(BrainContext& c) override {
        if (VillagerState* v = villager_of(c)) {
            start_sleeping(c, *v);
        }
        tick(c);
    }
    void tick(BrainContext& c) override {
        entity::EntityState* self = c.goal.state();
        const auto           home = c.memories().pos(MT::Home);
        if (self == nullptr || !home) {
            return;
        }
        // Lying on the bed, still.
        self->position = Vec3d{static_cast<f64>(home->x) + 0.5, static_cast<f64>(home->y) + 0.5625,
                               static_cast<f64>(home->z) + 0.5};
        self->velocity = Vec3d{};
        halt(c);
    }
    void stop(BrainContext& c) override {
        if (VillagerState* v = villager_of(c); v != nullptr && v->sleeping) {
            stop_sleeping(c, *v);
            next_ok_ = c.game_time + 40;
        }
    }

private:
    i64 next_ok_{0};
};

// ── Panic ───────────────────────────────────────────────────────────────────

class CalmDown final : public Behavior {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "calm_down"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override { return !frightened(c.memories()); }
    void start(BrainContext& c) override {
        c.brain.set_schedule(baby(c) ? &villager_baby_schedule() : &villager_schedule());
        c.brain.update_from_schedule(c);
    }
};

class Flee final : public Behavior {
public:
    Flee() : Behavior{{}, kForeverDuration, kForeverDuration} {}
    [[nodiscard]] std::string_view name() const noexcept override { return "flee"; }

protected:
    bool check_extra_start_conditions(BrainContext& c) override { return frightened(c.memories()); }
    bool can_still_use(BrainContext& c) override { return frightened(c.memories()); }
    void start(BrainContext& c) override { away(c); }
    void tick(BrainContext& c) override {
        if (!c.memories().has(MT::WalkTarget)) {
            away(c);
        }
    }

private:
    static void away(BrainContext& c) {
        const entity::EntityState* self = c.goal.state();
        if (self == nullptr || c.goal.random == nullptr) {
            return;
        }
        f64 dx = 0.0;
        f64 dz = 0.0;
        if (const entity::EntityState* hostile = remembered(c, MT::NearestHostile)) {
            dx = self->position.x - hostile->position.x;
            dz = self->position.z - hostile->position.z;
        }
        const f64 length = std::sqrt(dx * dx + dz * dz);
        if (length < 1e-6) {
            const f64 angle = static_cast<f64>(c.goal.random->next_float()) * 6.283185307179586;
            dx              = std::cos(angle);
            dz              = std::sin(angle);
        } else {
            dx /= length;
            dz /= length;
        }
        const BlockPos feet   = feet_of(self->position);
        const i32      jitter = c.goal.random->next_int(5) - 2;
        walk_to(c,
                BlockPos{feet.x + static_cast<i32>(std::lround(dx * 10.0)) + jitter, feet.y,
                         feet.z + static_cast<i32>(std::lround(dz * 10.0)) - jitter},
                static_cast<f32>(kVillagerPanicSpeed), 0);
    }
};

}  // namespace

// ── Public ──────────────────────────────────────────────────────────────────

i32 food_points(std::string_view item) noexcept {
    for (const Food& f : kFoods) {
        if (f.item == item) {
            return f.points;
        }
    }
    return 0;
}

bool wanted_item(std::string_view item) noexcept {
    return std::ranges::find(kWanted, item) != kWanted.end();
}

std::string_view wanted_item_name(std::string_view item) noexcept {
    const auto it = std::ranges::find(kWanted, item);
    return it != kWanted.end() ? *it : std::string_view{};
}

i32 food_available(const VillagerState& villager) noexcept {
    i32 sum = villager.food_level;
    for (const VillagerState::Slot& s : villager.inventory) {
        sum += food_points(s.item) * s.count;
    }
    return sum;
}

bool can_breed(const VillagerState& villager, bool is_baby) noexcept {
    return !is_baby && !villager.sleeping && !villager.wandering &&
           food_available(villager) >= kBreedFood;
}

void eat_for_breeding(VillagerState& villager) noexcept {
    for (VillagerState::Slot& s : villager.inventory) {
        const i32 points = food_points(s.item);
        while (villager.food_level < kBreedFood && points > 0 && s.count > 0) {
            villager.food_level += points;
            if (--s.count == 0) {
                s.item = {};
            }
        }
    }
    villager.food_level = std::max(0, villager.food_level - kBreedFood);
}

i32 pocket(VillagerState& villager, std::string_view item, i32 count) noexcept {
    const std::string_view name = wanted_item_name(item);
    if (name.empty() || count <= 0) {
        return count;
    }
    constexpr i32 kStack = 64;
    for (VillagerState::Slot& s : villager.inventory) {
        if (s.item == name && s.count < kStack) {
            const i32 moved = std::min(kStack - s.count, count);
            s.count += moved;
            count -= moved;
        }
    }
    for (VillagerState::Slot& s : villager.inventory) {
        if (count > 0 && s.count == 0) {
            s.item  = name;
            s.count = std::min(kStack, count);
            count -= s.count;
        }
    }
    return count;
}

bool take_from_pocket(VillagerState& villager, std::string_view item) noexcept {
    for (VillagerState::Slot& s : villager.inventory) {
        if (s.item == item && s.count > 0) {
            if (--s.count == 0) {
                s.item = {};
            }
            return true;
        }
    }
    return false;
}

bool wants_golem(const Memories& memories, i64 game_time) noexcept {
    const auto slept = memories.number(MemoryType::LastSlept);
    return slept && game_time - *slept < kGolemSleptWithin &&
           !memories.has(MemoryType::GolemDetectedRecently);
}

std::optional<BlockPos> golem_spawn_position(const world::LevelView& level, BlockPos origin,
                                             math::LegacyRandomSource& random) {
    const registry::BlockRegistry& blocks = level.blocks();
    const auto free = [&](registry::BlockStateId s) {
        return blocks.collision_boxes(s).empty();
    };
    const auto floor = [&](registry::BlockStateId s) {
        if (s == registry::kAirState || !blocks.face_is_sturdy(s, Face::Up)) {
            return false;
        }
        const registry::BlockId b    = blocks.block_of(s);
        const std::string_view  name = blocks.block_name(b);
        // The wiki's exclusions: no glass, leaves, ice, cobweb, cactus, conduit.
        return !blocks.is_leaves(b) && name.find("glass") == std::string_view::npos &&
               name != "minecraft:ice" && name != "minecraft:cobweb" &&
               name != "minecraft:cactus" && name != "minecraft:conduit";
    };
    for (i32 attempt = 0; attempt < kGolemSpawnAttempts; ++attempt) {
        const i32 dx = random.next_int(2 * kGolemSpawnHorizontal + 1) - kGolemSpawnHorizontal;
        const i32 dz = random.next_int(2 * kGolemSpawnHorizontal + 1) - kGolemSpawnHorizontal;
        for (i32 dy = kGolemSpawnVertical; dy >= -kGolemSpawnVertical; --dy) {
            const BlockPos at{origin.x + dx, origin.y + dy, origin.z + dz};
            if (!level.is_loaded(at)) {
                break;
            }
            if (floor(level.block_at(BlockPos{at.x, at.y - 1, at.z})) && free(level.block_at(at)) &&
                free(level.block_at(BlockPos{at.x, at.y + 1, at.z})) &&
                free(level.block_at(BlockPos{at.x, at.y + 2, at.z}))) {
                return at;
            }
        }
    }
    return std::nullopt;
}

bool spawn_golem_if_needed(BrainContext& c, i32 required) {
    const entity::EntityState* self = c.goal.state();
    if (self == nullptr || c.goal.entities == nullptr || c.goal.level == nullptr ||
        c.goal.random == nullptr || !wants_golem(c.memories(), c.game_time)) {
        return false;
    }
    std::array<MobBrain*, 32> near{};
    usize                     near_count = 0;
    i32                       wanting    = 0;
    for (const entity::EntityHandle h : c.goal.entities->handles()) {
        const entity::EntityState* s = c.goal.entities->state(h);
        MobBrain*                  b = villager_brain(*c.goal.entities, h);
        if (s == nullptr || b == nullptr || s->removed || s->health <= 0.0F) {
            continue;
        }
        if (std::abs(s->position.x - self->position.x) > kGolemCountReach ||
            std::abs(s->position.y - self->position.y) > kGolemCountReach ||
            std::abs(s->position.z - self->position.z) > kGolemCountReach) {
            continue;
        }
        if (near_count < near.size()) {
            near[near_count++] = b;
        }
        if (wanting < kGolemGossipVillagers && wants_golem(b->memories, c.game_time)) {
            ++wanting;
        }
    }
    if (wanting < required) {
        return false;
    }
    const auto at = golem_spawn_position(*c.goal.level, feet_of(self->position), *c.goal.random);
    if (!at) {
        return false;
    }
    push_event(c, VillagerEvent{VillagerEventKind::SummonGolem, c.goal.self, entity::kNoEntity, *at,
                                Vec3d{static_cast<f64>(at->x) + 0.5, static_cast<f64>(at->y),
                                      static_cast<f64>(at->z) + 0.5},
                                {}});
    for (usize i = 0; i < near_count; ++i) {
        near[i]->memories.set(MemoryType::GolemDetectedRecently, MemoryValue::unit(),
                              kGolemDetectedTicks);
    }
    return true;
}

std::unique_ptr<Brain> make_villager_brain(math::LegacyRandomSource& random) {
    auto b = std::make_unique<Brain>();
    b->set_core(Activity::Core);
    b->set_default(Activity::Idle);
    b->set_schedule(&villager_schedule());
    b->require(Activity::Work, {{MT::JobSite, MS::Present}});
    b->require(Activity::Meet, {{MT::MeetingPoint, MS::Present}});
    b->set_sense(&sense);
    b->add_sensor(kSenseHostile, 20, random);
    b->add_sensor(kSenseHurt, 1, random);
    b->add_sensor(kSenseGolem, 200, random);

    // core
    b->add(Activity::Core, 0, std::make_unique<RememberPois>());
    b->add(Activity::Core, 0, std::make_unique<PanicTrigger>());
    b->add(Activity::Core, 0, std::make_unique<WakeUp>());
    b->add(Activity::Core, 0, std::make_unique<LookAtTarget>());
    b->add(Activity::Core, 1, std::make_unique<MoveToTarget>());
    b->add(Activity::Core, 2, std::make_unique<TradeSink>());
    b->add(Activity::Core, 7, std::make_unique<AcquireJob>());
    b->add(Activity::Core, 99, std::make_unique<UpdateSchedule>());
    // idle
    b->add(Activity::Idle, 1, std::make_unique<FindMate>());
    b->add(Activity::Idle, 2, std::make_unique<MeetVillager>(20, false));
    b->add(Activity::Idle, 3, std::make_unique<MakeLove>());
    b->add(Activity::Idle, 3, std::make_unique<GossipWith>());
    b->add(Activity::Idle, 5, std::make_unique<Stroll>(10, std::nullopt, 40));
    // play (babies)
    b->add(Activity::Play, 5, std::make_unique<Stroll>(10, std::nullopt, 30));
    // work
    b->add(Activity::Work, 5, std::make_unique<GoToPoi>(MT::JobSite, 1, "go_to_job"));
    b->add(Activity::Work, 5, std::make_unique<Harvest>());
    b->add(Activity::Work, 7, std::make_unique<WorkAtPoi>());
    b->add(Activity::Work, 9, std::make_unique<Stroll>(4, MT::JobSite, 60));
    // meet
    b->add(Activity::Meet, 2, std::make_unique<GoToPoi>(MT::MeetingPoint, 6, "go_to_bell"));
    b->add(Activity::Meet, 2, std::make_unique<MeetVillager>(10, true));
    b->add(Activity::Meet, 2, std::make_unique<GossipWith>());
    b->add(Activity::Meet, 9, std::make_unique<Stroll>(6, MT::MeetingPoint, 60));
    // rest
    b->add(Activity::Rest, 0, std::make_unique<Sleep>());
    b->add(Activity::Rest, 1, std::make_unique<GoToPoi>(MT::Home, 1, "go_home"));
    // panic
    b->add(Activity::Panic, 0, std::make_unique<CalmDown>());
    b->add(Activity::Panic, 1, std::make_unique<Flee>());
    return b;
}

}  // namespace ov::gameplay::brain
