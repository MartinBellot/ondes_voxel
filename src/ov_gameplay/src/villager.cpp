#include "ov/gameplay/villager.hpp"

#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/sleep.hpp"
#include "ov/gameplay/trading.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::gameplay {
namespace {

constexpr std::array<std::string_view, kVillagerTypeCount> kTypeNames{
    "minecraft:desert", "minecraft:jungle", "minecraft:plains", "minecraft:savanna",
    "minecraft:snow",   "minecraft:swamp",  "minecraft:taiga",
};

constexpr std::array<std::string_view, kProfessionCount> kProfessionNames{
    "minecraft:none",      "minecraft:armorer",   "minecraft:butcher",
    "minecraft:cartographer", "minecraft:cleric", "minecraft:farmer",
    "minecraft:fisherman", "minecraft:fletcher",  "minecraft:leatherworker",
    "minecraft:librarian", "minecraft:mason",     "minecraft:nitwit",
    "minecraft:shepherd",  "minecraft:toolsmith", "minecraft:weaponsmith",
};

struct JobBlock {
    std::string_view block;
    Profession       profession;
};

// The thirteen acquirable job sites. The block of each POI type is the Wiki's
// ("Villager", job site blocks); the thirteen were then measured one by one
// (measure_villagers.py `claim`). The leatherworker's POI holds every cauldron.
constexpr std::array<JobBlock, 16> kJobBlocks{{
    {"minecraft:blast_furnace", Profession::Armorer},
    {"minecraft:smoker", Profession::Butcher},
    {"minecraft:cartography_table", Profession::Cartographer},
    {"minecraft:brewing_stand", Profession::Cleric},
    {"minecraft:composter", Profession::Farmer},
    {"minecraft:barrel", Profession::Fisherman},
    {"minecraft:fletching_table", Profession::Fletcher},
    {"minecraft:cauldron", Profession::Leatherworker},
    {"minecraft:water_cauldron", Profession::Leatherworker},
    {"minecraft:lava_cauldron", Profession::Leatherworker},
    {"minecraft:powder_snow_cauldron", Profession::Leatherworker},
    {"minecraft:lectern", Profession::Librarian},
    {"minecraft:stonecutter", Profession::Mason},
    {"minecraft:loom", Profession::Shepherd},
    {"minecraft:smithing_table", Profession::Toolsmith},
    {"minecraft:grindstone", Profession::Weaponsmith},
}};

// ── Speeds ──
// Walking: the law of elevage.md, v = 2.1586 · (attribute × modifier)², with
// modifier 0.5 — 0.1349 blocks a tick. Measured: 0.126 to 0.14 on the walk to
// a far job block and after a panic calms (villageois.md).
constexpr f64 kWalkModifier = 0.5;
// Panic: 0.21 to 0.24 blocks a tick, median 0.225, over twelve intervals of
// the flee campaign (from a zombie and after a hit). Carried as measured: it
// lies just past the speeds the walking law was fitted on.
constexpr f64 kPanicSpeed = 0.225;

// Named fields: since mobs-2 a kind carries the attribute and one modifier per
// goal, and a positional list would put the next number in the wrong one.
const MobKind kVillagerKind{
    .type_name      = "minecraft:villager",
    .category       = MobCategory::Misc,
    .movement_speed = kVillagerSpeedAttribute,
    .stroll         = kWalkModifier,
    .opens_doors    = true,
};

[[nodiscard]] BlockPos feet_of(const entity::EntityState& state) noexcept {
    return BlockPos{static_cast<i32>(std::floor(state.position.x)),
                    static_cast<i32>(std::floor(state.position.y)),
                    static_cast<i32>(std::floor(state.position.z))};
}

[[nodiscard]] f64 centre_distance_sq(const Vec3d& at, BlockPos block) noexcept {
    const f64 dx = at.x - (static_cast<f64>(block.x) + 0.5);
    const f64 dy = at.y - (static_cast<f64>(block.y) + 0.5);
    const f64 dz = at.z - (static_cast<f64>(block.z) + 0.5);
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] i64 block_distance_sq(BlockPos a, BlockPos b) noexcept {
    const i64 dx = a.x - b.x;
    const i64 dy = a.y - b.y;
    const i64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

/// Where a villager stands to reach a block: the first horizontal neighbour
/// with room for its feet, else the block itself (the path finder then gets as
/// close as it can).
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

[[nodiscard]] bool wants_job(const VillagerState& v, bool baby) noexcept {
    if (baby || v.profession == Profession::Nitwit) {
        return false;
    }
    return !v.claims.job_site && !v.claims.potential_job_site;
}

}  // namespace

// ── Identity ────────────────────────────────────────────────────────────────

std::string_view villager_type_name(VillagerType type) noexcept {
    return kTypeNames[static_cast<usize>(type)];
}

std::string_view profession_name(Profession profession) noexcept {
    return kProfessionNames[static_cast<usize>(profession)];
}

std::optional<VillagerType> villager_type_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kTypeNames.size(); ++i) {
        if (kTypeNames[i] == name) {
            return static_cast<VillagerType>(i);
        }
    }
    return std::nullopt;
}

std::optional<Profession> profession_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kProfessionNames.size(); ++i) {
        if (kProfessionNames[i] == name) {
            return static_cast<Profession>(i);
        }
    }
    return std::nullopt;
}

std::optional<Profession> profession_of_job_block(std::string_view block) noexcept {
    for (const JobBlock& job : kJobBlocks) {
        if (job.block == block) {
            return job.profession;
        }
    }
    return std::nullopt;
}

std::string_view job_block_of(Profession profession) noexcept {
    for (const JobBlock& job : kJobBlocks) {
        if (job.profession == profession) {
            return job.block;
        }
    }
    return {};
}

Activity activity_at(i64 day_time) noexcept {
    const i64 t = ((day_time % 24000) + 24000) % 24000;
    if (t < 10 || t >= 12000) {
        return Activity::Rest;
    }
    if (t < 2000) {
        return Activity::Idle;
    }
    if (t < 9000) {
        return Activity::Work;
    }
    if (t < 11000) {
        return Activity::Meet;
    }
    return Activity::Idle;
}

// ── The species ─────────────────────────────────────────────────────────────

const MobKind* villager_mob_kind(std::string_view type_name) noexcept {
    return type_name == kVillagerKind.type_name ? &kVillagerKind : nullptr;
}

void install_villager_goals(GoalSelector& selector, const MobKind& kind, i32 look_type) {
    const f64 walk  = kind.speed(kind.stroll);
    const f64 panic = kPanicSpeed;
    selector.add(0, std::make_unique<FloatGoal>());
    selector.add(1, std::make_unique<VillagerPanicGoal>(panic));
    selector.add(1, std::make_unique<TradeWithPlayerGoal>());
    selector.add(2, std::make_unique<SleepInBedGoal>(walk));
    selector.add(3, std::make_unique<AcquireJobSiteGoal>(walk));
    selector.add(4, std::make_unique<WorkAtJobSiteGoal>(walk));
    selector.add(6, std::make_unique<RandomStrollGoal>(walk));
    selector.add(7, std::make_unique<LookAtEntityGoal>(look_type, 8.0, 0.02F));
    selector.add(8, std::make_unique<RandomLookGoal>());
}

void init_villager(VillagerState& villager, i64 seed) noexcept {
    villager.active = true;
    villager.random.set_seed(seed ^ 0x5649'4C4C'4147'4552LL);
}

void set_profession(VillagerState& villager, Profession profession) noexcept {
    if (villager.profession == profession) {
        return;
    }
    villager.profession   = profession;
    villager.offers_drawn = false;
    villager.offers.clear();
    ++villager.revision;
}

bool claimed_by_other(entity::EntityWorld& entities, entity::EntityHandle self, BlockPos pos) {
    for (const entity::EntityHandle handle : entities.handles()) {
        if (handle == self) {
            continue;
        }
        const MobBrain* brain = mob_brain_of(entities, handle);
        if (brain == nullptr || !brain->villager.active) {
            continue;
        }
        const VillagerClaims& c = brain->villager.claims;
        if (c.job_site == pos || c.potential_job_site == pos || c.home == pos) {
            return true;
        }
    }
    return false;
}

void scan_step(VillagerState& villager, const world::LevelView& level, BlockPos feet,
               bool (*claimed)(entity::EntityWorld&, entity::EntityHandle, BlockPos),
               entity::EntityWorld& entities, entity::EntityHandle self, i32 budget) {
    VillagerScan& s = villager.scan;
    const i32     r = std::max(kJobSearchRadius, kBedSearchRadius);
    if (!s.active) {
        s.active = true;
        s.origin = feet;
        s.layer  = -kScanHalfHeight;
        s.row    = -r;
        s.best_job.reset();
        s.best_bed.reset();
    }
    const registry::BlockRegistry& blocks = level.blocks();
    const BedRules                 beds{blocks};
    const i64                      job_limit = static_cast<i64>(kJobSearchRadius) * kJobSearchRadius;
    const i64                      bed_limit = static_cast<i64>(kBedSearchRadius) * kBedSearchRadius;
    const bool                     job_wanted = villager.profession != Profession::Nitwit;
    const bool                     bed_wanted = !villager.claims.home.has_value();

    // Terrain repeats: the answer for the last state seen is kept.
    registry::BlockStateId last_state = registry::kAirState;
    std::optional<Profession> last_job;
    bool                      last_bed = false;

    i32 done = 0;
    while (done < budget && s.layer <= kScanHalfHeight) {
        const i32 dy     = s.layer;
        const i32 dz     = s.row;
        const i32 left   = r * r - dy * dy - dz * dz;
        if (left >= 0) {
            const i32 half = static_cast<i32>(std::sqrt(static_cast<f64>(left)));
            for (i32 dx = -half; dx <= half; ++dx) {
                const BlockPos pos{s.origin.x + dx, s.origin.y + dy, s.origin.z + dz};
                const registry::BlockStateId state = level.block_at(pos);
                ++done;
                if (state == registry::kAirState) {
                    continue;
                }
                if (state != last_state) {
                    last_state             = state;
                    const std::string_view name = blocks.block_name(blocks.block_of(state));
                    last_job               = profession_of_job_block(name);
                    last_bed               = beds.is_bed(state);
                }
                const i64 d = block_distance_sq(pos, s.origin);
                if (last_job && job_wanted && d <= job_limit &&
                    (villager.profession == Profession::None || *last_job == villager.profession) &&
                    (!s.best_job || d < s.best_job_distance) && !claimed(entities, self, pos)) {
                    s.best_job          = pos;
                    s.best_job_distance = d;
                }
                if (last_bed && bed_wanted && d <= bed_limit &&
                    (!s.best_bed || d < s.best_bed_distance)) {
                    const auto bed = beds.bed_at(level, pos);
                    if (bed && bed->head == pos && !bed->occupied &&
                        !claimed(entities, self, pos)) {
                        s.best_bed          = pos;
                        s.best_bed_distance = d;
                    }
                }
            }
        }
        if (++s.row > r) {
            s.row = -r;
            ++s.layer;
        }
    }
}

void tick_villager(VillagerState& villager, const entity::EntityState& self_state,
                   entity::EntityHandle self, entity::EntityWorld& entities,
                   const world::LevelView* level, const VillagerWorld* world, bool baby, i64 tick) {
    if (!villager.active) {
        return;
    }
    if (villager.unhappy > 0) {
        --villager.unhappy;
    }
    if (villager.hurt_ticks > 0) {
        --villager.hurt_ticks;
    }
    if (world != nullptr) {
        note_day(villager, world->day_time);
    }

    if (level != nullptr) {
        const registry::BlockRegistry& blocks = level->blocks();
        const auto job_at = [&](BlockPos pos) -> std::optional<Profession> {
            return profession_of_job_block(blocks.block_name(blocks.block_of(level->block_at(pos))));
        };
        // A job block that went is forgotten; an unloaded one is not judged.
        if (villager.claims.job_site && level->is_loaded(*villager.claims.job_site)) {
            const auto job = job_at(*villager.claims.job_site);
            if (!job || (villager.profession != Profession::None && *job != villager.profession)) {
                villager.claims.job_site.reset();
            }
        }
        if (villager.claims.potential_job_site &&
            level->is_loaded(*villager.claims.potential_job_site) &&
            !job_at(*villager.claims.potential_job_site)) {
            villager.claims.potential_job_site.reset();
        }
        if (villager.claims.home && level->is_loaded(*villager.claims.home) &&
            !BedRules{blocks}.is_bed(level->block_at(*villager.claims.home))) {
            villager.claims.home.reset();
            if (villager.sleeping) {
                villager.sleeping = false;
                ++villager.revision;
            }
        }

        // The scan: for a job while jobless (or for one's own trade's block
        // while the old one is gone), and for a bed while homeless.
        const bool job_needed = wants_job(villager, baby);
        const bool bed_needed = !villager.claims.home.has_value();
        if ((job_needed || bed_needed) && (villager.scan.active || tick >= villager.scan.next_scan_tick)) {
            scan_step(villager, *level, feet_of(self_state), &claimed_by_other, entities, self,
                      kScanBudget);
            if (villager.scan.layer > kScanHalfHeight) {
                VillagerScan& s = villager.scan;
                s.active        = false;
                bool found      = false;
                if (job_needed && s.best_job && !claimed_by_other(entities, self, *s.best_job)) {
                    villager.claims.potential_job_site = s.best_job;
                    found                              = true;
                }
                if (bed_needed && s.best_bed && !claimed_by_other(entities, self, *s.best_bed)) {
                    villager.claims.home = s.best_bed;
                    found                = true;
                }
                s.next_scan_tick = tick + (found ? 20 : kScanPause);
            }
        }
    }

    // Losing the job: a profession with no job site, before any trade.
    if (!villager.claims.job_site && !villager.claims.potential_job_site &&
        villager.profession != Profession::None && villager.profession != Profession::Nitwit &&
        villager.xp == 0 && villager.level <= 1) {
        set_profession(villager, Profession::None);
    }

    (void)tick_level_up(villager);
}

// ── Goals ───────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] VillagerState* villager_of(GoalContext& context) noexcept {
    return context.brain != nullptr && context.brain->villager.active ? &context.brain->villager
                                                                      : nullptr;
}

[[nodiscard]] Activity activity(const GoalContext& context) noexcept {
    return context.villagers != nullptr ? activity_at(context.villagers->day_time) : Activity::Idle;
}

/// The nearest hostile within its own sight distance, or null.
[[nodiscard]] const entity::EntityState* threat(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.villagers == nullptr || context.entities == nullptr) {
        return nullptr;
    }
    const entity::EntityState* best      = nullptr;
    f64                        best_dist = 0.0;
    for (const entity::EntityHandle handle : context.entities->handles()) {
        const entity::EntityState* other = context.entities->state(handle);
        if (other == nullptr || other->removed || other->health <= 0.0F || handle == context.self) {
            continue;
        }
        for (const HostileSight& sight : context.villagers->hostiles) {
            if (sight.type != other->type) {
                continue;
            }
            const f64 dx = other->position.x - self->position.x;
            const f64 dy = other->position.y - self->position.y;
            const f64 dz = other->position.z - self->position.z;
            const f64 d  = dx * dx + dy * dy + dz * dz;
            const f64 r  = static_cast<f64>(sight.distance);
            if (d <= r * r && (best == nullptr || d < best_dist)) {
                best      = other;
                best_dist = d;
            }
        }
    }
    return best;
}

void keep_going(GoalContext& context, f64 speed) {
    if (context.brain != nullptr && !context.brain->follower.done()) {
        context.brain->wants_move = true;
        context.brain->speed      = speed;
    }
}

void halt(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

}  // namespace

bool VillagerPanicGoal::pick_away(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.random == nullptr) {
        return false;
    }
    const entity::EntityState* hostile = threat(context);
    f64                        dx      = 0.0;
    f64                        dz      = 0.0;
    if (hostile != nullptr) {
        dx = self->position.x - hostile->position.x;
        dz = self->position.z - hostile->position.z;
    }
    const f64 length = std::sqrt(dx * dx + dz * dz);
    if (length < 1e-6) {
        const f64 angle = static_cast<f64>(context.random->next_float()) * 6.283185307179586;
        dx              = std::cos(angle);
        dz              = std::sin(angle);
    } else {
        dx /= length;
        dz /= length;
    }
    const BlockPos feet   = feet_of(*self);
    const i32      jitter = context.random->next_int(5) - 2;
    away_ = BlockPos{feet.x + static_cast<i32>(std::lround(dx * 10.0)) + jitter, feet.y,
                     feet.z + static_cast<i32>(std::lround(dz * 10.0)) - jitter};
    return true;
}

bool VillagerPanicGoal::can_use(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    if (v == nullptr || v->sleeping) {
        return false;
    }
    return (v->hurt_ticks > 0 || threat(context) != nullptr) && pick_away(context);
}

bool VillagerPanicGoal::can_continue_to_use(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    if (v == nullptr) {
        return false;
    }
    const bool afraid = v->hurt_ticks > 0 || threat(context) != nullptr;
    if (afraid && context.brain->follower.done() && pick_away(context)) {
        (void)move_to(context, away_, speed_, 32.0F);
    }
    return afraid;
}

void VillagerPanicGoal::start(GoalContext& context) { (void)move_to(context, away_, speed_, 32.0F); }

void VillagerPanicGoal::tick(GoalContext& context) { keep_going(context, speed_); }

void VillagerPanicGoal::stop(GoalContext& context) { halt(context); }

bool TradeWithPlayerGoal::can_use(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    return v != nullptr && v->trading_player >= 0;
}

void TradeWithPlayerGoal::start(GoalContext& context) { halt(context); }

void TradeWithPlayerGoal::tick(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    if (v == nullptr || context.brain == nullptr) {
        return;
    }
    context.brain->wants_move = false;
    context.brain->look_at    = v->trading_with;
    context.brain->has_look   = true;
}

bool AcquireJobSiteGoal::can_use(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    return v != nullptr && !v->sleeping && v->claims.potential_job_site && !v->claims.job_site;
}

bool AcquireJobSiteGoal::can_continue_to_use(GoalContext& context) { return can_use(context); }

void AcquireJobSiteGoal::start(GoalContext& context) {
    VillagerState* v = villager_of(context);
    if (v != nullptr && v->claims.potential_job_site) {
        (void)move_to(context, approach(context.level, *v->claims.potential_job_site), speed_,
                      64.0F);
    }
}

void AcquireJobSiteGoal::tick(GoalContext& context) {
    VillagerState*             v    = villager_of(context);
    const entity::EntityState* self = context.state();
    if (v == nullptr || self == nullptr || !v->claims.potential_job_site) {
        return;
    }
    const BlockPos site = *v->claims.potential_job_site;
    if (centre_distance_sq(self->position, site) <= kJobSiteReach * kJobSiteReach) {
        v->claims.job_site = site;
        v->claims.potential_job_site.reset();
        if (v->profession == Profession::None && context.level != nullptr) {
            const registry::BlockRegistry& blocks = context.level->blocks();
            if (const auto job = profession_of_job_block(
                    blocks.block_name(blocks.block_of(context.level->block_at(site))))) {
                set_profession(*v, *job);
            }
        }
        halt(context);
        return;
    }
    if (context.brain->follower.done()) {
        (void)move_to(context, approach(context.level, site), speed_, 64.0F);
    }
    keep_going(context, speed_);
}

void AcquireJobSiteGoal::stop(GoalContext& context) { halt(context); }

bool WorkAtJobSiteGoal::can_use(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    return v != nullptr && !v->sleeping && v->trading_player < 0 && v->claims.job_site &&
           activity(context) == Activity::Work && context.tick >= v->next_work_tick;
}

bool WorkAtJobSiteGoal::can_continue_to_use(GoalContext& context) { return can_use(context); }

void WorkAtJobSiteGoal::start(GoalContext& context) {
    VillagerState* v = villager_of(context);
    if (v != nullptr && v->claims.job_site) {
        (void)move_to(context, approach(context.level, *v->claims.job_site), speed_, 64.0F);
    }
}

void WorkAtJobSiteGoal::tick(GoalContext& context) {
    VillagerState*             v    = villager_of(context);
    const entity::EntityState* self = context.state();
    if (v == nullptr || self == nullptr || !v->claims.job_site) {
        return;
    }
    const BlockPos site = *v->claims.job_site;
    if (centre_distance_sq(self->position, site) <= kJobSiteReach * kJobSiteReach) {
        const i64 now = context.villagers != nullptr ? context.villagers->game_time : context.tick;
        if (needs_restock(*v) && allowed_to_restock(*v, now)) {
            restock(*v, now);
            ++restocks_;
        }
        // Vanilla works at its block on a cooldown; 300 ticks between visits.
        v->next_work_tick = context.tick + 300;
        halt(context);
        return;
    }
    if (context.brain->follower.done()) {
        (void)move_to(context, approach(context.level, site), speed_, 64.0F);
    }
    keep_going(context, speed_);
}

void WorkAtJobSiteGoal::stop(GoalContext& context) { halt(context); }

bool SleepInBedGoal::can_use(GoalContext& context) {
    const VillagerState* v = villager_of(context);
    return v != nullptr && v->trading_player < 0 && v->claims.home &&
           activity(context) == Activity::Rest;
}

bool SleepInBedGoal::can_continue_to_use(GoalContext& context) { return can_use(context); }

void SleepInBedGoal::start(GoalContext& context) {
    VillagerState* v = villager_of(context);
    if (v != nullptr && v->claims.home && !v->sleeping) {
        (void)move_to(context, approach(context.level, *v->claims.home), speed_, 64.0F);
    }
}

void SleepInBedGoal::tick(GoalContext& context) {
    VillagerState*       v    = villager_of(context);
    entity::EntityState* self = context.state();
    if (v == nullptr || self == nullptr || !v->claims.home) {
        return;
    }
    const BlockPos bed = *v->claims.home;
    if (v->sleeping) {
        // Lying on the bed: held there, head still.
        self->position = Vec3d{static_cast<f64>(bed.x) + 0.5, static_cast<f64>(bed.y) + 0.5625,
                               static_cast<f64>(bed.z) + 0.5};
        self->velocity = Vec3d{};
        context.brain->wants_move = false;
        return;
    }
    if (centre_distance_sq(self->position, bed) <= 1.5 * 1.5 + 1.0) {
        v->sleeping = true;
        ++v->revision;
        halt(context);
        return;
    }
    if (context.brain->follower.done()) {
        (void)move_to(context, approach(context.level, bed), speed_, 64.0F);
    }
    keep_going(context, speed_);
}

void SleepInBedGoal::stop(GoalContext& context) {
    VillagerState* v = villager_of(context);
    if (v != nullptr && v->sleeping) {
        v->sleeping = false;
        ++v->revision;
    }
    halt(context);
}

}  // namespace ov::gameplay
