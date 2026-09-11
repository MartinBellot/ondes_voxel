// Brewing and potions, against what a real 1.20.1 server did.
//
// The tables are measured by scripts/measure_brewing.py and written down, with
// their campaigns, in docs/provenance/alchimie.md. The `[parity]` cases replay
// the raw campaign output (data/vanilla/1.20.1/normalized/brewing.json, local
// and never committed) entry by entry; when it is missing they say so and
// skip, rather than passing on an empty comparison. The other cases hold the
// numbers the campaigns established, so they run everywhere.
#include "ov/gameplay/brewing.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <simdjson.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

/// Health and nothing else, as a splash or a drink sees a target.
class Target final : public EffectTarget {
public:
    explicit Target(f32 health, Body body = Body::Ordinary) : health_{health}, body_{body} {}

    [[nodiscard]] Body body() const noexcept override { return body_; }
    [[nodiscard]] f32  health() const noexcept override { return health_; }
    [[nodiscard]] f32  max_health() const noexcept override { return 20.0F; }
    void heal(f32 amount) override { health_ = std::min(20.0F, health_ + amount); }
    void hurt(DamageKind /*kind*/, f32 amount, const DamageConstants& /*constants*/) override {
        health_ -= amount;
    }

private:
    f32  health_;
    Body body_;
};

[[nodiscard]] std::filesystem::path normalized(std::string_view file) {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "normalized" /
           std::string{file};
}

[[nodiscard]] Bottle bottle_of(std::string_view item, std::string_view potion) {
    return Bottle{*potion_form(item), *potion_from_name(potion)};
}

}  // namespace

TEST_CASE("the forty-three potions are the registry's, in its order", "[brewing]") {
    const auto path =
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
    auto registries = registry::Registries::load(path);
    if (!registries) {
        WARN("no registry.ovpack — python3 tools/ov_datagen/ovpack.py");
        return;
    }
    const auto potions = registries->find("minecraft:potion");
    REQUIRE(potions.has_value());
    const auto names = registries->entries(*potions);
    REQUIRE(names.size() == kPotionCount);
    for (usize i = 0; i < kPotionCount; ++i) {
        INFO(names[i]);
        CHECK(potion_info(static_cast<Potion>(i)).name == names[i]);
        CHECK(registries->protocol_id(*potions, names[i]) == static_cast<i32>(i));
    }
}

TEST_CASE("the base potions and their mixes", "[brewing]") {
    const Bottle water{PotionForm::Drink, Potion::Water};
    CHECK(brew(water, "minecraft:nether_wart") == Bottle{PotionForm::Drink, Potion::Awkward});
    CHECK(brew(water, "minecraft:glowstone_dust") == Bottle{PotionForm::Drink, Potion::Thick});
    CHECK(brew(water, "minecraft:redstone") == Bottle{PotionForm::Drink, Potion::Mundane});
    CHECK(brew(water, "minecraft:fermented_spider_eye") ==
          Bottle{PotionForm::Drink, Potion::Weakness});
    // A splash bottle stays a splash bottle through a potion mix.
    CHECK(brew({PotionForm::Splash, Potion::Awkward}, "minecraft:blaze_powder") ==
          Bottle{PotionForm::Splash, Potion::Strength});
    // Container mixes keep the potion, whatever it is.
    CHECK(brew({PotionForm::Drink, Potion::Empty}, "minecraft:gunpowder") ==
          Bottle{PotionForm::Splash, Potion::Empty});
    CHECK(brew({PotionForm::Splash, Potion::StrongHarming}, "minecraft:dragon_breath") ==
          Bottle{PotionForm::Lingering, Potion::StrongHarming});
    // …and only from the right bottle.
    CHECK_FALSE(brew({PotionForm::Drink, Potion::Water}, "minecraft:dragon_breath"));
    CHECK_FALSE(brew({PotionForm::Lingering, Potion::Water}, "minecraft:gunpowder"));
    CHECK_FALSE(brew({PotionForm::Drink, Potion::Awkward}, "minecraft:carrot"));
    CHECK_FALSE(is_brewing_ingredient("minecraft:carrot"));
    CHECK(is_brewing_ingredient("minecraft:turtle_helmet"));
    CHECK(brewing_remainder("minecraft:dragon_breath") == "minecraft:glass_bottle");
    CHECK(brewing_remainder("minecraft:nether_wart").empty());
}

TEST_CASE("a brewing stand's clock and fuel", "[brewing]") {
    StandView view;
    view.bottles    = {Bottle{PotionForm::Drink, Potion::Water}, std::nullopt, std::nullopt};
    view.occupied   = {true, false, false};
    view.ingredient = "minecraft:nether_wart";
    view.powder     = true;

    StandState state;
    // Out of fuel, powder in the slot: refuel and start in the same tick.
    StandTick first = brewing_stand_tick(view, state);
    CHECK(first.refuelled);
    CHECK(first.started);
    CHECK(state.fuel == kFuelPerPowder - 1);
    CHECK(state.brew_time == kBrewTicks);

    view.powder = false;
    i32 ticks   = 0;
    StandTick step;
    do {
        step = brewing_stand_tick(view, state);
        ++ticks;
    } while (!step.brewed && ticks < 1000);
    CHECK(ticks == kBrewTicks);
    REQUIRE(step.results[0].has_value());
    CHECK(step.results[0]->potion == Potion::Awkward);
    CHECK_FALSE(step.results[1].has_value());

    // Nothing left to brew: no fuel spent.
    view.bottles[0] = Bottle{PotionForm::Drink, Potion::Awkward};
    const StandTick idle = brewing_stand_tick(view, state);
    CHECK_FALSE(idle.started);
    CHECK(state.fuel == kFuelPerPowder - 1);

    // Taking the ingredient away mid-brew stops it; the fuel is not given back.
    view.bottles[0] = Bottle{PotionForm::Drink, Potion::Water};
    (void)brewing_stand_tick(view, state);
    CHECK(state.brew_time == kBrewTicks);
    CHECK(state.fuel == kFuelPerPowder - 2);
    view.ingredient = {};
    const StandTick stopped = brewing_stand_tick(view, state);
    CHECK(stopped.stopped);
    CHECK(state.brew_time == 0);
    CHECK(state.fuel == kFuelPerPowder - 2);
}

TEST_CASE("the splash law", "[brewing]") {
    CHECK(splash_factor(0.0, true) == 1.0);
    CHECK(splash_factor(4.0, false) == 0.5);
    CHECK(splash_factor(16.0, false) == 0.0);
    CHECK(splash_duration(1000, 0.75) == 750);
    CHECK(splash_duration(100, 0.225) == 23);
    CHECK(arrow_duration(900) == 112);
    CHECK(arrow_duration(5) == 1);
    CHECK(cloud_duration(9600) == 2400);

    // A splash of strong healing at half strength heals 4, not 8.
    ActiveEffects active;
    Target        target{2.0F};
    const std::array<PotionEffect, 1> heal{{{Effect::InstantHealth, 1, 1}}};
    splash_effects(heal, 0.5, active, target);
    CHECK_THAT(target.health(), Catch::Matchers::WithinAbs(6.0, 1e-6));

    // Below twenty ticks a splashed effect is dropped.
    const std::array<PotionEffect, 1> speed{{{Effect::Speed, 100, 0}}};
    splash_effects(speed, 0.2, active, target);
    CHECK_FALSE(active.has(Effect::Speed));
    splash_effects(speed, 0.25, active, target);
    CHECK(active.get(Effect::Speed)->duration == 25);
}

TEST_CASE("potion colours", "[brewing]") {
    CHECK(potion_color(Potion::Water) == kWaterColor);
    CHECK(potion_color(Potion::Awkward) == kWaterColor);
    // Poison alone is poison's own measured colour, as the arrow metadata
    // campaign of projectiles.md read it: 8889187.
    CHECK(potion_color(Potion::Poison) == 8889187U);
}

// ── Parity: the raw campaigns ───────────────────────────────────────────────

TEST_CASE("every bottle the real server brewed", "[brewing][parity]") {
    const auto path = normalized("brewing.json");
    if (!std::filesystem::exists(path)) {
        WARN("no brewing.json — run scripts/measure_brewing.py");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(path.string()).get(document) == simdjson::SUCCESS);
    simdjson::dom::array results;
    if (document["recipes"]["results"].get(results) != simdjson::SUCCESS) {
        WARN("brewing.json has no recipes campaign");
        return;
    }
    usize                    checked = 0;
    usize                    agreed  = 0;
    std::vector<std::string> wrong;
    for (simdjson::dom::element row : results) {
        const std::string_view container  = row["container"].get_string().value();
        const std::string_view potion     = row["potion"].get_string().value();
        const std::string_view ingredient = row["ingredient"].get_string().value();
        const Bottle           in         = bottle_of(container, potion);
        std::optional<Bottle>  expected;
        simdjson::dom::array   out;
        if (row["out"].get(out) == simdjson::SUCCESS) {
            const Bottle now = bottle_of(out.at(0).get_string().value(),
                                         out.at(1).get_string().value());
            if (!(now == in)) {
                expected = now;
            }
        }
        const std::optional<Bottle> ours = brew(in, ingredient);
        ++checked;
        if (ours == expected) {
            ++agreed;
        } else if (wrong.size() < 20) {
            wrong.push_back(std::string{container} + " " + std::string{potion} + " + " +
                            std::string{ingredient});
        }
    }
    for (const std::string& line : wrong) {
        UNSCOPED_INFO(line);
    }
    INFO(agreed << " / " << checked);
    CHECK(checked == 3 * kPotionCount * 21);
    CHECK(agreed == checked);
}

TEST_CASE("every potion the real server's bot drank", "[brewing][parity]") {
    const auto path = normalized("brewing.json");
    if (!std::filesystem::exists(path)) {
        WARN("no brewing.json — run scripts/measure_brewing.py");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(path.string()).get(document) == simdjson::SUCCESS);
    simdjson::dom::object drinks;
    if (document["drink"].get(drinks) != simdjson::SUCCESS) {
        WARN("brewing.json has no drink campaign");
        return;
    }
    usize checked = 0;
    for (usize i = 0; i < kPotionCount; ++i) {
        const PotionInfo&      info = potion_info(static_cast<Potion>(i));
        simdjson::dom::element row;
        REQUIRE(drinks[info.name].get(row) == simdjson::SUCCESS);
        std::vector<PotionEffect> measured;
        for (simdjson::dom::element effect : row["effects"].get_array().value()) {
            const std::string_view name = effect["effect"].get_string().value();
            measured.push_back(PotionEffect{*effect_from_name(name),
                                            static_cast<i32>(effect["duration"].get_int64().value()),
                                            static_cast<u8>(effect["amplifier"].get_int64().value())});
        }
        std::vector<PotionEffect> ours;
        for (const PotionEffect& effect : info.effects) {
            if (!effect_info(effect.effect).instantaneous) {
                ours.push_back(effect);  // instant ones send no Entity Effect
            }
        }
        INFO(info.name);
        REQUIRE(measured.size() == ours.size());
        for (usize k = 0; k < ours.size(); ++k) {
            CHECK(measured[k].effect == ours[k].effect);
            CHECK(measured[k].duration == ours[k].duration);
            CHECK(measured[k].amplifier == ours[k].amplifier);
        }
        ++checked;
    }
    CHECK(checked == kPotionCount);
}

namespace {

/// A measured campaign, or nothing (and a warning) when it has not run.
[[nodiscard]] std::optional<simdjson::dom::element> campaign(simdjson::dom::parser& parser,
                                                             std::string_view       name) {
    const auto path = normalized("brewing.json");
    if (!std::filesystem::exists(path)) {
        WARN("no brewing.json — run scripts/measure_brewing.py");
        return std::nullopt;
    }
    simdjson::dom::element document;
    if (parser.load(path.string()).get(document) != simdjson::SUCCESS) {
        FAIL("brewing.json does not parse");
    }
    simdjson::dom::element out;
    if (document[name].get(out) != simdjson::SUCCESS ||
        out["error"].error() == simdjson::SUCCESS) {
        WARN("brewing.json has no usable " << name << " campaign");
        return std::nullopt;
    }
    return out;
}

/// The one effect of that name in a measured list, as (amplifier, duration).
[[nodiscard]] std::optional<std::pair<i64, i64>> measured_effect(simdjson::dom::element effects,
                                                                 std::string_view       name) {
    for (simdjson::dom::element effect : effects.get_array().value()) {
        if (effect["effect"].get_string().value() == name) {
            return std::pair{effect["amplifier"].get_int64().value(),
                             effect["duration"].get_int64().value()};
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("the splash law against the bot's Entity Effects", "[brewing][parity]") {
    simdjson::dom::parser parser;
    const auto            splash = campaign(parser, "splash");
    if (!splash) {
        return;
    }
    usize checked = 0;
    for (const auto& [label, base] :
         {std::pair{std::string_view{"speed_1000"}, 1000}, std::pair{std::string_view{"speed_100"}, 100}}) {
        for (auto [key, row] : (*splash)[label].get_object().value()) {
            const f64 r = std::stod(std::string{key});
            // The potion broke a hundredth of a block above the bot's floor.
            const f64  factor = splash_factor(r * r + 0.0001, r == 0.0);
            const i32  expect = splash_duration(base, factor);
            const auto got    = measured_effect(row["effects"], "minecraft:speed");
            INFO(label << " at r = " << r);
            if (factor <= 0.0 || expect <= kSplashMinimumTicks) {
                CHECK_FALSE(got.has_value());
            } else {
                REQUIRE(got.has_value());
                CHECK(got->second == expect);
            }
            ++checked;
        }
    }
    for (auto [key, row] : (*splash)["heal_amp1"].get_object().value()) {
        const f64 r      = std::stod(std::string{key});
        const f64 factor = splash_factor(r * r + 0.0001, r == 0.0);
        const f64 before = row["health_before"].get_double().value();
        const f64 after  = row["health_after"].get_double().value();
        Target    target{static_cast<f32>(before)};
        apply_instant(Effect::InstantHealth, 1, factor, target);
        INFO("healing II at r = " << r);
        CHECK_THAT(static_cast<f64>(target.health()), Catch::Matchers::WithinAbs(after, 1e-4));
        ++checked;
    }
    CHECK(checked > 20);
}

TEST_CASE("tipped and spectral arrows against the bot's Entity Effects", "[brewing][parity]") {
    simdjson::dom::parser parser;
    const auto            arrows = campaign(parser, "arrow");
    if (!arrows) {
        return;
    }
    usize checked = 0;
    for (simdjson::dom::element row : (*arrows)["cases"].get_array().value()) {
        const std::string_view nbt  = row["nbt"].get_string().value();
        const std::string_view kind = row["kind"].get_string().value();
        INFO(kind << " {" << nbt << "}");
        if (kind == "spectral_arrow") {
            if (nbt.empty()) {
                const auto glow = measured_effect(row["effects"], "minecraft:glowing");
                REQUIRE(glow.has_value());
                CHECK(glow->second == kSpectralGlowTicks);
                ++checked;
            }
            continue;
        }
        const auto quote = nbt.find("Potion:\"");
        if (quote == std::string_view::npos ||
            nbt.find("CustomPotionEffects") != std::string_view::npos) {
            continue;
        }
        const auto name = nbt.substr(quote + 8, nbt.find('"', quote + 8) - quote - 8);
        for (const PotionEffect& effect : potion_info(*potion_from_name(name)).effects) {
            if (effect_info(effect.effect).instantaneous) {
                continue;
            }
            const auto got = measured_effect(row["effects"], effect_info(effect.effect).name);
            REQUIRE(got.has_value());
            CHECK(got->first == effect.amplifier);
            CHECK(got->second == arrow_duration(effect.duration));
            ++checked;
        }
    }
    CHECK(checked >= 5);
}

TEST_CASE("the lingering cloud as the real server made it", "[brewing][parity]") {
    simdjson::dom::parser parser;
    const auto            lingering = campaign(parser, "lingering");
    if (!lingering) {
        return;
    }
    const Cloud            ours = lingering_cloud();
    simdjson::dom::element born = (*lingering)["long_swiftness"]["born"];
    CHECK(born["Radius"].get_double().value() == static_cast<f64>(ours.radius));
    CHECK(born["RadiusOnUse"].get_double().value() == static_cast<f64>(ours.radius_on_use));
    CHECK(born["RadiusPerTick"].get_double().value() == static_cast<f64>(ours.radius_per_tick));
    CHECK(born["Duration"].get_int64().value() == ours.duration);
    CHECK(born["DurationOnUse"].get_int64().value() == ours.duration_on_use);
    CHECK(born["WaitTime"].get_int64().value() == ours.wait_time);
    CHECK(born["ReapplicationDelay"].get_int64().value() == ours.reapplication_delay);
    const auto got = measured_effect((*lingering)["long_swiftness"]["effects"], "minecraft:speed");
    REQUIRE(got.has_value());
    CHECK(got->second == cloud_duration(9600));
}

TEST_CASE("suspicious stew as the real server's bot ate it", "[brewing][parity]") {
    simdjson::dom::parser parser;
    const auto            stew = campaign(parser, "stew");
    if (!stew) {
        return;
    }
    const auto plain =
        measured_effect((*stew)["night_vision_100"]["effects"], "minecraft:night_vision");
    REQUIRE(plain.has_value());
    CHECK(plain->second == 100);
    const auto bare = measured_effect((*stew)["no_duration"]["effects"], "minecraft:night_vision");
    REQUIRE(bare.has_value());
    CHECK(bare->second == kStewDefaultTicks);
}
