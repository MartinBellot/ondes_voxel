#include "ov/render/asset_source.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/texture_animation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace ov::render {

/// Defined in test_png.cpp.
std::vector<u8> encode_test_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels);

}  // namespace ov::render

namespace {

using Rgba = std::array<u8, 4>;

/// A 16-wide strip of `colours.size()` frames, one colour each.
std::vector<u8> strip(const std::vector<Rgba>& colours) {
    std::vector<u8> pixels;
    for (const Rgba& colour : colours) {
        for (u32 i = 0; i < 16 * 16; ++i) {
            pixels.insert(pixels.end(), colour.begin(), colour.end());
        }
    }
    return pixels;
}

TextureAtlas atlas_with(std::string_view mcmeta, const std::vector<Rgba>& colours) {
    MemoryAssetSource source;
    const auto        rgba  = strip(colours);
    const auto        bytes = encode_test_png(16, 16 * static_cast<u32>(colours.size()), 4, rgba);
    source.add("assets/minecraft/textures/block/flow.png",
               std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    source.add("assets/minecraft/textures/block/flow.png.mcmeta", mcmeta);
    AtlasBuilder builder(source);
    builder.add("minecraft:block/flow");
    auto atlas = builder.build();
    REQUIRE(atlas.has_value());
    return std::move(*atlas);
}

Rgba first_texel(const TextureAnimator& animator) {
    const auto bytes = animator.bytes();
    REQUIRE(bytes.size() >= 4);
    return Rgba{bytes[0], bytes[1], bytes[2], bytes[3]};
}

constexpr Rgba kRed{200, 0, 0, 255};
constexpr Rgba kBlue{0, 0, 100, 255};

}  // namespace

TEST_CASE("the phase walks the frames by their own times and wraps", "[animation]") {
    SpriteAnimation animation;
    animation.frames = {AnimationFrame{0, 2}, AnimationFrame{1, 3}};

    CHECK(animation_phase(animation, 0).frame == 0);
    CHECK(animation_phase(animation, 1).frame == 0);
    CHECK(animation_phase(animation, 1).sub_tick == 1);
    CHECK(animation_phase(animation, 2).frame == 1);
    CHECK(animation_phase(animation, 4).sub_tick == 2);
    CHECK(animation_phase(animation, 4).frame_time == 3);
    // A cycle is five ticks.
    CHECK(animation_phase(animation, 5).frame == 0);
    CHECK(animation_phase(animation, 7).frame == 1);
    CHECK(animation_phase(animation, 7).next == 0);
}

TEST_CASE("the atlas keeps every cell of an animated strip", "[animation]") {
    const auto atlas = atlas_with(R"({"animation":{"frametime":2}})", {kRed, kBlue});
    REQUIRE(atlas.animations().size() == 1);
    const AtlasAnimation& animation = atlas.animations().front();
    CHECK(animation.name == "minecraft:block/flow");
    CHECK(animation.cells.size() == 2);
    CHECK(animation.width == 16);
    CHECK(animation.cells[1][2] == kBlue[2]);
}

TEST_CASE("a cut animation changes on frame boundaries and nowhere else", "[animation]") {
    const auto      atlas = atlas_with(R"({"animation":{"frametime":2}})", {kRed, kBlue});
    TextureAnimator animator(atlas);

    CHECK(animator.tick(0) == 1);
    CHECK(first_texel(animator) == kRed);
    // Every mip level of the sprite's rect, down to 1x1.
    CHECK(animator.patches().size() == atlas.mips().size());
    CHECK(animator.patches().back().width == 16u >> (atlas.mips().size() - 1));

    CHECK(animator.tick(1) == 0);
    CHECK(animator.patches().empty());
    CHECK(animator.tick(2) == 1);
    CHECK(first_texel(animator) == kBlue);
    CHECK(animator.tick(4) == 1);
    CHECK(first_texel(animator) == kRed);
    CHECK(animator.bytes().size() <= animator.max_bytes());
}

TEST_CASE("an interpolated animation blends towards the next frame every tick", "[animation]") {
    const auto atlas =
        atlas_with(R"({"animation":{"frametime":4,"interpolate":true}})", {kRed, kBlue});
    TextureAnimator animator(atlas);

    CHECK(animator.tick(0) == 1);
    CHECK(first_texel(animator) == kRed);
    CHECK(animator.tick(1) == 1);
    // A quarter of the way: 200 * 0.75 and 100 * 0.25.
    CHECK(first_texel(animator) == Rgba{150, 0, 25, 255});
    CHECK(animator.tick(2) == 1);
    CHECK(first_texel(animator) == Rgba{100, 0, 50, 255});
    CHECK(animator.tick(4) == 1);
    CHECK(first_texel(animator) == kBlue);
}

TEST_CASE("patches land on the sprite's own rect at every level", "[animation]") {
    const auto      atlas = atlas_with(R"({"animation":{}})", {kRed, kBlue});
    TextureAnimator animator(atlas);
    REQUIRE(animator.tick(0) == 1);

    const AtlasSprite* sprite = atlas.find("minecraft:block/flow");
    REQUIRE(sprite != nullptr);
    for (const AtlasPatch& patch : animator.patches()) {
        CAPTURE(patch.mip);
        CHECK(patch.x == sprite->x >> patch.mip);
        CHECK(patch.y == sprite->y >> patch.mip);
        CHECK(patch.width == sprite->width >> patch.mip);
    }
}
