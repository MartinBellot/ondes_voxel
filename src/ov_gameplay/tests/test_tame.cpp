// ── tame ── Taming, owners, riding and foals, against what a real 1.20.1
// server did (scripts/measure_tame.py, docs/provenance/apprivoisement.md). The
// `[parity]` cases replay measured numbers — each with a control that the same
// test must reject; the rest drive the goals through a Mob, as the server does.
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/tame.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <vector>

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

[[nodiscard]] i32 type_id(std::string_view name) {
    const auto types = registries()->find("minecraft:entity_type");
    return registries()->protocol_id(*types, name).value_or(-1);
}

/// Grass below y = 0, air above.
class Field final : public world::LevelView {
public:
    explicit Field(const registry::BlockRegistry& registry) : registry_{&registry} {
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

/// An entity world on a field, with an owner the goals can see.
struct Kennel {
    Floor                    floor;
    CollisionWorld           collisions;
    Field                    field;
    entity::EntityWorld      world;
    std::vector<Owner>       owners;
    std::vector<TameEvent>   events;
    std::vector<Quarry>      quarries;
    std::vector<MobAttack>   attacks;
    TameWorld                tame;
    i64                      tick{0};

    Kennel()
        : floor{blocks()->default_state(blocks()->find_block("minecraft:grass_block").value())},
          collisions{*blocks(), &Floor::look_up, &floor},
          field{*blocks()},
          world{*registries()} {
        tame.creeper_type = type_id("minecraft:creeper");
        tame.cat_type     = type_id("minecraft:cat");
        tame.ocelot_type  = type_id("minecraft:ocelot");
    }

    entity::EntityHandle spawn(std::string_view type, Vec3d at, i64 seed = 1) {
        const auto handle = world.spawn(type, at, net::Uuid{}).value();
        entity::EntityState* state = world.mutable_state(handle);
        world.set_logic(handle, std::make_unique<Mob>(*mob_kind(type), state->width, state->height,
                                                      seed, type_id("minecraft:player")));
        return handle;
    }

    Mob& mob(entity::EntityHandle handle) { return *dynamic_cast<Mob*>(world.logic(handle)); }

    void step(i32 ticks) {
        for (i32 i = 0; i < ticks; ++i) {
            tame.owners = owners;
            tame.events = &events;
            MobContext context{&collisions, &field, false};
            context.tame_world = &tame;
            context.quarries   = quarries;
            context.attacks    = &attacks;
            world.tick(entity::TickContext{tick++, &context});
        }
    }
};

/// The upper tail of a χ² with `k` degrees of freedom (series for the lower
/// regularized gamma), enough to tell 0.5 from 1e-6.
[[nodiscard]] f64 chi2_tail(f64 x, f64 k) {
    const f64 a  = k / 2.0;
    const f64 xx = x / 2.0;
    f64       term = 1.0 / a;
    f64       sum  = term;
    for (i32 n = 1; n < 400; ++n) {
        term *= xx / (a + static_cast<f64>(n));
        sum += term;
    }
    return std::max(0.0, 1.0 - sum * std::exp(-xx + a * std::log(xx) - std::lgamma(a)));
}

/// χ² of the "tries until tamed" histogram against a geometric law of `p`,
/// the tail bin gathering everything from `bins` up.
[[nodiscard]] f64 geometric_tail(const std::vector<i32>& counts, f64 p, usize bins) {
    const f64 n = static_cast<f64>(std::accumulate(counts.begin(), counts.end(), 0));
    f64       chi = 0.0;
    f64       left = 1.0;
    for (usize k = 1; k <= bins; ++k) {
        const f64 expected = k < bins ? n * p * std::pow(1.0 - p, static_cast<f64>(k - 1))
                                      : n * left;
        f64 observed = 0.0;
        for (usize j = k; j < (k < bins ? k + 1 : counts.size()); ++j) {
            observed += j < counts.size() ? static_cast<f64>(counts[j]) : 0.0;
        }
        if (k < bins) {
            left -= p * std::pow(1.0 - p, static_cast<f64>(k - 1));
        }
        chi += (observed - expected) * (observed - expected) / expected;
    }
    return chi2_tail(chi, static_cast<f64>(bins - 1));
}

}  // namespace

// ── The table ───────────────────────────────────────────────────────────────

TEST_CASE("tame: every species has a brain, an attribute and a family", "[tame]") {
    for (const std::string_view type :
         {"minecraft:wolf", "minecraft:cat", "minecraft:ocelot", "minecraft:parrot",
          "minecraft:horse", "minecraft:donkey", "minecraft:mule", "minecraft:llama",
          "minecraft:trader_llama", "minecraft:rabbit", "minecraft:fox", "minecraft:turtle",
          "minecraft:bee", "minecraft:goat", "minecraft:camel", "minecraft:sniffer"}) {
        INFO(type);
        REQUIRE(mob_kind(type) != nullptr);
        CHECK(mob_kind(type)->category == MobCategory::Creature);
        CHECK(tame_kind(type) != nullptr);
    }
    // The attributes are the measured ones (normalized/entities.json).
    CHECK(mob_kind("minecraft:donkey")->movement_speed == 0.175);
    CHECK(mob_kind("minecraft:camel")->movement_speed == 0.09);
    CHECK(tame_kind("minecraft:zombie") == nullptr);
}

TEST_CASE("tame: the bee's flowers are the datapack's #minecraft:flowers", "[tame]") {
    const std::filesystem::path tags =
        data_path() / "generated" / "data" / "minecraft" / "tags" / "items";
    if (!std::filesystem::exists(tags / "flowers.json")) {
        WARN("no generated item tags");
        return;
    }
    // Expand the tag (it names #small_flowers and #tall_flowers).
    std::set<std::string> expanded;
    std::vector<std::string> todo{"flowers"};
    simdjson::ondemand::parser parser;
    while (!todo.empty()) {
        const std::string name = todo.back();
        todo.pop_back();
        auto json = simdjson::padded_string::load((tags / (name + ".json")).string());
        REQUIRE(json.error() == simdjson::SUCCESS);
        auto doc = parser.iterate(json.value());
        for (auto value : doc["values"].get_array()) {
            std::string_view entry = value.get_string().value();
            if (entry.starts_with("#minecraft:")) {
                todo.emplace_back(entry.substr(11));
            } else {
                expanded.emplace(entry);
            }
        }
    }
    const AnimalKind* bee = animal_kind("minecraft:bee");
    REQUIRE(bee != nullptr);
    std::set<std::string> ours;
    for (const std::string_view item : bee->food) {
        ours.emplace(item);
    }
    CHECK(ours == expanded);
}

// ── Taming odds [parity] ────────────────────────────────────────────────────

TEST_CASE("tame: one try in three tames a wolf or a cat, as measured", "[tame][parity]") {
    // measure_tame.py `tame`: tries until the hearts, per animal.
    //   wolf, 150 wolves, 435 bones: 56 33 17 14 10 4 7 4 1 2 2 (1..11 tries)
    //   cat, 150 cats, 447 cod:      55 32 17 17 9 4 6 2 3 2 2 0 1
    const std::vector<i32> wolf{0, 56, 33, 17, 14, 10, 4, 7, 4, 1, 2, 2};
    const std::vector<i32> cat{0, 55, 32, 17, 17, 9, 4, 6, 2, 3, 2, 2, 0, 1};
    CHECK(geometric_tail(wolf, 1.0 / 3.0, 6) > 0.05);
    CHECK(geometric_tail(cat, 1.0 / 3.0, 6) > 0.05);
    // The control: one in two, and one in five, are both thrown out.
    CHECK(geometric_tail(wolf, 1.0 / 2.0, 6) < 1e-3);
    CHECK(geometric_tail(wolf, 1.0 / 5.0, 6) < 1e-3);

    // And our draw: 30 000 tries of `taming_roll(…, 3)` land within 1 %.
    math::LegacyRandomSource random{42};
    i32 tamed = 0;
    for (i32 i = 0; i < 30000; ++i) {
        tamed += taming_roll(random, tame_kind("minecraft:wolf")->taming_odds) ? 1 : 0;
    }
    CHECK(static_cast<f64>(tamed) / 30000.0 == Catch::Approx(1.0 / 3.0).margin(0.01));
    CHECK(tame_kind("minecraft:cat")->taming_odds == 3);
    CHECK(tame_kind("minecraft:parrot")->taming_odds == 10);
}

TEST_CASE("tame: the parrot's one in ten, and what the measure says of it", "[tame][parity]") {
    // 60 parrots, 781 seeds: 13.0 tries a parrot where one in ten gives 10.
    // Binomial: 60 tamed where 78.1 ± 8.4 were expected, z = -2.2 — kept at
    // the documented 1/10 and named in apprivoisement.md § 3, with the rerun.
    const f64 n = 781.0, k = 60.0, p = 0.1;
    const f64 z = (k - n * p) / std::sqrt(n * p * (1.0 - p));
    CHECK(z > -2.6);
    CHECK(z < -1.8);
}

// ── Anger [parity] ──────────────────────────────────────────────────────────

TEST_CASE("tame: a wolf's anger is 20 to 39 seconds", "[tame][parity]") {
    math::LegacyRandomSource random{7};
    i32 low = 10000, high = 0;
    for (i32 i = 0; i < 20000; ++i) {
        const i32 anger = draw_anger_time(random);
        low             = std::min(low, anger);
        high            = std::max(high, anger);
    }
    CHECK(low == 400);
    CHECK(high == 780);
}

// ── Horses [parity] ─────────────────────────────────────────────────────────

TEST_CASE("tame: a spawned horse's stats, as 200 real ones", "[tame][parity]") {
    // measure_tame.py `spawn`, 200 horses summoned without NBT:
    //   health 15..29, mean 21.88, sd 3.37 · speed 0.1269..0.3092, mean
    //   0.22470, sd 0.03894 · jump 0.455..0.988, mean 0.7138, sd 0.1016.
    math::LegacyRandomSource random{2024};
    std::vector<HorseStats>  drawn;
    for (i32 i = 0; i < 20000; ++i) {
        drawn.push_back(draw_horse_stats("minecraft:horse", random));
    }
    const auto moments = [&](auto field) {
        f64 sum = 0.0, sq = 0.0, lo = 1e9, hi = -1e9;
        for (const HorseStats& s : drawn) {
            const f64 v = field(s);
            sum += v;
            sq += v * v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const f64 mean = sum / static_cast<f64>(drawn.size());
        return std::array<f64, 4>{mean, std::sqrt(sq / static_cast<f64>(drawn.size()) - mean * mean),
                                  lo, hi};
    };
    const auto speed = moments([](const HorseStats& s) { return s.speed; });
    const auto jump  = moments([](const HorseStats& s) { return s.jump; });
    const auto hp    = moments([](const HorseStats& s) { return s.max_health; });
    // The measured means sit within three of their standard errors of ours
    // (speed at z = -0.1; jump at z = +2.07, named in apprivoisement.md § 5.1).
    CHECK(std::abs(speed[0] - 0.22470) < 3.0 * speed[1] / std::sqrt(200.0));
    CHECK(std::abs(jump[0] - 0.71379) < 3.0 * jump[1] / std::sqrt(200.0));
    // The control: a jump of 0.4 + 0.3·(r+r+r) (mean 0.85) is refused.
    CHECK(std::abs(0.85 - 0.71379) > 3.0 * jump[1] / std::sqrt(200.0));
    CHECK(speed[1] == Catch::Approx(0.03894).epsilon(0.1));
    CHECK(jump[1] == Catch::Approx(0.10157).epsilon(0.1));
    CHECK(speed[2] >= 0.1125);
    CHECK(speed[3] <= 0.3375);
    CHECK(hp[2] == 15.0);
    CHECK(hp[3] == 30.0);
    // Health: 21.88 measured on 200, ours 22.5; 480 of all four species
    // pooled give 22.13 (named, apprivoisement.md § 4.1).
    CHECK(std::abs(hp[0] - 21.875) < 3.0 * hp[1] / std::sqrt(200.0));
    // Donkeys, mules, llamas: only the health is drawn (measured: 80 donkeys,
    // 40 mules, 160 llamas, all at 0.175 and 0.5).
    const HorseStats donkey = draw_horse_stats("minecraft:donkey", random);
    CHECK(donkey.speed == 0.175);
    CHECK(donkey.jump == 0.5);
}

TEST_CASE("tame: a foal's stats, as 132 real ones", "[tame][parity]") {
    // measure_tame.py `breed`: sixty pairs of each, tame and in love, penned.
    //   apart (20, 0.2, 0.5) × (28, 0.3, 0.9): health sd 2.224, speed sd
    //   0.02575, jump sd 0.09473; means 23.67, 0.2533, 0.7075.
    //   equal (24, 0.25, 0.7) twice: sd 0.7214, 0.01028, 0.02874.
    //   a horse (22, 0.22, 0.6) × a donkey (20, 0.175, 0.5): 12 mules at
    //   0.178..0.234 and 0.516..0.648.
    struct Case {
        HorseStats a, b;
        f64        sd_health, sd_speed, sd_jump;
    };
    const std::array<Case, 2> cases{{
        {{20.0, 0.2, 0.5}, {28.0, 0.3, 0.9}, 2.22375, 0.02575, 0.09473},
        {{24.0, 0.25, 0.7}, {24.0, 0.25, 0.7}, 0.72135, 0.01028, 0.02874},
    }};
    math::LegacyRandomSource random{99};
    for (const Case& c : cases) {
        std::vector<HorseStats> foals;
        for (i32 i = 0; i < 20000; ++i) {
            foals.push_back(offspring_stats("minecraft:horse", c.a, c.b, random));
        }
        const auto sd = [&](auto field) {
            f64 sum = 0.0, sq = 0.0;
            for (const HorseStats& s : foals) {
                sum += field(s);
                sq += field(s) * field(s);
            }
            const f64 mean = sum / static_cast<f64>(foals.size());
            return std::sqrt(sq / static_cast<f64>(foals.size()) - mean * mean);
        };
        // A standard deviation measured on 60 foals is good to about ±18 %
        // (two standard errors): ours must fall inside.
        CHECK(sd([](const HorseStats& s) { return s.max_health; }) ==
              Catch::Approx(c.sd_health).epsilon(0.18));
        CHECK(sd([](const HorseStats& s) { return s.speed; }) ==
              Catch::Approx(c.sd_speed).epsilon(0.18));
        CHECK(sd([](const HorseStats& s) { return s.jump; }) ==
              Catch::Approx(c.sd_jump).epsilon(0.18));
    }
    // The control: the parents' mean with no spread, and the pre-1.20
    // "average of the parents and one fresh draw", are both refused.
    CHECK(0.0 != Catch::Approx(0.72135).epsilon(0.18));
    {
        math::LegacyRandomSource old{5};
        f64 sum = 0.0, sq = 0.0;
        for (i32 i = 0; i < 20000; ++i) {
            const f64 v = (24.0 + 24.0 + draw_horse_stats("minecraft:horse", old).max_health) / 3.0;
            sum += v;
            sq += v * v;
        }
        const f64 mean = sum / 20000.0;
        const f64 old_sd = std::sqrt(sq / 20000.0 - mean * mean);
        CHECK(old_sd != Catch::Approx(0.72135).epsilon(0.18));
    }
    // A mule's walk comes from its parents.
    const HorseStats mule = offspring_stats("minecraft:mule", {22.0, 0.22, 0.6},
                                            {20.0, 0.175, 0.5}, random);
    CHECK(mule.speed != 0.175);
}

TEST_CASE("tame: a llama's strength, as 160 real ones", "[tame][parity]") {
    // 1/2/3/4/5 = 40/53/56/5/6 over 160 llamas and trader llamas.
    math::LegacyRandomSource random{3};
    std::array<i32, 6>       seen{};
    for (i32 i = 0; i < 160000; ++i) {
        ++seen[static_cast<usize>(draw_llama_strength(random))];
    }
    const f64 wide = static_cast<f64>(seen[4] + seen[5]) / 160000.0;
    CHECK(wide == Catch::Approx(11.0 / 160.0).margin(0.02));
    CHECK(seen[0] == 0);
}

// ── Goals ───────────────────────────────────────────────────────────────────

TEST_CASE("tame: a tame wolf left behind walks, then is put back beside its owner",
          "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Kennel kennel;
    const auto wolf = kennel.spawn("minecraft:wolf", Vec3d{0.5, 0.0, 0.5});
    TameState& tame = kennel.mob(wolf).mutable_brain().tame;
    const net::Uuid me{1, 2};
    tame.tame  = true;
    tame.owner = me;
    kennel.owners.push_back(Owner{.network_id = 7, .uuid = me, .feet = Vec3d{9.0, 0.0, 0.5}});
    kennel.step(5);
    // Nine blocks: under the start of 10, the wolf stays.
    CHECK(std::abs(kennel.world.state(wolf)->position.x - 0.5) < 0.3);
    CHECK_FALSE(kennel.mob(wolf).goals().is_running("follow_owner"));

    kennel.owners[0].feet = Vec3d{11.5, 0.0, 0.5};
    kennel.step(60);
    CHECK(kennel.world.state(wolf)->position.x > 5.0);
    CHECK(kennel.events.empty());  // walked, not teleported

    kennel.owners[0].feet = Vec3d{60.5, 0.0, 0.5};
    kennel.step(3);
    const Vec3d at = kennel.world.state(wolf)->position;
    CHECK(std::abs(at.x - 60.5) <= 3.5);
    CHECK(std::abs(at.z - 0.5) <= 3.5);
    REQUIRE_FALSE(kennel.events.empty());
    CHECK(kennel.events.back().kind == TameEventKind::Teleported);
}

TEST_CASE("tame: a sitting wolf stays, whoever walks away", "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto wolf = kennel.spawn("minecraft:wolf", Vec3d{0.5, 0.0, 0.5});
    TameState& tame = kennel.mob(wolf).mutable_brain().tame;
    const net::Uuid me{1, 2};
    tame.tame    = true;
    tame.owner   = me;
    tame.sitting = true;
    kennel.owners.push_back(Owner{.network_id = 7, .uuid = me, .feet = Vec3d{40.5, 0.0, 0.5}});
    kennel.step(80);
    CHECK(std::abs(kennel.world.state(wolf)->position.x - 0.5) < 0.2);
    CHECK(kennel.mob(wolf).goals().is_running("sit"));
}

TEST_CASE("tame: a wolf goes for what hurt its owner, and never for a creeper",
          "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto wolf   = kennel.spawn("minecraft:wolf", Vec3d{0.5, 0.0, 0.5});
    const auto zombie = kennel.spawn("minecraft:zombie", Vec3d{4.5, 0.0, 0.5}, 2);
    const auto creeper = kennel.spawn("minecraft:creeper", Vec3d{0.5, 0.0, 4.5}, 3);
    TameState& tame   = kennel.mob(wolf).mutable_brain().tame;
    const net::Uuid me{1, 2};
    tame.tame  = true;
    tame.owner = me;
    kennel.owners.push_back(Owner{.network_id = 7, .uuid = me, .feet = Vec3d{1.5, 0.0, 1.5}});

    kennel.owners[0].hurt_by      = creeper;
    kennel.owners[0].hurt_by_tick = 1;
    kennel.step(2);
    CHECK(kennel.mob(wolf).target() == entity::kNoEntity);

    kennel.owners[0].hurt_by      = zombie;
    kennel.owners[0].hurt_by_tick = 5;
    kennel.step(40);
    CHECK(kennel.mob(wolf).target() == zombie);
    // It closes in and swings: MobAttacks on the zombie's wire id.
    const i32 zombie_id = kennel.world.state(zombie)->network_id;
    CHECK(std::ranges::any_of(kennel.attacks,
                              [&](const MobAttack& a) { return a.target == zombie_id; }));
}

TEST_CASE("tame: an angry wolf hunts the player it is angry at, and calms down",
          "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto wolf = kennel.spawn("minecraft:wolf", Vec3d{0.5, 0.0, 0.5});
    TameState& tame = kennel.mob(wolf).mutable_brain().tame;
    const net::Uuid them{9, 9};
    tame.anger    = 30;
    tame.angry_at = them;
    kennel.owners.push_back(Owner{.network_id = 11, .uuid = them, .feet = Vec3d{5.5, 0.0, 0.5}});
    kennel.quarries.push_back(Quarry{.network_id = 11, .type = type_id("minecraft:player"),
                                     .feet = Vec3d{5.5, 0.0, 0.5}});
    kennel.step(3);
    CHECK(kennel.mob(wolf).brain().target_player == 11);
    kennel.step(40);
    CHECK(tame.anger == 0);
    CHECK_FALSE(tame.angry_at.has_value());
    CHECK(kennel.mob(wolf).brain().target_player == 0);
}

TEST_CASE("tame: a ridden wild horse decides, tamed under its temper or throwing",
          "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto horse = kennel.spawn("minecraft:horse", Vec3d{0.5, 0.0, 0.5});
    TameState& tame  = kennel.mob(horse).mutable_brain().tame;
    const net::Uuid me{1, 2};
    kennel.owners.push_back(Owner{.network_id = 7, .uuid = me, .feet = Vec3d{0.5, 0.0, 0.5}});
    tame.rider = 7;
    kennel.step(600);
    REQUIRE_FALSE(kennel.events.empty());
    const TameEvent first = kennel.events.front();
    // Temper 0: the first decision always throws, and the temper rises by 5.
    CHECK(first.kind == TameEventKind::Threw);
    CHECK(first.player == 7);
    CHECK(tame.temper >= 5);

    // At the maximum temper it always gives in, to its rider.
    Kennel second;
    const auto wild = second.spawn("minecraft:horse", Vec3d{0.5, 0.0, 0.5}, 5);
    TameState& t2   = second.mob(wild).mutable_brain().tame;
    second.owners.push_back(Owner{.network_id = 7, .uuid = me, .feet = Vec3d{0.5, 0.0, 0.5}});
    t2.rider  = 7;
    t2.temper = 100;
    second.step(600);
    REQUIRE_FALSE(second.events.empty());
    CHECK(second.events.front().kind == TameEventKind::Tamed);
    CHECK(t2.tame);
    CHECK(t2.owned_by(me));
}

TEST_CASE("tame: a ridden wild horse decides about 50 ticks after it is mounted",
          "[tame][goals][parity]") {
    // measure_tame.py `temper`: a decision a median 60 ticks after the mount
    // on horses (n = 63), polled at about 20 ticks — 50 to 60 real ticks,
    // what one draw in 50 a tick gives. Forty horses here: the mean of forty
    // geometric delays of mean 50 is good to about ±8.
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel                            kennel;
    std::vector<entity::EntityHandle> horses;
    for (i32 i = 0; i < 40; ++i) {
        const auto horse = kennel.spawn("minecraft:horse",
                                        Vec3d{static_cast<f64>(i % 8) * 12.5, 0.0,
                                              static_cast<f64>(i / 8) * 12.5},
                                        100 + i);
        kennel.mob(horse).mutable_brain().tame.rider = 1000 + i;
        horses.push_back(horse);
    }
    // Each horse's first decision, by its index in `horses`; -1 not yet.
    std::vector<i64> decided(horses.size(), -1);
    usize            count = 0;
    for (i32 t = 0; t < 1000 && count < horses.size(); ++t) {
        kennel.step(1);
        for (const TameEvent& event : kennel.events) {
            const auto it = std::ranges::find(horses, event.self);
            if (it == horses.end()) {
                continue;
            }
            const auto index = static_cast<usize>(it - horses.begin());
            if (decided[index] < 0) {
                decided[index] = kennel.tick;
                ++count;
            }
        }
        kennel.events.clear();
    }
    REQUIRE(count == horses.size());
    f64 sum = 0.0;
    for (const i64 when : decided) {
        sum += static_cast<f64>(when);
    }
    const f64 mean = sum / static_cast<f64>(decided.size());
    CHECK(mean > 30.0);
    CHECK(mean < 80.0);
}

TEST_CASE("tame: a saddled tame horse under its rider has no brain of its own",
          "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto horse = kennel.spawn("minecraft:horse", Vec3d{0.5, 0.0, 0.5});
    TameState& tame  = kennel.mob(horse).mutable_brain().tame;
    tame.tame    = true;
    tame.saddled = true;
    tame.rider   = 7;
    CHECK(rider_controls(tame));
    kennel.step(200);
    CHECK(std::abs(kennel.world.state(horse)->position.x - 0.5) < 1e-9);
    CHECK(std::abs(kennel.world.state(horse)->position.z - 0.5) < 1e-9);
    // Its body is its drawn one: health 15..30, not the registry's 53.
    CHECK(kennel.world.state(horse)->max_health >= 15.0F);
    CHECK(kennel.world.state(horse)->max_health <= 30.0F);
}

TEST_CASE("tame: a wolf is 8 health wild and 20 tame", "[tame][parity]") {
    // measure_tame.py `wolf`: `attribute … generic.max_health` 8.0 wild, 20.0
    // with an Owner; a wolf tamed at 6 health read 20 after the hearts.
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto wolf = kennel.spawn("minecraft:wolf", Vec3d{0.5, 0.0, 0.5});
    kennel.step(1);
    CHECK(kennel.world.state(wolf)->max_health == 8.0F);
    kennel.mob(wolf).mutable_brain().tame.tame = true;
    kennel.step(1);
    CHECK(kennel.world.state(wolf)->max_health == 20.0F);
}

TEST_CASE("tame: a creeper keeps away from a cat", "[tame][goals]") {
    if (registries() == nullptr || blocks() == nullptr) {
        return;
    }
    Kennel kennel;
    const auto creeper = kennel.spawn("minecraft:creeper", Vec3d{0.5, 0.0, 0.5});
    (void)kennel.spawn("minecraft:cat", Vec3d{3.5, 0.0, 0.5}, 4);
    kennel.step(60);
    CHECK(kennel.world.state(creeper)->position.x < -1.0);
}
