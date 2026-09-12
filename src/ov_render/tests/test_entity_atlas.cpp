// The entity atlas: every texture on one sheet, nothing overlapping, every
// pixel where its rectangle says it is.
#include "ov/render/entity_atlas.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;

namespace {

[[nodiscard]] TextureImage solid(u32 width, u32 height, u8 red) {
    TextureImage image;
    image.width  = width;
    image.height = height;
    image.rgba.assign(static_cast<usize>(width) * height * 4, 0);
    for (usize pixel = 0; pixel < static_cast<usize>(width) * height; ++pixel) {
        image.rgba[pixel * 4 + 0] = red;
        image.rgba[pixel * 4 + 3] = 255;
    }
    return image;
}

}  // namespace

TEST_CASE("the atlas packs every texture without overlap and keeps its pixels", "[entity]") {
    std::vector<std::pair<std::string, TextureImage>> images;
    images.emplace_back("minecraft:entity/cow/cow", solid(128, 64, 10));
    images.emplace_back("minecraft:entity/villager/villager", solid(128, 128, 20));
    images.emplace_back("minecraft:entity/chicken", solid(64, 32, 30));
    images.emplace_back("minecraft:entity/enderdragon/dragon", solid(512, 512, 40));
    const EntityAtlas atlas = EntityAtlas::pack(std::move(images), 1024);

    REQUIRE(atlas.size() == 4);
    const TextureImage& sheet = atlas.image();
    // Power-of-two sheets: MoltenVK does not need them, but nothing is lost.
    CHECK((sheet.width & (sheet.width - 1)) == 0);
    CHECK((sheet.height & (sheet.height - 1)) == 0);

    std::vector<UvRect> rects;
    for (const auto& [name, red] :
         std::array<std::pair<std::string, u8>, 4>{{{"minecraft:entity/cow/cow", 10},
                                                     {"minecraft:entity/villager/villager", 20},
                                                     {"minecraft:entity/chicken", 30},
                                                     {"minecraft:entity/enderdragon/dragon", 40}}}) {
        const auto rect = atlas.find(name);
        REQUIRE(rect.has_value());
        rects.push_back(*rect);
        // The texel at the rectangle's centre is the image's own colour.
        const auto x = static_cast<u32>((rect->u0 + rect->u1) * 0.5F * static_cast<f32>(sheet.width));
        const auto y = static_cast<u32>((rect->v0 + rect->v1) * 0.5F * static_cast<f32>(sheet.height));
        CHECK(sheet.rgba[sheet.index(x, y)] == red);
    }
    // No two rectangles share a texel.
    for (usize a = 0; a < rects.size(); ++a) {
        for (usize b = a + 1; b < rects.size(); ++b) {
            const bool apart = rects[a].u1 <= rects[b].u0 || rects[b].u1 <= rects[a].u0 ||
                               rects[a].v1 <= rects[b].v0 || rects[b].v1 <= rects[a].v0;
            CHECK(apart);
        }
    }
    // A rectangle is exactly its image's size on the sheet: no padding, no
    // resampling.
    const auto cow = atlas.find("minecraft:entity/cow/cow");
    REQUIRE(cow.has_value());
    CHECK((cow->u1 - cow->u0) * static_cast<f32>(sheet.width) == Catch::Approx(128.0F));
    CHECK((cow->v1 - cow->v0) * static_cast<f32>(sheet.height) == Catch::Approx(64.0F));

    CHECK_FALSE(atlas.find("minecraft:entity/pig/pig").has_value());
}
