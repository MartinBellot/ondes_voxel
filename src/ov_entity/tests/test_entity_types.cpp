// The measured entity table, checked back against the measurement itself.
//
// The pack is built from data/vanilla/1.20.1/normalized/entities.json, which is
// what scripts/measure_entities.py wrote after asking a real 1.20.1 server. So
// this file does not ask "does the number look right" — it asks "did every
// number survive the trip through the emitter and the reader unchanged", which
// is the part of the chain a test can actually falsify.
//
// A handful of values are also spelled out on their own. Not because a formula
// predicts them, but because they are the ones a reader will want to see: a
// zombie is 0.6 by 1.95 with its eyes at 1.74, and if that stops being true the
// failure should name the zombie rather than a count.
#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <simdjson.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace ov;
using namespace ov::registry;

namespace {

[[nodiscard]] std::filesystem::path source_dir() { return std::filesystem::path{OV_SOURCE_DIR}; }

[[nodiscard]] const Registries* loaded() {
    static const auto pack =
        Registries::load(source_dir() / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return pack ? &*pack : nullptr;
}

[[nodiscard]] std::optional<ProtocolId> type_of(std::string_view name) {
    const auto registry = loaded()->find("minecraft:entity_type");
    if (!registry) {
        return std::nullopt;
    }
    return loaded()->protocol_id(*registry, name);
}

[[nodiscard]] std::filesystem::path measured_path() {
    return source_dir() / "data" / "vanilla" / "1.20.1" / "normalized" / "entities.json";
}

}  // namespace

TEST_CASE("every measured hitbox and eye height survives the pack unchanged",
          "[entity][registry]") {
    if (loaded() == nullptr) {
        WARN("registry.ovpack missing; run tools/ov_datagen/ovpack.py");
        return;
    }
    if (!std::filesystem::exists(measured_path())) {
        WARN("entities.json missing; run scripts/measure_entities.py");
        return;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(measured_path().string()).get(document) == simdjson::SUCCESS);

    const auto registry = loaded()->find("minecraft:entity_type");
    REQUIRE(registry.has_value());

    int boxes = 0;
    for (auto [name, box] : simdjson::dom::object{document["hitbox"]}) {
        const auto id = loaded()->protocol_id(*registry, std::string_view{name});
        REQUIRE(id.has_value());
        const auto info = loaded()->entity_type(*id);
        REQUIRE(info.has_value());
        // f32 in the pack against the f64 the measurement wrote. The tolerance
        // is the float's own precision and nothing looser, so a value that was
        // rounded on the way in rather than stored still fails here.
        CHECK(static_cast<double>(info->width) ==
              Catch::Approx(double{box["width"]}).epsilon(1e-6));
        CHECK(static_cast<double>(info->height) ==
              Catch::Approx(double{box["height"]}).epsilon(1e-6));
        ++boxes;
    }

    int eyes = 0;
    for (auto [name, value] : simdjson::dom::object{document["eye_height"]}) {
        const auto id = loaded()->protocol_id(*registry, std::string_view{name});
        REQUIRE(id.has_value());
        const auto info = loaded()->entity_type(*id);
        REQUIRE(info.has_value());
        CHECK(info->eye_measured);
        CHECK(static_cast<double>(info->eye_height) ==
              Catch::Approx(double{value}).epsilon(1e-6));
        ++eyes;
    }

    INFO("hitboxes " << boxes << ", eye heights " << eyes);
    CHECK(boxes >= 100);
    CHECK(eyes >= 100);
}

TEST_CASE("every attribute the game reported survives the pack unchanged", "[entity][registry]") {
    if (loaded() == nullptr || !std::filesystem::exists(measured_path())) {
        return;
    }
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    REQUIRE(parser.load(measured_path().string()).get(document) == simdjson::SUCCESS);

    const auto types      = loaded()->find("minecraft:entity_type").value();
    const auto attributes = loaded()->find("minecraft:attribute").value();

    int values = 0;
    for (auto [type_name, owned] : simdjson::dom::object{document["attributes"]}) {
        const auto type = loaded()->protocol_id(types, std::string_view{type_name});
        REQUIRE(type.has_value());
        for (auto [attribute_name, base] : simdjson::dom::object{owned}) {
            const auto attribute =
                loaded()->protocol_id(attributes, std::string_view{attribute_name});
            REQUIRE(attribute.has_value());
            const auto stored = loaded()->attribute_base(*type, *attribute);
            REQUIRE(stored.has_value());
            // Exact equality, not an approximation: the pack stores f64 for
            // precisely this reason. A movement speed of 0.23 is really
            // 0.23000000417232513 — the double nearest the float the game holds
            // — and squeezing it through an f32 would not come back.
            CHECK(*stored == double{base});
            ++values;
        }
    }
    INFO("attribute values " << values);
    CHECK(values >= 600);
}

TEST_CASE("the four types that could not be measured are refused by name", "[entity][registry]") {
    if (loaded() == nullptr) {
        return;
    }
    // A lightning bolt lives one tick, evoker fangs about twenty, a fishing
    // bobber cannot exist without an angler, and a player cannot be summoned at
    // all. None of them has a box here, and the answer is "no" rather than
    // "zero" — a mob nothing can ever hit is worse than one that fails to
    // spawn, because only one of the two says so.
    for (const std::string_view name :
         {"minecraft:lightning_bolt", "minecraft:evoker_fangs", "minecraft:fishing_bobber",
          "minecraft:player"}) {
        const auto id = type_of(name);
        REQUIRE(id.has_value());
        CHECK_FALSE(loaded()->entity_type(*id).has_value());
    }
}

TEST_CASE("the numbers a reader will want to check by eye", "[entity][registry]") {
    if (loaded() == nullptr) {
        return;
    }
    struct Case {
        std::string_view name;
        float            width;
        float            height;
        float            eye;
    };
    // Measured, not remembered: each box came out of
    // `execute positioned … if entity @e[…,dx=0,dy=0,dz=0]` bisected against a
    // running server, and each eye height out of a marker summoned at the mob's
    // own eye anchor.
    static constexpr Case kCases[] = {
        {"minecraft:zombie", 0.6F, 1.95F, 1.74F},
        {"minecraft:creeper", 0.6F, 1.7F, 1.445F},
        {"minecraft:enderman", 0.6F, 2.9F, 2.55F},
        {"minecraft:cow", 0.9F, 1.4F, 1.3F},
        {"minecraft:chicken", 0.4F, 0.7F, 0.644F},
        {"minecraft:ender_dragon", 16.0F, 8.0F, 6.8F},
        {"minecraft:item", 0.25F, 0.25F, 0.2125F},
    };
    for (const Case& test : kCases) {
        const auto id = type_of(test.name);
        REQUIRE(id.has_value());
        const auto info = loaded()->entity_type(*id);
        REQUIRE(info.has_value());
        CHECK(info->width == Catch::Approx(test.width).epsilon(1e-5));
        CHECK(info->height == Catch::Approx(test.height).epsilon(1e-5));
        CHECK(info->eye_height == Catch::Approx(test.eye).epsilon(1e-5));
    }
}

TEST_CASE("absence of an attribute is absence, not zero", "[entity][registry]") {
    if (loaded() == nullptr) {
        return;
    }
    const auto attributes = loaded()->find("minecraft:attribute").value();
    const auto attack =
        loaded()->protocol_id(attributes, "minecraft:generic.attack_damage").value();
    const auto jump = loaded()->protocol_id(attributes, "minecraft:horse.jump_strength").value();

    // A zombie has no jump strength; a horse does. A cow has no attack damage;
    // a zombie does. Answering 0.0 to any of these would make "does not have
    // it" indistinguishable from "has it, at nothing".
    const auto zombie = type_of("minecraft:zombie").value();
    CHECK(loaded()->attribute_base(zombie, attack) == 3.0);
    CHECK_FALSE(loaded()->attribute_base(zombie, jump).has_value());

    const auto cow = type_of("minecraft:cow").value();
    CHECK_FALSE(loaded()->attribute_base(cow, attack).has_value());

    const auto horse = type_of("minecraft:horse").value();
    CHECK(loaded()->attribute_base(horse, jump).has_value());

    // A boat has no attributes at all, which is different again from a type
    // that does not exist.
    const auto boat = type_of("minecraft:boat").value();
    CHECK(loaded()->entity_attributes(boat).empty());
    CHECK(loaded()->entity_type(boat).has_value());
}
