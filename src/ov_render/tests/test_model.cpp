#include "ov/render/model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::render;

namespace {

ResourceLocation location(std::string_view text) {
    auto parsed = ResourceLocation::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

void add_model(MemoryAssetSource& source, std::string_view name, std::string_view json) {
    source.add("assets/minecraft/models/" + std::string(name) + ".json", json);
}

const FaceDefinition& face_of(const Model& model, usize element, Direction direction) {
    const auto& face = model.elements.at(element).faces[static_cast<usize>(direction)];
    REQUIRE(face.has_value());
    return *face;
}

}  // namespace

TEST_CASE("a parent chain contributes elements once, from the nearest ancestor", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/block", R"({"ambientocclusion": true})");
    add_model(source, "block/cube", R"({
        "parent": "block/block",
        "elements": [{
            "from": [0, 0, 0], "to": [16, 16, 16],
            "faces": {"up": {"texture": "#up", "cullface": "up"}}
        }]
    })");
    add_model(source, "block/cube_all", R"({
        "parent": "block/cube",
        "textures": {"up": "#all", "particle": "#all"}
    })");
    add_model(source, "block/stone", R"({
        "parent": "block/cube_all",
        "textures": {"all": "block/stone"}
    })");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/stone"));

    REQUIRE(model.has_value());
    REQUIRE((*model)->elements.size() == 1);

    // Three hops of texture variables: #up -> #all -> block/stone. Getting any
    // one of them wrong shows up as a missing texture on every ordinary block.
    CHECK(face_of(**model, 0, Direction::Up).sprite == "minecraft:block/stone");
    CHECK((*model)->particle_sprite == "minecraft:block/stone");
    CHECK(face_of(**model, 0, Direction::Up).cullface == Direction::Up);
}

TEST_CASE("a child's elements replace its parent's rather than adding to them", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/parent", R"({
        "elements": [
            {"from": [0, 0, 0], "to": [16, 16, 16], "faces": {}},
            {"from": [0, 0, 0], "to": [8, 8, 8], "faces": {}}
        ]
    })");
    add_model(source, "block/child", R"({
        "parent": "block/parent",
        "elements": [{"from": [0, 0, 0], "to": [16, 8, 16], "faces": {}}]
    })");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/child"));

    REQUIRE(model.has_value());
    CHECK((*model)->elements.size() == 1);
    CHECK((*model)->elements[0].to.y == 8.0F);
}

TEST_CASE("an empty elements list means nothing, not inheritance", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/parent",
              R"({"elements": [{"from": [0,0,0], "to": [16,16,16], "faces": {}}]})");
    add_model(source, "block/child", R"({"parent": "block/parent", "elements": []})");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/child"));

    REQUIRE(model.has_value());
    CHECK((*model)->elements.empty());
}

TEST_CASE("ambient occlusion comes from the root of the chain", "[model]") {
    // The format's own wording is that ambientocclusion "only works on Parent
    // file", and every model in the 1.20.1 assets that declares it is itself a
    // root. block/cross is the one this matters for: without it, grass and
    // flowers get smooth lighting they should not have.
    MemoryAssetSource source;
    add_model(source, "block/cross", R"({
        "ambientocclusion": false,
        "elements": [{"from": [0,0,8], "to": [16,16,8], "faces": {}}]
    })");
    add_model(source, "block/tinted_cross", R"({"parent": "block/cross"})");

    ModelLoader loader(source);

    const auto root = loader.load(location("block/cross"));
    REQUIRE(root.has_value());
    CHECK_FALSE((*root)->ambient_occlusion);

    const auto child = loader.load(location("block/tinted_cross"));
    REQUIRE(child.has_value());
    CHECK_FALSE((*child)->ambient_occlusion);
}

TEST_CASE("an unresolvable texture variable becomes the missing sprite", "[model]") {
    // Vanilla shows its checkerboard rather than dropping the face. A dropped
    // face is invisible and gets reported as "the block does not render"; a
    // checkerboard gets fixed.
    MemoryAssetSource source;
    add_model(source, "block/broken", R"({
        "elements": [{"from": [0,0,0], "to": [16,16,16],
                      "faces": {"up": {"texture": "#nosuch"}}}]
    })");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/broken"));

    REQUIRE(model.has_value());
    CHECK(face_of(**model, 0, Direction::Up).sprite == kMissingSprite);
}

TEST_CASE("a cyclic parent chain is an error, not a hang", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/a", R"({"parent": "block/b"})");
    add_model(source, "block/b", R"({"parent": "block/a"})");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/a"));

    REQUIRE_FALSE(model.has_value());
    CHECK(model.error() == ModelError::ParentChainTooLong);
}

TEST_CASE("a builtin parent ends the chain instead of failing", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/thing", R"({
        "parent": "builtin/generated",
        "textures": {"layer0": "block/thing"}
    })");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/thing"));

    REQUIRE(model.has_value());
    CHECK((*model)->elements.empty());
}

TEST_CASE("a model the pack does not have is NotFound", "[model]") {
    MemoryAssetSource source;
    ModelLoader       loader(source);

    const auto model = loader.load(location("block/absent"));
    REQUIRE_FALSE(model.has_value());
    CHECK(model.error() == ModelError::NotFound);
}

TEST_CASE("resolved models are cached", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/x", R"({"elements": []})");

    ModelLoader loader(source);
    const auto  first  = loader.load(location("block/x"));
    const auto  second = loader.load(location("block/x"));

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
    CHECK(loader.cached_models() == 1);
}

TEST_CASE("face fields are read, including the bottom spelling of down", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/faces", R"({
        "elements": [{
            "from": [0, 0, 0], "to": [16, 16, 16],
            "faces": {
                "down": {"texture": "block/a", "cullface": "bottom"},
                "up":   {"texture": "block/b", "uv": [0, 8, 16, 16],
                         "rotation": 90, "tintindex": 0}
            }
        }]
    })");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/faces"));
    REQUIRE(model.has_value());

    const auto& down = face_of(**model, 0, Direction::Down);
    CHECK(down.cullface == Direction::Down);
    CHECK(down.sprite == "minecraft:block/a");
    CHECK(down.tint_index == -1);

    const auto& up = face_of(**model, 0, Direction::Up);
    REQUIRE(up.uv.has_value());
    CHECK((*up.uv)[1] == 8.0F);
    CHECK(up.rotation == 90);
    CHECK(up.tint_index == 0);
    CHECK_FALSE(up.cullface.has_value());
}

TEST_CASE("element rotation is read with its defaults", "[model]") {
    MemoryAssetSource source;
    add_model(source, "block/rot", R"({
        "elements": [{
            "from": [0.8, 0, 8], "to": [15.2, 16, 8],
            "rotation": {"origin": [8, 8, 8], "axis": "y", "angle": 45, "rescale": true},
            "shade": false,
            "faces": {"north": {"texture": "block/c"}}
        }]
    })");

    ModelLoader loader(source);
    const auto  model = loader.load(location("block/rot"));
    REQUIRE(model.has_value());

    const auto& element = (*model)->elements.at(0);
    REQUIRE(element.rotation.has_value());
    CHECK(element.rotation->axis == Axis::Y);
    CHECK(element.rotation->angle == 45.0F);
    CHECK(element.rotation->rescale);
    CHECK_FALSE(element.shade);
    CHECK(element.from.x == 0.8F);
}
