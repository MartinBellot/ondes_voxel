#include "ov/render/item_model.hpp"

#include "ov/render/asset_source.hpp"
#include "ov/render/atlas.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace ov;
using namespace ov::render;

namespace {

/// A pack with one solid item and one flat one, and nothing else.
///
/// Hermetic, like every other model test here: the projection of a cube into a
/// sixteen-pixel cell is arithmetic, and arithmetic is checkable without a
/// resource pack or a GPU.
[[nodiscard]] MemoryAssetSource pack() {
    MemoryAssetSource source;

    // The root every block model inherits its GUI transform from. `block/block`
    // in the real assets says exactly this.
    source.add("assets/minecraft/models/block/base.json", R"({
        "gui_light": "side",
        "display": {
            "gui": {
                "rotation": [30, 225, 0],
                "translation": [0, 0, 0],
                "scale": [0.625, 0.625, 0.625]
            }
        }
    })");

    source.add("assets/minecraft/models/block/cube.json", R"({
        "parent": "minecraft:block/base",
        "elements": [{
            "from": [0, 0, 0],
            "to": [16, 16, 16],
            "faces": {
                "down":  {"texture": "minecraft:block/white"},
                "up":    {"texture": "minecraft:block/white"},
                "north": {"texture": "minecraft:block/white"},
                "south": {"texture": "minecraft:block/white"},
                "west":  {"texture": "minecraft:block/white"},
                "east":  {"texture": "minecraft:block/white"}
            }
        }]
    })");
    source.add("assets/minecraft/models/item/cube.json",
               R"({"parent": "minecraft:block/cube"})");

    source.add("assets/minecraft/models/item/generated.json",
               R"({"parent": "builtin/generated", "gui_light": "front"})");
    source.add("assets/minecraft/models/item/sword.json", R"({
        "parent": "minecraft:item/generated",
        "textures": {"layer0": "minecraft:item/sword"}
    })");

    return source;
}

/// An atlas with no sprites at all. Every name resolves to the checkerboard,
/// which is the right answer for a geometry test: the rect is a rect.
[[nodiscard]] TextureAtlas empty_atlas(const AssetSource& source) {
    const AtlasBuilder builder(source);
    auto               atlas = builder.build();
    REQUIRE(atlas.has_value());
    return std::move(*atlas);
}

}  // namespace

TEST_CASE("a block item is three faces in its cell, not six", "[item_model]") {
    const auto     source = pack();
    ItemModelCache cache(source);
    cache.resolve("minecraft:cube");
    cache.bake(empty_atlas(source));

    const ItemMesh* mesh = cache.mesh("minecraft:cube");
    REQUIRE(mesh != nullptr);
    CHECK(mesh->drawable);
    CHECK_FALSE(mesh->flat);
    // Thirty degrees about X and two hundred and twenty-five about Y show
    // exactly three of a cube's six faces. Drawing all six would cost twice the
    // fill to cover half of it.
    CHECK(mesh->quads.size() == 3);
}

TEST_CASE("a block item fits its cell, and fills it the way vanilla does",
          "[item_model]") {
    const auto     source = pack();
    ItemModelCache cache(source);
    cache.resolve("minecraft:cube");
    cache.bake(empty_atlas(source));

    const ItemMesh* mesh = cache.mesh("minecraft:cube");
    REQUIRE(mesh != nullptr);

    f32 min_x = 1e9F;
    f32 max_x = -1e9F;
    f32 min_y = 1e9F;
    f32 max_y = -1e9F;
    for (const ItemQuad& quad : mesh->quads) {
        for (const ItemPoint& point : quad.position) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        }
    }

    // The silhouette is decided by three numbers and nothing else: the scale
    // (0.625), the cell (16 pixels), and the rotation.
    //
    // Width  = 2 · (√2/2) · 0.625 · 16          = 14.142
    // Height = 2 · (½·cos30 + √2/2·sin30) · 10  = 15.731
    //
    // Both under sixteen, which is why a block icon does not spill into its
    // neighbour — and the height being within a quarter-pixel of the cell is
    // why vanilla chose 0.625 rather than a rounder number.
    constexpr f32 kRoot2 = 1.41421356F;
    const f32     width  = kRoot2 * 0.625F * 16.0F;
    const f32     height = 2.0F * (0.5F * std::cos(30.0F * std::numbers::pi_v<f32> / 180.0F) +
                               (kRoot2 / 2.0F) * 0.5F) *
                       0.625F * 16.0F;

    CHECK_THAT(static_cast<f64>(max_x - min_x),
               Catch::Matchers::WithinAbs(static_cast<f64>(width), 0.01));
    CHECK_THAT(static_cast<f64>(max_y - min_y),
               Catch::Matchers::WithinAbs(static_cast<f64>(height), 0.01));
    CHECK(width < kItemCellSize);
    CHECK(height < kItemCellSize);

    // Centred on the cell: the model turns about its own middle, so the
    // silhouette's middle is the cell's.
    CHECK_THAT(static_cast<f64>((min_x + max_x) * 0.5F),
               Catch::Matchers::WithinAbs(8.0, 0.01));
    CHECK_THAT(static_cast<f64>((min_y + max_y) * 0.5F),
               Catch::Matchers::WithinAbs(8.0, 0.01));
}

TEST_CASE("the visible faces are the top and two sides, shaded as such",
          "[item_model]") {
    const auto     source = pack();
    ItemModelCache cache(source);
    cache.resolve("minecraft:cube");
    cache.bake(empty_atlas(source));

    const ItemMesh* mesh = cache.mesh("minecraft:cube");
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->quads.size() == 3);

    std::vector<f32> shades;
    for (const ItemQuad& quad : mesh->quads) {
        shades.push_back(quad.shade);
    }
    std::ranges::sort(shades);
    // Vanilla's directional shading: 0.6 for east/west, 0.8 for north/south,
    // 1.0 for the top. A cube turned this way shows one of each, which is why
    // its three faces are three different greys.
    CHECK_THAT(static_cast<f64>(shades[0]), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK_THAT(static_cast<f64>(shades[1]), Catch::Matchers::WithinAbs(0.8, 1e-6));
    CHECK_THAT(static_cast<f64>(shades[2]), Catch::Matchers::WithinAbs(1.0, 1e-6));

    // Sorted back to front, which is the contract the drawer relies on: it
    // has no depth buffer and paints in order.
    //
    // ⚠️ The *top* face is not the last one. Its centre sits higher but not
    // nearer: the two side faces lean toward the viewer and their centres come
    // out 0.19 blocks out against the top's 0.156. It does not matter for a
    // cube, whose three visible faces never overlap — it matters for a stair
    // or a torch, which is why the sort exists at all, and it is worth writing
    // down because "the top is nearest" is the obvious wrong guess.
    CHECK(std::ranges::is_sorted(mesh->quads, [](const ItemQuad& a, const ItemQuad& b) {
        return a.depth < b.depth;
    }));
}

TEST_CASE("a generated item is a picture, not a solid", "[item_model]") {
    const auto     source = pack();
    ItemModelCache cache(source);
    cache.resolve("minecraft:sword");
    cache.bake(empty_atlas(source));

    const ItemMesh* mesh = cache.mesh("minecraft:sword");
    REQUIRE(mesh != nullptr);
    CHECK(mesh->flat);
    CHECK(mesh->drawable);
    CHECK(mesh->quads.empty());
    // One layer, and it is an atlas rect rather than a name: the caller draws
    // it and has no business resolving a sprite.
    REQUIRE(mesh->layers.size() == 1);
    CHECK(mesh->layers[0].u1 > mesh->layers[0].u0);
}

TEST_CASE("an item with no model file is reported rather than drawn blank",
          "[item_model]") {
    const auto     source = pack();
    ItemModelCache cache(source);
    cache.resolve("minecraft:nothing_like_this");
    cache.bake(empty_atlas(source));

    const ItemMesh* mesh = cache.mesh("minecraft:nothing_like_this");
    REQUIRE(mesh != nullptr);
    CHECK_FALSE(mesh->drawable);
    CHECK(cache.missing_count() == 1);
}

TEST_CASE("the sprites an item needs are collected before the atlas is built",
          "[item_model]") {
    const auto     source = pack();
    ItemModelCache cache(source);
    cache.resolve("minecraft:cube");
    cache.resolve("minecraft:sword");
    // Sorted and unique, so the same name asked for by a thousand items costs
    // nothing.
    const auto& sprites = cache.sprites();
    CHECK(sprites.size() == 2);
    CHECK(std::ranges::is_sorted(sprites));
    CHECK(std::ranges::find(sprites, "minecraft:block/white") != sprites.end());
    CHECK(std::ranges::find(sprites, "minecraft:item/sword") != sprites.end());

    // Resolving twice is free and changes nothing.
    cache.resolve("minecraft:cube");
    CHECK(cache.sprites().size() == 2);
    CHECK(cache.resolved_count() == 2);
}
