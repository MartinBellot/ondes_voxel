#include "ov/render/asset_path.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;

namespace {

ResourceLocation location(std::string_view text) {
    auto parsed = ResourceLocation::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("asset paths follow the pack layout", "[assets]") {
    CHECK(model_asset_path(location("block/cube")) == "assets/minecraft/models/block/cube.json");
    CHECK(blockstate_asset_path(location("stone")) == "assets/minecraft/blockstates/stone.json");
    CHECK(texture_asset_path(location("block/stone")) ==
          "assets/minecraft/textures/block/stone.png");
}

TEST_CASE("a non-vanilla namespace lands in its own tree", "[assets]") {
    CHECK(model_asset_path(location("ov:block/test")) == "assets/ov/models/block/test.json");
}

TEST_CASE("builtin models are recognised so the parent walk can stop", "[assets]") {
    CHECK(is_builtin_model(location("builtin/generated")));
    CHECK(is_builtin_model(location("builtin/entity")));
    CHECK_FALSE(is_builtin_model(location("block/block")));
}
