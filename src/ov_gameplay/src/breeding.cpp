#include "ov/gameplay/breeding.hpp"

#include "ov/gameplay/mob_logic.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

// ── The measured food lists ─────────────────────────────────────────────────
//
// scripts/measure_husbandry.py `food`: each species offered 18 candidate items
// by a probe client, one Interact each, InLove and the stack read back. These
// are exactly the ones that were eaten; nothing else of the 18 was.
constexpr std::array<std::string_view, 1> kWheat{"minecraft:wheat"};
constexpr std::array<std::string_view, 3> kPigFood{"minecraft:carrot", "minecraft:potato",
                                                   "minecraft:beetroot"};
constexpr std::array<std::string_view, 6> kSeeds{
    "minecraft:wheat_seeds",    "minecraft:melon_seeds",       "minecraft:pumpkin_seeds",
    "minecraft:beetroot_seeds", "minecraft:torchflower_seeds", "minecraft:pitcher_pod"};

// ── tame ── The simpler species this wave brings: husbandry feeds, breeds and
// tempts them as it does a cow. The rabbit's list is measured (elevage.md
// § 4.1); the others are the wiki's, and the bee's is the 1.20.1 datapack's
// `#minecraft:flowers` (test_tame.cpp checks it against the generated tag).
constexpr std::array<std::string_view, 3> kRabbitFood{"minecraft:carrot", "minecraft:golden_carrot",
                                                      "minecraft:dandelion"};
constexpr std::array<std::string_view, 2> kFoxFood{"minecraft:sweet_berries",
                                                   "minecraft:glow_berries"};
constexpr std::array<std::string_view, 1> kTurtleFood{"minecraft:seagrass"};
constexpr std::array<std::string_view, 1> kCamelFood{"minecraft:cactus"};
constexpr std::array<std::string_view, 24> kFlowers{
    "minecraft:dandelion",          "minecraft:poppy",
    "minecraft:blue_orchid",        "minecraft:allium",
    "minecraft:azure_bluet",        "minecraft:red_tulip",
    "minecraft:orange_tulip",       "minecraft:white_tulip",
    "minecraft:pink_tulip",         "minecraft:oxeye_daisy",
    "minecraft:cornflower",         "minecraft:lily_of_the_valley",
    "minecraft:wither_rose",        "minecraft:torchflower",
    "minecraft:sunflower",          "minecraft:lilac",
    "minecraft:peony",              "minecraft:rose_bush",
    "minecraft:pitcher_plant",      "minecraft:flowering_azalea_leaves",
    "minecraft:flowering_azalea",   "minecraft:mangrove_propagule",
    "minecraft:cherry_leaves",      "minecraft:pink_petals"};

// Tempt multipliers over the walk speed: measured speeds of a tempted animal
// divided by its measured attribute halved (docs/provenance/elevage.md § 8).
// ── tame ── The six added rows tempt at 1.0: not measured, named.
const std::array<AnimalKind, 10> kAnimals{{
    //  type                food     tempt  eggs   shear  milk   saddle baby eyes
    {"minecraft:cow", kWheat, 1.25, false, false, true, false, 0.665F},
    {"minecraft:sheep", kWheat, 1.1, false, true, false, false, 0.6175F},
    {"minecraft:pig", kPigFood, 1.2, false, false, false, true, 0.3825F},
    {"minecraft:chicken", kSeeds, 1.0, true, false, false, false, 0.2975F},
    {"minecraft:rabbit", kRabbitFood, 1.0, false, false, false, false, 0.0F},
    {"minecraft:fox", kFoxFood, 1.0, false, false, false, false, 0.0F},
    {"minecraft:goat", kWheat, 1.0, false, false, true, false, 0.0F},
    {"minecraft:turtle", kTurtleFood, 1.0, false, false, false, false, 0.0F},
    {"minecraft:bee", kFlowers, 1.0, false, false, false, false, 0.0F},
    {"minecraft:camel", kCamelFood, 1.0, false, false, false, true, 0.0F},
}};

constexpr std::array<std::string_view, 16> kColours{
    "white", "orange", "magenta", "light_blue", "yellow", "lime",  "pink", "gray",
    "light_gray", "cyan", "purple", "blue",    "brown",  "green", "red",  "black"};

enum Colour : i8 {
    kWhite = 0, kOrange = 1, kMagenta = 2, kLightBlue = 3, kYellow = 4, kLime = 5, kPink = 6,
    kGray = 7, kLightGray = 8, kCyan = 9, kPurple = 10, kBlue = 11, kBrown = 12, kGreen = 13,
    kRed = 14, kBlack = 15,
};

struct Mix {
    i8 a, b, result;
};

// The two-dye shapeless recipes of the 1.20.1 datapack, as (a, b) -> result.
// Checked against the generated recipes by test_breeding.cpp, and against the
// game by the `inherit` campaign: these nine, in both orders, and nothing else
// gave a lamb a colour neither parent had.
constexpr std::array<Mix, 9> kMixes{{
    {kBlue, kGreen, kCyan},
    {kBlack, kWhite, kGray},
    {kBlue, kWhite, kLightBlue},
    {kGray, kWhite, kLightGray},
    {kGreen, kWhite, kLime},
    {kPurple, kPink, kMagenta},
    {kRed, kYellow, kOrange},
    {kRed, kWhite, kPink},
    {kBlue, kRed, kPurple},
}};

[[nodiscard]] f64 distance_sq(const Vec3d& a, const Vec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    const f64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] BlockPos block_of(const Vec3d& at) noexcept {
    return BlockPos{static_cast<i32>(std::floor(at.x)), static_cast<i32>(std::floor(at.y)),
                    static_cast<i32>(std::floor(at.z))};
}

}  // namespace

const AnimalKind* animal_kind(std::string_view type_name) noexcept {
    for (const AnimalKind& kind : kAnimals) {
        if (kind.type_name == type_name) {
            return &kind;
        }
    }
    return nullptr;
}

bool is_food(const AnimalKind& kind, std::string_view item) noexcept {
    return std::ranges::find(kind.food, item) != kind.food.end();
}

FeedResult feed(AnimalState& animal, const AnimalKind& kind, std::string_view item,
                i32 player) noexcept {
    if (!is_food(kind, item)) {
        return FeedResult::NotFood;
    }
    if (animal.baby()) {
        // Consumed even when the tenth rounds to nothing: measured at -194,
        // which gained 0 ticks and still took the wheat.
        age_up(animal, feeding_growth(animal.age));
        return FeedResult::Grew;
    }
    if (animal.age > 0 || animal.in_love()) {
        // Measured: a cow at Age 3000, and one already in love, both left the
        // wheat in the hand.
        return FeedResult::Refused;
    }
    animal.love       = kLoveTicks;
    animal.love_cause = player;
    return FeedResult::Love;
}

void age_up(AnimalState& animal, i32 ticks) noexcept {
    if (animal.age < 0) {
        animal.age = std::min(0, animal.age + ticks);
    }
}

i32 wool_count(math::LegacyRandomSource& random) noexcept { return 1 + random.next_int(3); }

i32 breeding_xp(math::LegacyRandomSource& random) noexcept { return 1 + random.next_int(7); }

i32 egg_interval(math::LegacyRandomSource& random) noexcept {
    return 6000 + random.next_int(6000);
}

std::span<const std::string_view, 16> colour_names() noexcept { return kColours; }

std::optional<i8> dye_colour(std::string_view item) noexcept {
    constexpr std::string_view kPrefix = "minecraft:";
    constexpr std::string_view kSuffix = "_dye";
    if (!item.starts_with(kPrefix) || !item.ends_with(kSuffix)) {
        return std::nullopt;
    }
    const std::string_view colour =
        item.substr(kPrefix.size(), item.size() - kPrefix.size() - kSuffix.size());
    for (usize i = 0; i < kColours.size(); ++i) {
        if (kColours[i] == colour) {
            return static_cast<i8>(i);
        }
    }
    return std::nullopt;
}

std::optional<i8> mixed_colour(i8 a, i8 b) noexcept {
    for (const Mix& mix : kMixes) {
        if ((mix.a == a && mix.b == b) || (mix.a == b && mix.b == a)) {
            return mix.result;
        }
    }
    return std::nullopt;
}

i8 offspring_colour(i8 a, i8 b, math::LegacyRandomSource& random) noexcept {
    if (const auto mixed = mixed_colour(a, b)) {
        return *mixed;
    }
    // One of the two, evenly: measured 113 to the first parent and 109 to the
    // second over 222 pairs of different colours. (Same-colour pairs, and the
    // eighteen mixing ones, are the other 34 of the 256.)
    return random.next_boolean() ? a : b;
}

void baby_box(entity::EntityState& state, f32 adult_width, f32 adult_height,
              f32 adult_eye) noexcept {
    state.width      = adult_width * kBabyScale;
    state.height     = adult_height * kBabyScale;
    state.eye_height = adult_eye * kBabyScale;
}

MobBrain* mob_brain_of(entity::EntityWorld& world, entity::EntityHandle handle) noexcept {
    auto* mob = dynamic_cast<Mob*>(world.logic(handle));
    return mob == nullptr ? nullptr : &mob->mutable_brain();
}

// ── Mob: the husbandry half of its tick ─────────────────────────────────────

void Mob::make_baby(entity::EntityState& state) noexcept { set_age(state, kBabyAge); }

void Mob::set_age(entity::EntityState& state, i32 age) noexcept {
    if (adult_eye_ < 0.0F) {
        // The first time the mob is resized its state is still the adult the
        // registry built; remember that before halving it.
        adult_eye_ = state.eye_height;
    }
    brain_.animal.age = age;
    baby_box_         = age < 0;
    if (age < 0) {
        baby_box(state, adult_width_, adult_height_, adult_eye_);
        if (const AnimalKind* kind = animal_kind(kind_->type_name);
            kind != nullptr && kind->baby_eye_height > 0.0F) {
            state.eye_height = kind->baby_eye_height;
        }
    } else {
        state.width      = adult_width_;
        state.height     = adult_height_;
        state.eye_height = adult_eye_;
    }
    brain_.size = MobSize::from_box(state.width, state.height);
}

void Mob::tick_husbandry(entity::EntityState& state, entity::EntityHandle self,
                         const MobContext& context) {
    AnimalState& animal = brain_.animal;
    if (animal.age < 0) {
        // One a tick, measured: eight calves, four of them NoAI, all 1:1.
        ++animal.age;
    } else if (animal.age > 0) {
        --animal.age;
    }
    // Grown by the clock, by food or by grass: the box follows whichever.
    if (baby_box_ && animal.age >= 0) {
        set_age(state, animal.age);
        if (context.animal_events != nullptr) {
            AnimalEvent grew;
            grew.kind = AnimalEventKind::GrewUp;
            grew.self = self;
            grew.at   = state.position;
            context.animal_events->push_back(grew);
        }
    }
    if (animal.love > 0) {
        // 600 when fed, 591 nine ticks later: one a tick.
        --animal.love;
    }

    const AnimalKind* kind = animal_kind(kind_->type_name);
    if (kind != nullptr && kind->lays_eggs && !animal.baby()) {
        // Measured: a chick summoned with EggLayTime 20 still read 20 sixty
        // ticks later and laid nothing; an adult laid one egg and drew a new
        // time in [6000, 12000).
        --animal.egg_time;
        if (animal.egg_time <= 0) {
            if (context.animal_events != nullptr) {
                AnimalEvent egg;
                egg.kind = AnimalEventKind::LaidEgg;
                egg.self = self;
                egg.at   = state.position;
                context.animal_events->push_back(egg);
            }
            animal.egg_time = egg_interval(random_);
        }
    }
}

// ── TemptGoal ───────────────────────────────────────────────────────────────

const Tempter* TemptGoal::find(GoalContext& context) const {
    const entity::EntityState* self = context.state();
    if (self == nullptr) {
        return nullptr;
    }
    const Tempter* best          = nullptr;
    f64            best_distance = kRange * kRange;
    for (const Tempter& tempter : context.tempters) {
        if (!is_food(*kind_, tempter.main_hand) && !is_food(*kind_, tempter.off_hand)) {
            continue;
        }
        const f64 d = distance_sq(self->position, tempter.position);
        if (d <= best_distance) {
            best          = &tempter;
            best_distance = d;
        }
    }
    return best;
}

bool TemptGoal::can_use(GoalContext& context) {
    if (calm_down_ > 0) {
        --calm_down_;
        return false;
    }
    const Tempter* tempter = find(context);
    if (tempter == nullptr) {
        return false;
    }
    player_ = tempter->position;
    return true;
}

bool TemptGoal::can_continue_to_use(GoalContext& context) {
    const Tempter* tempter = find(context);
    if (tempter == nullptr) {
        return false;
    }
    player_ = tempter->position;
    return true;
}

void TemptGoal::start(GoalContext&) {}

void TemptGoal::stop(GoalContext& context) {
    calm_down_ = kCalmDown;
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
}

void TemptGoal::tick(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.brain == nullptr) {
        return;
    }
    context.brain->look_at  = Vec3d{player_.x, player_.y + 1.62, player_.z};
    context.brain->has_look = true;
    if (distance_sq(self->position, player_) < kStopDistance * kStopDistance) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
        return;
    }
    if (move_to(context, block_of(player_), speed_, 16.0F)) {
        context.brain->wants_move = true;
        context.brain->speed      = speed_;
    }
}

// ── EatGrassGoal ────────────────────────────────────────────────────────────

namespace {

/// What a sheep standing here would eat: the tuft at its feet first, else the
/// grass block below. Nullopt when there is neither.
[[nodiscard]] std::optional<std::pair<BlockPos, bool>> grazing(const world::LevelView& level,
                                                               BlockPos feet) {
    const registry::BlockRegistry& blocks = level.blocks();
    // `minecraft:grass` is the short tuft in 1.20.1 (renamed short_grass later).
    if (blocks.block_name(blocks.block_of(level.block_at(feet))) == "minecraft:grass") {
        return std::pair{feet, false};
    }
    const BlockPos below = feet.below();
    if (blocks.block_name(blocks.block_of(level.block_at(below))) == "minecraft:grass_block") {
        return std::pair{below, true};
    }
    return std::nullopt;
}

}  // namespace

bool EatGrassGoal::can_use(GoalContext& context) {
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.brain == nullptr || context.random == nullptr ||
        context.level == nullptr) {
        return false;
    }
    // Measured on 40 penned adults: 28 ate within 2575 ticks, a maximum
    // likelihood of one in 2060 per tick — not the one in 1000 the wiki gives.
    // The game's goal selector offers a goal a start only every other tick;
    // drawing one in 1000 on even ticks only is one in 2000 per tick, and that
    // is what this does. Ten lambs all ate within 307 ticks: one in 50 every
    // other tick is 0.95 by then.
    if ((context.tick & 1) != 0) {
        return false;
    }
    const i32 odds = context.brain->animal.baby() ? kLambOdds : kAdultOdds;
    if (context.random->next_int(odds) != 0) {
        return false;
    }
    return grazing(*context.level, block_of(self->position)).has_value();
}

bool EatGrassGoal::can_continue_to_use(GoalContext&) { return remaining_ > 0; }

void EatGrassGoal::start(GoalContext& context) {
    remaining_ = kDuration;
    if (context.brain != nullptr) {
        context.brain->follower.clear();
        context.brain->wants_move = false;
    }
    if (context.animal_events != nullptr) {
        AnimalEvent event;
        event.kind = AnimalEventKind::GrazeStart;
        event.self = context.self;
        context.animal_events->push_back(event);
    }
}

void EatGrassGoal::stop(GoalContext&) { remaining_ = 0; }

void EatGrassGoal::tick(GoalContext& context) {
    if (remaining_ <= 0) {
        return;
    }
    --remaining_;
    if (remaining_ != 4) {
        return;
    }
    const entity::EntityState* self = context.state();
    if (self == nullptr || context.level == nullptr || context.brain == nullptr) {
        return;
    }
    const auto eaten = grazing(*context.level, block_of(self->position));
    if (!eaten) {
        return;
    }
    AnimalState& animal = context.brain->animal;
    animal.sheared      = false;
    // Measured: ten lambs over 2575 ticks read exactly 1200, 2400 or 3600
    // ticks older than their clock alone — one minute per mouthful.
    age_up(animal, 1200);
    if (context.animal_events != nullptr) {
        AnimalEvent event;
        event.kind        = AnimalEventKind::AteGrass;
        event.self        = context.self;
        event.at          = self->position;
        event.block       = eaten->first;
        event.grass_block = eaten->second;
        context.animal_events->push_back(event);
    }
}

}  // namespace ov::gameplay
