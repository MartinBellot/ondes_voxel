// ── tame ── See tame.hpp; the numbers are in docs/provenance/apprivoisement.md.
#include "ov/gameplay/tame.hpp"

#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::gameplay {
namespace {

[[nodiscard]] BlockPos block_at(const Vec3d& at) noexcept {
    return BlockPos{static_cast<i32>(std::floor(at.x)), static_cast<i32>(std::floor(at.y)),
                    static_cast<i32>(std::floor(at.z))};
}

[[nodiscard]] f64 dist_sq(const Vec3d& a, const Vec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    const f64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

/// Re-assert the intent to walk, every tick (goals.cpp's `keep_walking`: the
/// mob clears it at the top of each of its ticks).
void keep_moving(GoalContext& context, f64 speed) {
    if (context.brain == nullptr || context.brain->follower.done()) {
        return;
    }
    context.brain->wants_move = true;
    context.brain->speed      = speed;
}

void stand_still(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

// ── The food lists ──────────────────────────────────────────────────────────
//
// The wiki's, per species; the taming ones are confirmed by the `tame`
// campaign (every try answered 6 or 7), the wolf's meats by `wolf`.
constexpr std::array<std::string_view, 1> kBone{"minecraft:bone"};
constexpr std::array<std::string_view, 2> kFish{"minecraft:cod", "minecraft:salmon"};
constexpr std::array<std::string_view, 6> kSeeds{
    "minecraft:wheat_seeds",    "minecraft:melon_seeds",       "minecraft:pumpkin_seeds",
    "minecraft:beetroot_seeds", "minecraft:torchflower_seeds", "minecraft:pitcher_pod"};

struct Meat {
    std::string_view item;
    i32              heal;
};
/// Everything a wolf eats, and its hunger points — which is what it heals.
constexpr std::array<Meat, 12> kMeat{{
    {"minecraft:beef", 3},
    {"minecraft:cooked_beef", 8},
    {"minecraft:porkchop", 3},
    {"minecraft:cooked_porkchop", 8},
    {"minecraft:chicken", 2},
    {"minecraft:cooked_chicken", 6},
    {"minecraft:mutton", 2},
    {"minecraft:cooked_mutton", 6},
    {"minecraft:rabbit", 3},
    {"minecraft:cooked_rabbit", 5},
    {"minecraft:rabbit_stew", 10},
    {"minecraft:rotten_flesh", 4},
}};
constexpr std::array<std::string_view, 12> kMeatNames{
    kMeat[0].item, kMeat[1].item, kMeat[2].item,  kMeat[3].item,  kMeat[4].item,  kMeat[5].item,
    kMeat[6].item, kMeat[7].item, kMeat[8].item, kMeat[9].item, kMeat[10].item, kMeat[11].item};
/// What breeds a horse, a donkey (and, for healing and temper, a mule).
constexpr std::array<std::string_view, 3> kGoldenFood{
    "minecraft:golden_carrot", "minecraft:golden_apple", "minecraft:enchanted_golden_apple"};
constexpr std::array<std::string_view, 1> kHay{"minecraft:hay_block"};

const std::array<TameKind, 16> kTame{{
    //  type, family, taming food, odds, food, max temper, sits, follows, collar, tempt, own
    {"minecraft:wolf", TameFamily::Wolf, kBone, 3, kMeatNames, 0, true, true, true, 0.0, true},
    {"minecraft:cat", TameFamily::Cat, kFish, 3, kFish, 0, true, true, true, 0.6, true},
    {"minecraft:ocelot", TameFamily::Ocelot, kFish, 3, kFish, 0, false, false, false, 0.6, true},
    {"minecraft:parrot", TameFamily::Parrot, kSeeds, 10, {}, 0, true, true, false, 0.0, true},
    {"minecraft:horse", TameFamily::Horse, {}, 0, kGoldenFood, 100, false, false, false, 1.25, true},
    {"minecraft:donkey", TameFamily::Horse, {}, 0, kGoldenFood, 100, false, false, false, 1.25, true},
    {"minecraft:mule", TameFamily::Horse, {}, 0, kGoldenFood, 100, false, false, false, 1.25, true},
    {"minecraft:llama", TameFamily::Llama, {}, 0, kHay, 30, false, false, false, 1.25, true},
    {"minecraft:trader_llama", TameFamily::Llama, {}, 0, kHay, 30, false, false, false, 1.25, true},
    // Their brains are the ordinary animal's (husbandry feeds and breeds
    // them, breeding.cpp); what they keep here is a variant or a flag.
    {"minecraft:rabbit", TameFamily::Rabbit, {}, 0, {}, 0, false, false, false, 0.0, false},
    {"minecraft:fox", TameFamily::Fox, {}, 0, {}, 0, false, false, false, 0.0, false},
    {"minecraft:turtle", TameFamily::Turtle, {}, 0, {}, 0, false, false, false, 0.0, false},
    {"minecraft:bee", TameFamily::Bee, {}, 0, {}, 0, false, false, false, 0.0, false},
    {"minecraft:goat", TameFamily::Goat, {}, 0, {}, 0, false, false, false, 0.0, false},
    {"minecraft:camel", TameFamily::Camel, {}, 0, {}, 0, false, false, false, 0.0, false},
    {"minecraft:sniffer", TameFamily::None, {}, 0, {}, 0, false, false, false, 0.0, false},
}};

// Tempting lists, for the goals that need an AnimalKind (TemptGoal).
const AnimalKind kFishTempt{"minecraft:cat", kFish, 1.0, false, false, false, false, 0.0F};
const AnimalKind kGoldenTempt{"minecraft:horse", kGoldenFood, 1.25, false, false, false, false,
                              0.0F};
const AnimalKind kHayTempt{"minecraft:llama", kHay, 1.25, false, false, false, false, 0.0F};

// ── The species the table of mob_species.cpp does not carry ─────────────────
//
// `movement_speed` is the measured attribute (normalized/entities.json). The
// goal modifiers were **not** measured for these species: the horse family's
// 0.7 stroll and 1.2 panic are the horse's (mobs-2.md § 1.3) on the ground
// that all three share the same goals; the others walk at 1.0. Named in
// apprivoisement.md.
constexpr MobKind creature(std::string_view type, f64 attribute, f64 stroll, f64 panic,
                           bool breeds) {
    MobKind kind{};
    kind.type_name      = type;
    kind.category       = MobCategory::Creature;
    kind.movement_speed = attribute;
    kind.stroll         = stroll;
    kind.panic          = panic;
    kind.panics         = panic > 0.0;
    kind.breeds         = breeds;
    kind.hostile        = false;
    return kind;
}

constexpr std::array<MobKind, 11> kKinds{{
    creature("minecraft:donkey", 0.175, 0.7, 1.2, true),
    creature("minecraft:mule", 0.175, 0.7, 1.2, false),
    creature("minecraft:llama", 0.175, 0.7, 1.2, true),
    creature("minecraft:trader_llama", 0.175, 0.7, 1.2, false),
    creature("minecraft:ocelot", 0.3, 0.8, 0.0, true),
    creature("minecraft:parrot", 0.2, 1.0, 1.25, false),
    creature("minecraft:turtle", 0.25, 1.0, 1.2, true),
    creature("minecraft:bee", 0.3, 1.0, 0.0, true),
    creature("minecraft:goat", 0.2, 1.0, 1.25, true),
    creature("minecraft:camel", 0.09, 1.0, 4.0, true),
    creature("minecraft:sniffer", 0.1, 1.0, 2.0, true),
}};

}  // namespace

// ── Lookups ─────────────────────────────────────────────────────────────────

const TameKind* tame_kind(std::string_view type_name) noexcept {
    for (const TameKind& kind : kTame) {
        if (kind.type_name == type_name) {
            return &kind;
        }
    }
    return nullptr;
}

const MobKind* tame_mob_kind(std::string_view type_name) noexcept {
    for (const MobKind& kind : kKinds) {
        if (kind.type_name == type_name) {
            return &kind;
        }
    }
    return nullptr;
}

bool is_taming_food(const TameKind& kind, std::string_view item) noexcept {
    return std::ranges::find(kind.taming_food, item) != kind.taming_food.end();
}

bool is_tame_food(const TameKind& kind, std::string_view item) noexcept {
    return std::ranges::find(kind.food, item) != kind.food.end();
}

std::optional<i32> wolf_food_heal(std::string_view item) noexcept {
    for (const Meat& meat : kMeat) {
        if (meat.item == item) {
            return meat.heal;
        }
    }
    return std::nullopt;
}

std::optional<HorseFood> horse_food(std::string_view type_name, std::string_view item) noexcept {
    // The wiki's *Horse* and *Llama* feeding tables: health, ticks of growth,
    // temper, and whether it starts love in a tame adult.
    const bool llama = type_name == "minecraft:llama" || type_name == "minecraft:trader_llama";
    if (llama) {
        if (item == "minecraft:wheat") {
            return HorseFood{2.0F, 200, 3, false};
        }
        if (item == "minecraft:hay_block") {
            return HorseFood{10.0F, 1800, 6, true};
        }
        return std::nullopt;
    }
    if (item == "minecraft:sugar") {
        return HorseFood{1.0F, 30, 3, false};
    }
    if (item == "minecraft:wheat") {
        return HorseFood{2.0F, 20, 3, false};
    }
    if (item == "minecraft:apple") {
        return HorseFood{3.0F, 60, 3, false};
    }
    if (item == "minecraft:golden_carrot") {
        return HorseFood{4.0F, 60, 5, true};
    }
    if (item == "minecraft:golden_apple" || item == "minecraft:enchanted_golden_apple") {
        return HorseFood{10.0F, 240, 10, true};
    }
    if (item == "minecraft:hay_block") {
        return HorseFood{20.0F, 180, 0, false};
    }
    return std::nullopt;
}

const Owner* find_owner(std::span<const Owner> owners, const net::Uuid& uuid) noexcept {
    for (const Owner& owner : owners) {
        if (owner.uuid == uuid) {
            return &owner;
        }
    }
    return nullptr;
}

const Owner* find_owner(std::span<const Owner> owners, i32 network_id) noexcept {
    for (const Owner& owner : owners) {
        if (owner.network_id == network_id) {
            return &owner;
        }
    }
    return nullptr;
}

// ── Draws ───────────────────────────────────────────────────────────────────

bool taming_roll(math::LegacyRandomSource& random, i32 odds) noexcept {
    return odds > 0 && random.next_int(odds) == 0;
}

bool temper_tames(math::LegacyRandomSource& random, i32 temper, i32 max_temper) noexcept {
    return max_temper > 0 && random.next_int(max_temper) < temper;
}

i32 draw_anger_time(math::LegacyRandomSource& random) noexcept {
    return 400 + random.next_int(381);
}

HorseRanges horse_ranges(std::string_view type_name) noexcept {
    HorseRanges ranges;
    if (type_name != "minecraft:horse") {
        // Donkeys, mules and llamas: a drawn health, a fixed walk and jump
        // (their attributes, entities.json: 0.175 and 0.5).
        ranges.speed_drawn = false;
        ranges.jump_drawn  = false;
    }
    return ranges;
}

HorseStats draw_horse_stats(std::string_view type_name, math::LegacyRandomSource& random) noexcept {
    const HorseRanges ranges = horse_ranges(type_name);
    HorseStats        stats;
    stats.max_health = 15.0 + static_cast<f64>(random.next_int(8) + random.next_int(9));
    if (ranges.speed_drawn) {
        const f64 a = random.next_double();
        const f64 b = random.next_double();
        const f64 c = random.next_double();
        stats.speed = (0.45 + a * 0.3 + b * 0.3 + c * 0.3) * 0.25;
    } else {
        stats.speed = ranges.fixed_speed;
    }
    if (ranges.jump_drawn) {
        const f64 a = random.next_double();
        const f64 b = random.next_double();
        const f64 c = random.next_double();
        stats.jump = 0.4 + a * 0.2 + b * 0.2 + c * 0.2;
    } else {
        stats.jump = ranges.fixed_jump;
    }
    return stats;
}

f64 offspring_stat(f64 a, f64 b, f64 min, f64 max, math::LegacyRandomSource& random) noexcept {
    const f64 spread = std::abs(a - b) + 0.3 * (max - min);
    const f64 r1     = random.next_double();
    const f64 r2     = random.next_double();
    const f64 r3     = random.next_double();
    f64       value  = (a + b) * 0.5 + spread * ((r1 + r2 + r3) / 3.0 - 0.5);
    if (value > max) {
        value = 2.0 * max - value;
    } else if (value < min) {
        value = 2.0 * min - value;
    }
    return std::clamp(value, min, max);
}

HorseStats offspring_stats(std::string_view child_type, const HorseStats& mother,
                           const HorseStats& father, math::LegacyRandomSource& random) noexcept {
    const HorseRanges ranges = horse_ranges(child_type);
    HorseStats        child;
    child.max_health = offspring_stat(mother.max_health, father.max_health, ranges.health_min,
                                      ranges.health_max, random);
    child.speed = ranges.speed_drawn
                      ? offspring_stat(mother.speed, father.speed, ranges.speed_min,
                                       ranges.speed_max, random)
                      : ranges.fixed_speed;
    child.jump = ranges.jump_drawn ? offspring_stat(mother.jump, father.jump, ranges.jump_min,
                                                    ranges.jump_max, random)
                                   : ranges.fixed_jump;
    return child;
}

i32 draw_llama_strength(math::LegacyRandomSource& random) noexcept {
    const i32 top = random.next_float() < 0.04F ? 5 : 3;
    return 1 + random.next_int(top);
}

void init_tame(TameState& tame, const TameKind& kind, math::LegacyRandomSource& random) noexcept {
    tame.family = kind.family;
    switch (kind.family) {
    case TameFamily::Horse:
        tame.stats = draw_horse_stats(kind.type_name, random);
        if (kind.type_name == "minecraft:horse") {
            // Seven colours, five markings: Variant = colour | markings << 8.
            const i32 colour   = random.next_int(7);
            const i32 markings = random.next_int(5);
            tame.variant       = colour | (markings << 8);
        }
        break;
    case TameFamily::Llama:
        tame.stats    = draw_horse_stats(kind.type_name, random);
        tame.strength = draw_llama_strength(random);
        tame.variant  = random.next_int(4);
        break;
    case TameFamily::Parrot:
        tame.variant = random.next_int(5);
        break;
    case TameFamily::Cat:
        // Eleven variants. Vanilla's choice depends on the moon and on witch
        // huts; a uniform draw is named in apprivoisement.md.
        tame.variant = random.next_int(11);
        break;
    case TameFamily::Rabbit: {
        // The temperate types — brown, black, black-and-white, salt — evenly;
        // snow and desert rabbits by biome are named, not done.
        constexpr std::array<i32, 4> kTemperate{0, 2, 3, 5};
        tame.variant = kTemperate[static_cast<usize>(random.next_int(4))];
        break;
    }
    case TameFamily::Goat:
        // One goat in fifty screams (the wiki); both horns on.
        tame.flag = random.next_int(50) == 0;
        break;
    case TameFamily::None:
    case TameFamily::Wolf:
    case TameFamily::Ocelot:
    case TameFamily::Fox:
    case TameFamily::Turtle:
    case TameFamily::Bee:
    case TameFamily::Camel:
        break;
    }
}

void apply_tame_body(TameState& tame, entity::EntityState& state) noexcept {
    f32 wanted = 0.0F;
    if (tame.stats.drawn()) {
        wanted = static_cast<f32>(tame.stats.max_health);
    } else if (tame.family == TameFamily::Wolf) {
        wanted = tame.tame ? kTameWolfHealth : kWildWolfHealth;
    }
    if (wanted <= 0.0F || state.max_health == wanted) {
        return;
    }
    // A body at its old maximum is a fresh one (spawned at the registry's 53
    // for a horse): it starts full at the new maximum.
    const bool full  = state.health >= state.max_health;
    state.max_health = wanted;
    state.health     = full ? wanted : std::min(state.health, wanted);
}

void tick_tame(TameState& tame, entity::EntityState& state) noexcept {
    (void)state;
    if (tame.anger > 0) {
        --tame.anger;
        if (tame.anger == 0) {
            tame.angry_at.reset();
            tame.dirty = true;
        }
    }
    tame.ridden_for = tame.rider != 0 ? tame.ridden_for + 1 : 0;
}

f64 tame_speed_factor(const TameState& tame, f64 species_attribute) noexcept {
    if (!tame.stats.drawn() || tame.stats.speed <= 0.0 || species_attribute <= 0.0) {
        return 1.0;
    }
    const f64 ratio = tame.stats.speed / species_attribute;
    return ratio * ratio;
}

bool rider_controls(const TameState& tame) noexcept {
    return tame.rider != 0 && tame.tame && tame.saddled && tame.family == TameFamily::Horse;
}

// ── Goal lists ──────────────────────────────────────────────────────────────

void install_tame_goals(GoalSelector& selector, const MobKind& kind, const TameKind& tame,
                        i32 look_type, i32 quarry_type) {
    (void)quarry_type;
    selector.add(0, std::make_unique<FloatGoal>());
    switch (tame.family) {
    case TameFamily::Wolf:
        selector.add(2, std::make_unique<SitWhenOrderedGoal>());
        selector.add(5, std::make_unique<MeleeAttackGoal>(kind.speed(1.0), kMeleeCooldownTicks,
                                                          0.0, true));
        selector.add(6, std::make_unique<FollowOwnerGoal>(kind.speed(1.0)));
        selector.add(7, std::make_unique<BreedGoal>(kind.speed(1.0), kMateReach));
        selector.add(8, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
        // Targets: what hurt the owner, what the owner hit, then its own anger.
        selector.add(1, std::make_unique<OwnerTargetGoal>(OwnerTargetGoal::Mode::HurtBy));
        selector.add(2, std::make_unique<OwnerTargetGoal>(OwnerTargetGoal::Mode::Attacked));
        selector.add(3, std::make_unique<AngerTargetGoal>());
        break;
    case TameFamily::Cat:
        if (kind.panics) {
            selector.add(1, std::make_unique<PanicGoal>(kind.speed(kind.panic)));
        }
        selector.add(1, std::make_unique<SitWhenOrderedGoal>());
        selector.add(4, std::make_unique<TemptGoal>(kFishTempt, kind.speed(tame.tempt_speed)));
        selector.add(6, std::make_unique<FollowOwnerGoal>(kind.speed(1.0)));
        selector.add(9, std::make_unique<BreedGoal>(kind.speed(0.8), kMateReach));
        selector.add(10, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
        break;
    case TameFamily::Ocelot:
        selector.add(3, std::make_unique<TemptGoal>(kFishTempt, kind.speed(tame.tempt_speed)));
        selector.add(9, std::make_unique<BreedGoal>(kind.speed(0.8), kMateReach));
        selector.add(10, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
        break;
    case TameFamily::Parrot:
        if (kind.panics) {
            selector.add(0, std::make_unique<PanicGoal>(kind.speed(kind.panic)));
        }
        selector.add(2, std::make_unique<SitWhenOrderedGoal>());
        selector.add(2, std::make_unique<FollowOwnerGoal>(kind.speed(1.0)));
        selector.add(3, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
        break;
    case TameFamily::Horse:
    case TameFamily::Llama:
        if (kind.panics) {
            selector.add(1, std::make_unique<PanicGoal>(kind.speed(kind.panic)));
        }
        selector.add(1, std::make_unique<RideTantrumGoal>(kind.speed(1.2), tame.max_temper));
        if (kind.breeds) {
            selector.add(2, std::make_unique<BreedGoal>(kind.speed(1.0), kMateReach));
        }
        selector.add(4, std::make_unique<TemptGoal>(
                            tame.family == TameFamily::Llama ? kHayTempt : kGoldenTempt,
                            kind.speed(tame.tempt_speed)));
        selector.add(5, std::make_unique<FollowParentGoal>(kind.speed(1.0)));
        selector.add(6, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
        break;
    case TameFamily::None:
    case TameFamily::Rabbit:
    case TameFamily::Fox:
    case TameFamily::Turtle:
    case TameFamily::Bee:
    case TameFamily::Goat:
    case TameFamily::Camel:
        selector.add(6, std::make_unique<RandomStrollGoal>(kind.speed(kind.stroll)));
        break;
    }
    selector.add(10, std::make_unique<LookAtEntityGoal>(look_type, 8.0, 0.02F));
    selector.add(11, std::make_unique<RandomLookGoal>());
}

void install_creeper_fear(GoalSelector& selector, const MobKind& kind) {
    selector.add(2, std::make_unique<AvoidEntityGoal>(6.0, kind.speed(1.0), kind.speed(1.2)));
}

// ── SitWhenOrderedGoal ──────────────────────────────────────────────────────

bool SitWhenOrderedGoal::can_use(GoalContext& context) {
    return context.brain != nullptr && context.brain->tame.tame && context.brain->tame.sitting;
}

void SitWhenOrderedGoal::start(GoalContext& context) { stand_still(context); }

void SitWhenOrderedGoal::tick(GoalContext& context) { stand_still(context); }

// ── FollowOwnerGoal ─────────────────────────────────────────────────────────

namespace {

[[nodiscard]] const Owner* owner_of(GoalContext& context) {
    if (context.brain == nullptr || context.tame_world == nullptr) {
        return nullptr;
    }
    const TameState& tame = context.brain->tame;
    if (!tame.tame || !tame.owner) {
        return nullptr;
    }
    return find_owner(context.tame_world->owners, *tame.owner);
}

/// A spot beside the owner a teleport may land on: the floor below solid, the
/// two blocks of the body free, not on the owner's own block. Ten tries in a
/// seven by seven square a block up or down.
[[nodiscard]] std::optional<BlockPos> landing(GoalContext& context, const Vec3d& owner) {
    if (context.level == nullptr || context.random == nullptr) {
        return std::nullopt;
    }
    const registry::BlockRegistry& blocks = context.level->blocks();
    const BlockPos                 base   = block_at(owner);
    for (i32 attempt = 0; attempt < 10; ++attempt) {
        const i32 dx = context.random->next_int(7) - 3;
        const i32 dy = context.random->next_int(3) - 1;
        const i32 dz = context.random->next_int(7) - 3;
        if (std::abs(dx) < 2 && std::abs(dz) < 2) {
            continue;
        }
        const BlockPos             at{base.x + dx, base.y + dy, base.z + dz};
        const registry::BlockStateId floor = context.level->block_at(at.below());
        const registry::BlockStateId feet  = context.level->block_at(at);
        const registry::BlockStateId head  = context.level->block_at(at.above());
        if (blocks.is_air(blocks.block_of(floor)) || blocks.holds_fluid(floor)) {
            continue;
        }
        if (!blocks.is_air(blocks.block_of(feet)) || !blocks.is_air(blocks.block_of(head))) {
            continue;
        }
        return at;
    }
    return std::nullopt;
}

}  // namespace

bool FollowOwnerGoal::can_use(GoalContext& context) {
    const Owner*               owner = owner_of(context);
    const entity::EntityState* self  = context.state();
    if (owner == nullptr || self == nullptr || context.brain->tame.sitting) {
        return false;
    }
    if (dist_sq(self->position, owner->feet) < kFollowStart * kFollowStart) {
        return false;
    }
    owner_ = owner->feet;
    return true;
}

bool FollowOwnerGoal::can_continue_to_use(GoalContext& context) {
    const Owner*               owner = owner_of(context);
    const entity::EntityState* self  = context.state();
    if (owner == nullptr || self == nullptr || context.brain->tame.sitting) {
        return false;
    }
    owner_ = owner->feet;
    return dist_sq(self->position, owner->feet) > kFollowStop * kFollowStop;
}

void FollowOwnerGoal::stop(GoalContext& context) { stand_still(context); }

void FollowOwnerGoal::tick(GoalContext& context) {
    entity::EntityState* self = context.state();
    if (self == nullptr || context.brain == nullptr) {
        return;
    }
    context.brain->look_at  = Vec3d{owner_.x, owner_.y + 1.62, owner_.z};
    context.brain->has_look = true;
    if (dist_sq(self->position, owner_) >= kTeleportDistance * kTeleportDistance) {
        if (const auto spot = landing(context, owner_)) {
            self->position = Vec3d{static_cast<f64>(spot->x) + 0.5, static_cast<f64>(spot->y),
                                   static_cast<f64>(spot->z) + 0.5};
            self->velocity = Vec3d{};
            context.brain->follower.clear();
            context.brain->wants_move = false;
            if (context.tame_world != nullptr && context.tame_world->events != nullptr) {
                context.tame_world->events->push_back(
                    TameEvent{TameEventKind::Teleported, context.self, 0, self->position});
            }
            return;
        }
    }
    (void)move_to(context, block_at(owner_), speed_, 32.0F);
    keep_moving(context, speed_);
}

// ── OwnerTargetGoal ─────────────────────────────────────────────────────────

namespace {

/// Would a tame animal attack this? Alive, not itself, not a creeper (the
/// wiki: a wolf never attacks one), not another animal of the same owner.
[[nodiscard]] bool fair_target(GoalContext& context, entity::EntityHandle handle,
                               const net::Uuid& owner) {
    if (handle == entity::kNoEntity || handle == context.self || context.entities == nullptr) {
        return false;
    }
    const entity::EntityState* target = context.entities->state(handle);
    if (target == nullptr || target->removed || target->health <= 0.0F) {
        return false;
    }
    if (context.tame_world != nullptr && target->type == context.tame_world->creeper_type) {
        return false;
    }
    if (context.brain_of != nullptr) {
        if (const MobBrain* other = context.brain_of(*context.entities, handle);
            other != nullptr && other->tame.owned_by(owner)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool OwnerTargetGoal::can_use(GoalContext& context) {
    const Owner* owner = owner_of(context);
    if (owner == nullptr || context.brain->tame.sitting) {
        return false;
    }
    const entity::EntityHandle handle = mode_ == Mode::HurtBy ? owner->hurt_by : owner->attacked;
    const i64 when = mode_ == Mode::HurtBy ? owner->hurt_by_tick : owner->attacked_tick;
    if (when < 0 || when == seen_ || !fair_target(context, handle, owner->uuid)) {
        return false;
    }
    seen_  = when;
    found_ = handle;
    return true;
}

bool OwnerTargetGoal::can_continue_to_use(GoalContext& context) {
    if (context.brain == nullptr || context.brain->tame.sitting || context.entities == nullptr) {
        return false;
    }
    const entity::EntityState* self   = context.state();
    const entity::EntityState* target = context.entities->state(context.brain->target);
    if (self == nullptr || target == nullptr || target->removed || target->health <= 0.0F) {
        return false;
    }
    // The follow range of a wolf, 16 (entities.json).
    return dist_sq(self->position, target->position) <= 16.0 * 16.0;
}

void OwnerTargetGoal::start(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target        = found_;
        context.brain->target_player = 0;
    }
}

void OwnerTargetGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target              = entity::kNoEntity;
        context.brain->target_forgotten_at = context.tick;
    }
    found_ = entity::kNoEntity;
}

// ── AngerTargetGoal ─────────────────────────────────────────────────────────

bool AngerTargetGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || context.tame_world == nullptr) {
        return false;
    }
    const TameState& tame = context.brain->tame;
    if (tame.anger <= 0 || !tame.angry_at || tame.sitting) {
        return false;
    }
    const Owner* player = find_owner(context.tame_world->owners, *tame.angry_at);
    if (player == nullptr || find_quarry(context.quarries, player->network_id) == nullptr) {
        return false;
    }
    player_ = player->network_id;
    return true;
}

bool AngerTargetGoal::can_continue_to_use(GoalContext& context) {
    if (context.brain == nullptr || context.brain->tame.anger <= 0) {
        return false;
    }
    const Quarry*              quarry = find_quarry(context.quarries, player_);
    const entity::EntityState* self   = context.state();
    return quarry != nullptr && self != nullptr &&
           dist_sq(self->position, quarry->feet) <= 16.0 * 16.0;
}

void AngerTargetGoal::start(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target        = entity::kNoEntity;
        context.brain->target_player = player_;
    }
}

void AngerTargetGoal::stop(GoalContext& context) {
    if (context.brain != nullptr) {
        context.brain->target_player       = 0;
        context.brain->target_forgotten_at = context.tick;
    }
    player_ = 0;
}

// ── RideTantrumGoal ─────────────────────────────────────────────────────────

bool RideTantrumGoal::can_use(GoalContext& context) {
    if (context.brain == nullptr || context.random == nullptr) {
        return false;
    }
    const TameState& tame = context.brain->tame;
    if (tame.tame || tame.rider == 0) {
        return false;
    }
    const entity::EntityState* self = context.state();
    if (self == nullptr) {
        return false;
    }
    const BlockPos here = block_at(self->position);
    wanted_ = BlockPos{here.x + context.random->next_int(11) - 5, here.y,
                       here.z + context.random->next_int(11) - 5};
    return true;
}

bool RideTantrumGoal::can_continue_to_use(GoalContext& context) {
    return context.brain != nullptr && !context.brain->tame.tame && context.brain->tame.rider != 0;
}

void RideTantrumGoal::start(GoalContext& context) { (void)move_to(context, wanted_, speed_, 16.0F); }

void RideTantrumGoal::stop(GoalContext& context) { stand_still(context); }

void RideTantrumGoal::tick(GoalContext& context) {
    if (context.brain == nullptr || context.random == nullptr) {
        return;
    }
    TameState& tame = context.brain->tame;
    if (context.brain->follower.done()) {
        if (const entity::EntityState* self = context.state()) {
            const BlockPos here = block_at(self->position);
            wanted_ = BlockPos{here.x + context.random->next_int(11) - 5, here.y,
                               here.z + context.random->next_int(11) - 5};
            (void)move_to(context, wanted_, speed_, 16.0F);
        }
    }
    keep_moving(context, speed_);
    if (context.random->next_int(kTantrumOdds) != 0) {
        return;
    }
    const entity::EntityState* self  = context.state();
    const i32                  rider = tame.rider;
    TameEvent                  event;
    event.self   = context.self;
    event.player = rider;
    event.at     = self != nullptr ? self->position : Vec3d{};
    if (temper_tames(*context.random, tame.temper, max_temper_)) {
        tame.tame = true;
        if (context.tame_world != nullptr) {
            if (const Owner* owner = find_owner(context.tame_world->owners, rider)) {
                tame.owner = owner->uuid;
            }
        }
        event.kind = TameEventKind::Tamed;
    } else {
        tame.temper = std::min(tame.temper + kTemperStep, max_temper_);
        event.kind  = TameEventKind::Threw;
    }
    tame.dirty = true;
    if (context.tame_world != nullptr && context.tame_world->events != nullptr) {
        context.tame_world->events->push_back(event);
    }
}

// ── AvoidEntityGoal ─────────────────────────────────────────────────────────

bool AvoidEntityGoal::can_use(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.entities == nullptr || context.tame_world == nullptr) {
        return false;
    }
    entity::EntityHandle best      = entity::kNoEntity;
    f64                  best_dist = radius_ * radius_;
    for (const i32 type : {context.tame_world->cat_type, context.tame_world->ocelot_type}) {
        if (type < 0) {
            continue;
        }
        const entity::EntityHandle found = nearest_entity(*context.entities, context.self, type, radius_);
        const entity::EntityState* other = context.entities->state(found);
        if (other == nullptr) {
            continue;
        }
        const f64 d = dist_sq(self->position, other->position);
        if (d <= best_dist) {
            best      = found;
            best_dist = d;
        }
    }
    if (best == entity::kNoEntity) {
        return false;
    }
    const entity::EntityState* other = context.entities->state(best);
    f64       dx     = self->position.x - other->position.x;
    f64       dz     = self->position.z - other->position.z;
    const f64 length = std::sqrt(dx * dx + dz * dz);
    if (length < 1e-6) {
        dx = 1.0;
        dz = 0.0;
    } else {
        dx /= length;
        dz /= length;
    }
    feared_ = best;
    away_   = block_at(Vec3d{self->position.x + dx * 8.0, self->position.y,
                           self->position.z + dz * 8.0});
    return true;
}

bool AvoidEntityGoal::can_continue_to_use(GoalContext& context) {
    return context.brain != nullptr && !context.brain->follower.done() &&
           context.entities != nullptr && context.entities->state(feared_) != nullptr;
}

void AvoidEntityGoal::start(GoalContext& context) { (void)move_to(context, away_, walk_, 16.0F); }

void AvoidEntityGoal::stop(GoalContext& context) {
    stand_still(context);
    feared_ = entity::kNoEntity;
}

void AvoidEntityGoal::tick(GoalContext& context) {
    const entity::EntityState* self  = context.state();
    const entity::EntityState* other = context.entities != nullptr ? context.entities->state(feared_)
                                                                   : nullptr;
    const bool close = self != nullptr && other != nullptr &&
                       dist_sq(self->position, other->position) < 49.0;
    keep_moving(context, close ? sprint_ : walk_);
}

}  // namespace ov::gameplay
