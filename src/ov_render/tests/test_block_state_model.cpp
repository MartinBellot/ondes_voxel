#include "ov/render/block_state_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace {

BlockStateFile parse_ok(std::string_view json) {
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(json.data()), json.size());
    auto                      file = BlockStateFile::parse(bytes);
    REQUIRE(file.has_value());
    return std::move(*file);
}

using Property   = std::pair<std::string_view, std::string_view>;
using Properties = std::vector<Property>;

}  // namespace

TEST_CASE("a one-model block keys its only variant on the empty string", "[blockstate]") {
    const auto file = parse_ok(R"({"variants": {"": {"model": "minecraft:block/stone"}}})");

    CHECK_FALSE(file.is_multipart());

    const Properties none;
    const auto       groups = file.select(none);
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].alternatives.size() == 1);
    CHECK(groups[0].alternatives[0].model.full() == "minecraft:block/stone");
    CHECK(groups[0].alternatives[0].x == 0);
    CHECK(groups[0].alternatives[0].y == 0);
    CHECK_FALSE(groups[0].alternatives[0].uvlock);
}

TEST_CASE("variants select on every property the key names", "[blockstate]") {
    const auto file = parse_ok(R"({"variants": {
        "facing=north,half=bottom": {"model": "block/stairs"},
        "facing=east,half=bottom":  {"model": "block/stairs", "y": 90},
        "facing=east,half=top":     {"model": "block/stairs", "y": 90, "x": 180, "uvlock": true}
    }})");

    const Properties east_top{{"facing", "east"}, {"half", "top"}};
    const auto       groups = file.select(east_top);
    REQUIRE(groups.size() == 1);

    const auto& variant = groups[0].alternatives.at(0);
    CHECK(variant.y == 90);
    CHECK(variant.x == 180);
    CHECK(variant.uvlock);
}

TEST_CASE("a state no variant key matches draws nothing", "[blockstate]") {
    const auto       file = parse_ok(R"({"variants": {"facing=north": {"model": "block/a"}}})");
    const Properties south{{"facing", "south"}};

    CHECK(file.select(south).empty());
}

TEST_CASE("weighted alternatives are all kept, for the mesher to choose from", "[blockstate]") {
    // Vanilla picks one by hashing the block position, so the same block must
    // pick the same model every time it is re-meshed. Only the mesher knows
    // the position, so the choice cannot be made here.
    const auto file = parse_ok(R"({"variants": {"": [
        {"model": "block/stone"},
        {"model": "block/stone_mirrored", "weight": 2, "y": 180}
    ]}})");

    const Properties none;
    const auto       groups = file.select(none);
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].alternatives.size() == 2);
    CHECK(groups[0].alternatives[0].weight == 1);
    CHECK(groups[0].alternatives[1].weight == 2);
    CHECK(groups[0].alternatives[1].y == 180);
}

TEST_CASE("multipart draws every piece whose condition holds", "[blockstate]") {
    const auto file = parse_ok(R"({"multipart": [
        {"apply": {"model": "block/fence_post"}},
        {"when": {"north": "true"}, "apply": {"model": "block/fence_side", "uvlock": true}},
        {"when": {"east": "true"},  "apply": {"model": "block/fence_side", "y": 90}}
    ]})");

    CHECK(file.is_multipart());

    const Properties north_only{{"north", "true"}, {"east", "false"}};
    const auto       groups = file.select(north_only);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].alternatives.at(0).model.full() == "minecraft:block/fence_post");
    CHECK(groups[1].alternatives.at(0).uvlock);
}

TEST_CASE("a condition on a property the block lacks never holds", "[blockstate]") {
    // Treating an absent property as satisfied would put fence sides on a wall.
    const auto file =
        parse_ok(R"({"multipart": [{"when": {"north": "true"}, "apply": {"model": "block/a"}}]})");

    const Properties unrelated{{"facing", "north"}};
    CHECK(file.select(unrelated).empty());
}

TEST_CASE("a pipe in a when value is a set of accepted values", "[blockstate]") {
    // `"north": "side|up"` is one test with two values, not two tests. Walls
    // depend on it: a side connection and an upward one both draw the piece.
    const auto file = parse_ok(
        R"({"multipart": [{"when": {"north": "low|tall"}, "apply": {"model": "block/wall_side"}}]})");

    CHECK(file.select(Properties{{"north", "low"}}).size() == 1);
    CHECK(file.select(Properties{{"north", "tall"}}).size() == 1);
    CHECK(file.select(Properties{{"north", "none"}}).empty());
}

TEST_CASE("OR is a disjunction of conjunctions", "[blockstate]") {
    const auto file = parse_ok(R"({"multipart": [{
        "when": {"OR": [{"north": "true", "east": "true"}, {"south": "true"}]},
        "apply": {"model": "block/piece"}
    }]})");

    CHECK(file.select(Properties{{"north", "true"}, {"east", "true"}, {"south", "false"}}).size() ==
          1);
    // One half of a conjunction is not enough.
    CHECK(
        file.select(Properties{{"north", "true"}, {"east", "false"}, {"south", "false"}}).empty());
    CHECK(
        file.select(Properties{{"north", "false"}, {"east", "false"}, {"south", "true"}}).size() ==
        1);
}

TEST_CASE("a when value written as a JSON literal still compares", "[blockstate]") {
    const auto file = parse_ok(
        R"({"multipart": [{"when": {"lit": true, "age": 3}, "apply": {"model": "block/a"}}]})");

    CHECK(file.select(Properties{{"lit", "true"}, {"age", "3"}}).size() == 1);
    CHECK(file.select(Properties{{"lit", "false"}, {"age", "3"}}).empty());
}

TEST_CASE("a file that is neither variants nor multipart is malformed", "[blockstate]") {
    const std::string_view    json = R"({"something": {}})";
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(json.data()), json.size());

    const auto file = BlockStateFile::parse(bytes);
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error() == ModelError::Malformed);
}
