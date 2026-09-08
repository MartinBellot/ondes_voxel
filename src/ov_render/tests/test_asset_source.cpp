#include "ov/render/asset_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::render;

namespace {

std::string text_of(const std::optional<std::vector<u8>>& bytes) {
    REQUIRE(bytes.has_value());
    return std::string(bytes->begin(), bytes->end());
}

}  // namespace

TEST_CASE("a memory source returns what was put in it", "[assets]") {
    MemoryAssetSource source;
    source.add("assets/minecraft/models/block/stone.json", R"({"parent": "block/cube_all"})");

    CHECK(source.contains("assets/minecraft/models/block/stone.json"));
    CHECK(text_of(source.read("assets/minecraft/models/block/stone.json")) ==
          R"({"parent": "block/cube_all"})");
    CHECK_FALSE(source.read("assets/minecraft/models/block/absent.json").has_value());
}

TEST_CASE("the last pack pushed wins, per file", "[assets]") {
    // This is the whole mechanism of a resource pack stack: Faithful replaces
    // the textures it ships and inherits everything else from the jar
    // underneath, file by file rather than pack by pack.
    MemoryAssetSource vanilla;
    vanilla.add("a.json", "vanilla-a");
    vanilla.add("b.json", "vanilla-b");

    MemoryAssetSource pack;
    pack.add("a.json", "pack-a");

    AssetStack stack;
    stack.push(vanilla);
    stack.push(pack);

    CHECK(stack.size() == 2);
    CHECK(text_of(stack.read("a.json")) == "pack-a");
    CHECK(text_of(stack.read("b.json")) == "vanilla-b");
    CHECK_FALSE(stack.read("c.json").has_value());
}

TEST_CASE("a directory source refuses to leave its pack", "[assets]") {
    // ov::ResourceLocation documents that '.' and '/' are legal path
    // characters, so `minecraft:../../etc/passwd` is a well-formed location.
    // This is the layer that has to say no.
    const DirectoryAssetSource source(std::filesystem::path("/does/not/exist"));

    CHECK_FALSE(source.read("../secrets").has_value());
    CHECK_FALSE(source.read("assets/../../etc/passwd").has_value());
    CHECK_FALSE(source.read("/etc/passwd").has_value());
    CHECK_FALSE(source.read("").has_value());
}
