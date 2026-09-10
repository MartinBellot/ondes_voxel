// Husbandry against what a real 1.20.1 server did (scripts/measure_husbandry.py,
// docs/provenance/elevage.md). The `[parity]` cases replay measured numbers;
// the rest drive the rules through a Mob in an entity world, as the server does.
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <simdjson.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] std::filesystem::path data_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(data_path() / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(data_path() / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

/// Grass blocks below y = 0, air above.
class Pasture final : public world::LevelView {
public:
    explicit Pasture(const registry::BlockRegistry& registry) : registry_{&registry} {
        air_   = registry.default_state(registry.find_block("minecraft:air").value());
        grass_ = registry.default_state(registry.find_block("minecraft:grass_block").value());
    }
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        return pos.y < 0 ? grass_ : air_;
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         air_{};
    registry::BlockStateId         grass_{};
};

struct Floor {
    registry::BlockStateId grass{};
    static registry::BlockStateId look_up(void* context, i32, i32 y, i32) {
        return y < 0 ? static_cast<const Floor*>(context)->grass : registry::BlockStateId{0};
    }
};

/// An entity world with a pasture, and the plumbing a server would provide.
struct Farm {
    Floor                    floor;
    CollisionWorld           collisions;
    Pasture                  pasture;
    entity::EntityWorld      world;
    std::vector<AnimalEvent> events;
    std::vector<Tempter>     tempters;
    i64                      tick{0};

    Farm()
        : floor{blocks()->default_state(blocks()->find_block("minecraft:grass_block").value())},
          collisions{*blocks(), &Floor::look_up, &floor},
          pasture{*blocks()},
          world{*registries()} {}

    entity::EntityHandle spawn(std::string_view type, Vec3d at) {
        const auto handle = world.spawn(type, at, net::Uuid{}).value();
        entity::EntityState* state = world.mutable_state(handle);
        world.set_logic(handle, std::make_unique<Mob>(*mob_kind(type), state->width, state->height,
                                                      state->network_id));
        return handle;
    }
    Mob& mob(entity::EntityHandle handle) { return *dynamic_cast<Mob*>(world.logic(handle)); }
    AnimalState& animal(entity::EntityHandle handle) { return mob(handle).mutable_brain().animal; }

    void step(i32 ticks = 1) {
        for (i32 i = 0; i < ticks; ++i) {
            MobContext context{&collisions, &pasture, false};
            context.tempters      = tempters;
            context.animal_events = &events;
            world.tick(entity::TickContext{tick++, &context});
        }
    }
    [[nodiscard]] usize count(AnimalEventKind kind) const {
        return static_cast<usize>(std::ranges::count_if(
            events, [kind](const AnimalEvent& e) { return e.kind == kind; }));
    }
};

}  // namespace

// ── Measured numbers ────────────────────────────────────────────────────────

TEST_CASE("a fed calf gains a tenth of what is left, in whole seconds", "[husbandry][parity]") {
    // (age at the feeding tick, ticks gained), from the `feed` campaign: the
    // age read before and after, minus the nine ticks the reads took.
    constexpr std::array<std::pair<i32, i32>, 9> kMeasured{{
        {-23994, 2380}, {-19994, 1980}, {-11994, 1180}, {-5994, 580}, {-1995, 180},
        {-395, 20}, {-194, 0}, {-94, 0}, {-13, 0},
    }};
    for (const auto& [age, gain] : kMeasured) {
        CAPTURE(age);
        CHECK(feeding_growth(age) == gain);
    }
    CHECK(feeding_growth(0) == 0);
}

TEST_CASE("feeding: love, growth, and the two refusals", "[husbandry][parity]") {
    const AnimalKind& cow = *animal_kind("minecraft:cow");

    AnimalState adult;
    CHECK(feed(adult, cow, "minecraft:wheat", 7) == FeedResult::Love);
    CHECK(adult.love == 600);
    CHECK(adult.love_cause == 7);
    // Already in love: measured, the wheat stays in the hand.
    CHECK(feed(adult, cow, "minecraft:wheat", 7) == FeedResult::Refused);

    AnimalState parent;
    parent.age = 3000;
    CHECK(feed(parent, cow, "minecraft:wheat", 7) == FeedResult::Refused);
    CHECK(parent.love == 0);

    AnimalState calf;
    calf.age = -24000;
    CHECK(feed(calf, cow, "minecraft:wheat", 7) == FeedResult::Grew);
    CHECK(calf.age == -24000 + 2400);
    CHECK(calf.love == 0);

    AnimalState near_grown;
    near_grown.age = -4;
    CHECK(feed(near_grown, cow, "minecraft:wheat", 7) == FeedResult::Grew);
    CHECK(near_grown.age == -4);  // a tenth of four ticks is no whole second

    CHECK(feed(adult, cow, "minecraft:carrot", 7) == FeedResult::NotFood);
}

TEST_CASE("the measured food lists, and nothing else", "[husbandry][parity]") {
    CHECK(is_food(*animal_kind("minecraft:cow"), "minecraft:wheat"));
    CHECK(is_food(*animal_kind("minecraft:sheep"), "minecraft:wheat"));
    CHECK_FALSE(is_food(*animal_kind("minecraft:cow"), "minecraft:hay_block"));
    for (const auto* item : {"minecraft:carrot", "minecraft:potato", "minecraft:beetroot"}) {
        CHECK(is_food(*animal_kind("minecraft:pig"), item));
    }
    CHECK_FALSE(is_food(*animal_kind("minecraft:pig"), "minecraft:golden_carrot"));
    for (const auto* item : {"minecraft:wheat_seeds", "minecraft:melon_seeds",
                             "minecraft:pumpkin_seeds", "minecraft:beetroot_seeds",
                             "minecraft:torchflower_seeds", "minecraft:pitcher_pod"}) {
        CHECK(is_food(*animal_kind("minecraft:chicken"), item));
    }
    CHECK_FALSE(is_food(*animal_kind("minecraft:chicken"), "minecraft:wheat"));
    CHECK(animal_kind("minecraft:zombie") == nullptr);
}

TEST_CASE("the draws stay inside the measured ranges", "[husbandry][parity]") {
    math::LegacyRandomSource random{1234};
    std::map<i32, i32> wool;
    std::map<i32, i32> xp;
    i32 egg_min = 1'000'000;
    i32 egg_max = 0;
    for (i32 i = 0; i < 3000; ++i) {
        ++wool[wool_count(random)];
        ++xp[breeding_xp(random)];
        const i32 egg = egg_interval(random);
        egg_min = std::min(egg_min, egg);
        egg_max = std::max(egg_max, egg);
    }
    CHECK(wool.size() == 3);
    CHECK(wool.begin()->first == 1);
    CHECK(wool.rbegin()->first == 3);
    CHECK(xp.size() == 7);
    CHECK(xp.begin()->first == 1);
    CHECK(xp.rbegin()->first == 7);
    CHECK(egg_min >= 6000);
    CHECK(egg_max <= 11999);
}

TEST_CASE("the walk law reproduces the seven measured speeds", "[husbandry][parity]") {
    // (attribute × modifier, blocks per tick measured on the real server)
    constexpr std::array<std::pair<f64, f64>, 7> kMeasured{{
        {0.23, 0.11419},        // zombie chasing (mobs.md)
        {0.20, 0.0863},         // cow strolling
        {0.20 * 1.25, 0.1347},  // cow tempted
        {0.23 * 1.1, 0.1380},   // sheep tempted
        {0.25 * 1.2, 0.1936},   // pig tempted
        {0.25 * 1.0, 0.1349},   // chicken tempted
        {0.20 * 1.25, 0.1348},  // calf tempted: a baby walks as its adult
    }};
    for (const auto& [speed, measured] : kMeasured) {
        CAPTURE(speed);
        CHECK(walk_blocks_per_tick(speed) == Catch::Approx(measured).epsilon(0.02));
    }
}

TEST_CASE("lamb colours: the nine dye mixes, else a parent", "[husbandry][parity]") {
    const auto colour = [](std::string_view name) {
        return dye_colour("minecraft:" + std::string{name} + "_dye").value();
    };
    CHECK(mixed_colour(colour("red"), colour("white")) == colour("pink"));
    CHECK(mixed_colour(colour("white"), colour("red")) == colour("pink"));
    CHECK(mixed_colour(colour("blue"), colour("red")) == colour("purple"));
    CHECK_FALSE(mixed_colour(colour("red"), colour("green")).has_value());
    CHECK_FALSE(mixed_colour(colour("white"), colour("white")).has_value());

    math::LegacyRandomSource random{99};
    i32 first = 0;
    for (i32 i = 0; i < 2000; ++i) {
        const i8 lamb = offspring_colour(colour("red"), colour("green"), random);
        REQUIRE((lamb == colour("red") || lamb == colour("green")));
        first += lamb == colour("red") ? 1 : 0;
    }
    CHECK(first > 900);
    CHECK(first < 1100);
}

TEST_CASE("the dye mixes are exactly the datapack's two-dye recipes", "[husbandry][parity]") {
    const auto recipes = data_path() / "generated" / "data" / "minecraft" / "recipes";
    if (!std::filesystem::exists(recipes)) {
        WARN("generated recipes missing — the table is not checked against them");
        return;
    }
    simdjson::ondemand::parser parser;
    usize                      found = 0;
    for (const auto& entry : std::filesystem::directory_iterator{recipes}) {
        auto json = simdjson::padded_string::load(entry.path().string());
        if (json.error()) {
            continue;
        }
        simdjson::dom::parser dom;
        simdjson::dom::element doc;
        if (dom.parse(json.value()).get(doc)) {
            continue;
        }
        std::string_view type;
        if (doc["type"].get(type) || type != "minecraft:crafting_shapeless") {
            continue;
        }
        simdjson::dom::array ingredients;
        std::string_view     result;
        if (doc["ingredients"].get(ingredients) || doc["result"]["item"].get(result)) {
            continue;
        }
        if (ingredients.size() != 2 || !dye_colour(result)) {
            continue;
        }
        std::array<std::optional<i8>, 2> dyes;
        usize                            k = 0;
        for (simdjson::dom::element ingredient : ingredients) {
            std::string_view item;
            if (!ingredient["item"].get(item)) {
                dyes[k] = dye_colour(item);
            }
            ++k;
        }
        if (!dyes[0] || !dyes[1]) {
            continue;
        }
        CAPTURE(entry.path().filename().string());
        CHECK(mixed_colour(*dyes[0], *dyes[1]) == dye_colour(result));
        ++found;
    }
    (void)parser;
    CHECK(found == 9);
}

// ── Through a Mob ───────────────────────────────────────────────────────────

TEST_CASE("two cows in love side by side have a calf sixty ticks later", "[husbandry]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Farm farm;
    const auto a = farm.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5});
    const auto b = farm.spawn("minecraft:cow", Vec3d{1.5, 0.0, 0.5});
    farm.animal(a).love = kLoveTicks;
    farm.animal(b).love = kLoveTicks;

    i32 born_at = -1;
    for (i32 t = 0; t < 120 && born_at < 0; ++t) {
        farm.step();
        if (farm.count(AnimalEventKind::Birth) > 0) {
            born_at = t + 1;
        }
    }
    // Measured: 59 to 60 ticks from the feeding to the calf.
    CHECK(born_at >= 59);
    CHECK(born_at <= 62);
    CHECK(farm.count(AnimalEventKind::Birth) == 1);
    // Set to 6000 by whichever parent bred; the other may already have
    // ticked once since, within the same tick. Vanilla read 5996..5998 three
    // ticks after, which does not separate the two.
    CHECK(farm.animal(a).age >= kParentCooldown - 1);
    CHECK(farm.animal(b).age >= kParentCooldown - 1);
    CHECK(farm.animal(a).love == 0);
    CHECK(farm.animal(b).love == 0);
}

TEST_CASE("a mate is found at 8.5 blocks and not at 9.5", "[husbandry][parity]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    // Measured (`reach`): pairs summoned 6..9 blocks apart walked together,
    // 9.5 and 10 did not.
    for (const auto& [gap, found] : {std::pair{8.5, true}, std::pair{9.5, false}}) {
        CAPTURE(gap);
        Farm farm;
        const auto a = farm.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5});
        const auto b = farm.spawn("minecraft:cow", Vec3d{0.5 + gap, 0.0, 0.5});
        farm.animal(a).love = kLoveTicks;
        farm.animal(b).love = kLoveTicks;
        farm.step(4);
        CHECK(farm.mob(a).goals().is_running("breed") == found);
    }
}

TEST_CASE("a calf is half its mother and grows back to her size", "[husbandry][parity]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Farm farm;
    const auto calf = farm.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5});
    entity::EntityState& state = *farm.world.mutable_state(calf);
    farm.mob(calf).make_baby(state);
    // Measured: 0.45 × 0.7, eyes at 0.665, against the adult's 0.9 × 1.4 / 1.3.
    CHECK(state.width == Catch::Approx(0.45F));
    CHECK(state.height == Catch::Approx(0.7F));
    CHECK(state.eye_height == Catch::Approx(0.665F));
    CHECK(farm.animal(calf).age == kBabyAge);

    farm.mob(calf).set_age(state, -3);
    farm.step(2);
    CHECK(farm.count(AnimalEventKind::GrewUp) == 0);
    farm.step(1);
    CHECK(farm.count(AnimalEventKind::GrewUp) == 1);
    CHECK(state.width == Catch::Approx(0.9F));
    CHECK(state.height == Catch::Approx(1.4F));

    // Grown by food between two ticks: the box still follows.
    const auto other = farm.spawn("minecraft:cow", Vec3d{4.5, 0.0, 4.5});
    entity::EntityState& other_state = *farm.world.mutable_state(other);
    farm.mob(other).set_age(other_state, -300);
    age_up(farm.animal(other), 1000);
    farm.step(1);
    CHECK(other_state.width == Catch::Approx(0.9F));
}

TEST_CASE("a hen lays when her timer runs out, a chick never", "[husbandry][parity]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Farm farm;
    const auto hen   = farm.spawn("minecraft:chicken", Vec3d{0.5, 0.0, 0.5});
    const auto chick = farm.spawn("minecraft:chicken", Vec3d{6.5, 0.0, 0.5});
    farm.mob(chick).make_baby(*farm.world.mutable_state(chick));
    CHECK(farm.animal(hen).egg_time >= 6000);
    farm.animal(hen).egg_time   = 3;
    farm.animal(chick).egg_time = 3;
    farm.step(5);
    CHECK(farm.count(AnimalEventKind::LaidEgg) == 1);
    CHECK(farm.animal(hen).egg_time >= 5990);
    CHECK(farm.animal(chick).egg_time == 3);
}

TEST_CASE("wheat in hand tempts a cow from ten blocks, not from eleven", "[husbandry][parity]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    for (const auto& [gap, tempted] : {std::pair{9.5, true}, std::pair{11.0, false}}) {
        CAPTURE(gap);
        Farm farm;
        const auto cow = farm.spawn("minecraft:cow", Vec3d{0.5 + gap, 0.0, 0.5});
        farm.tempters.push_back(Tempter{Vec3d{0.5, 0.0, 0.5}, "minecraft:wheat", {}});
        farm.step(3);
        CHECK(farm.mob(cow).goals().is_running("tempt") == tempted);
    }
    // And walks up to about 2.5 blocks, then stops.
    Farm farm;
    const auto cow = farm.spawn("minecraft:cow", Vec3d{8.5, 0.0, 0.5});
    farm.tempters.push_back(Tempter{Vec3d{0.5, 0.0, 0.5}, {}, "minecraft:wheat"});
    farm.step(200);
    const f64 left = std::abs(farm.world.state(cow)->position.x - 0.5);
    CHECK(left < 3.0);
    CHECK(left > 1.5);
}

TEST_CASE("a shorn sheep grazes, and its wool grows back", "[husbandry][parity]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Farm farm;
    const auto sheep = farm.spawn("minecraft:sheep", Vec3d{0.5, 0.0, 0.5});
    farm.animal(sheep).sheared = true;
    // One in 1000 on even ticks: about 2000 ticks on average; 30000 is a
    // certainty for any seed worth having.
    for (i32 i = 0; i < 30000 && farm.count(AnimalEventKind::AteGrass) == 0; ++i) {
        farm.step();
    }
    REQUIRE(farm.count(AnimalEventKind::AteGrass) == 1);
    CHECK(farm.count(AnimalEventKind::GrazeStart) >= 1);
    CHECK_FALSE(farm.animal(sheep).sheared);
    const auto eaten = std::ranges::find_if(farm.events, [](const AnimalEvent& e) {
        return e.kind == AnimalEventKind::AteGrass;
    });
    CHECK(eaten->grass_block);
    CHECK(eaten->block.y == -1);
}
