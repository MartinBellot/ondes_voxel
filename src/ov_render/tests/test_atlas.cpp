#include "ov/render/asset_source.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace ov::render {

/// Defined in test_png.cpp. The atlas reads PNG bytes out of its AssetSource,
/// so its tests need real ones — and building them inline is what keeps this
/// file hermetic. Nothing here touches `run/assets`: the game's textures are
/// gitignored, and a test that needed them would not run in CI.
std::vector<u8> encode_test_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels);

}  // namespace ov::render

namespace {

using Rgba = std::array<u8, 4>;

constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kGreen{0, 255, 0, 255};
constexpr Rgba kBlue{0, 0, 255, 255};

std::vector<u8> solid(u32 width, u32 height, Rgba colour) {
    std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
    for (usize i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = colour[0];
        pixels[i + 1] = colour[1];
        pixels[i + 2] = colour[2];
        pixels[i + 3] = colour[3];
    }
    return pixels;
}

void add_texture(MemoryAssetSource& source, std::string_view name, u32 width, u32 height,
                 const std::vector<u8>& rgba) {
    const auto bytes = encode_test_png(width, height, 4, rgba);
    source.add("assets/minecraft/textures/" + std::string(name) + ".png",
               std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void add_mcmeta(MemoryAssetSource& source, std::string_view name, std::string_view json) {
    source.add("assets/minecraft/textures/" + std::string(name) + ".png.mcmeta", json);
}

Rgba texel(const AtlasMip& mip, u32 x, u32 y) {
    const usize offset = (static_cast<usize>(y) * mip.width + x) * 4;
    return Rgba{mip.rgba[offset], mip.rgba[offset + 1], mip.rgba[offset + 2], mip.rgba[offset + 3]};
}

const AtlasSprite& sprite_of(const TextureAtlas& atlas, std::string_view name) {
    const AtlasSprite* sprite = atlas.find(name);
    REQUIRE(sprite != nullptr);
    return *sprite;
}

bool overlaps(const AtlasSprite& a, const AtlasSprite& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

}  // namespace

TEST_CASE("every sprite gets a rect of its own", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));
    add_texture(source, "block/green", 16, 16, solid(16, 16, kGreen));
    add_texture(source, "block/blue", 16, 16, solid(16, 16, kBlue));

    AtlasBuilder builder(source);
    builder.add("block/red").add("block/green").add("block/blue");

    const auto atlas = builder.build();
    REQUIRE(atlas.has_value());

    // Square and a power of two, as the block atlas is in vanilla.
    CHECK(atlas->width() == atlas->height());
    CHECK(std::has_single_bit(atlas->width()));

    // Three requested, plus the checkerboard that is always stitched in.
    REQUIRE(atlas->sprites().size() == 4);
    CHECK(atlas->find(kMissingSprite) != nullptr);

    for (usize i = 0; i < atlas->sprites().size(); ++i) {
        for (usize j = i + 1; j < atlas->sprites().size(); ++j) {
            INFO(atlas->sprites()[i].name << " vs " << atlas->sprites()[j].name);
            CHECK_FALSE(overlaps(atlas->sprites()[i], atlas->sprites()[j]));
        }
        CHECK(atlas->sprites()[i].x + atlas->sprites()[i].width <= atlas->width());
        CHECK(atlas->sprites()[i].y + atlas->sprites()[i].height <= atlas->height());
    }
}

TEST_CASE("a sprite's uv rect maps back onto its own pixels", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));
    add_texture(source, "block/green", 16, 16, solid(16, 16, kGreen));
    add_texture(source, "block/blue", 16, 16, solid(16, 16, kBlue));

    AtlasBuilder builder(source);
    const auto   atlas =
        builder.add("block/red").add("block/green").add("block/blue").build().value();

    const std::array<std::pair<std::string_view, Rgba>, 3> expected{{
        {"minecraft:block/red", kRed},
        {"minecraft:block/green", kGreen},
        {"minecraft:block/blue", kBlue},
    }};

    const AtlasMip& level0 = atlas.mip(0);
    const auto      side   = static_cast<f32>(atlas.width());

    for (const auto& [name, colour] : expected) {
        const AtlasSprite& sprite = sprite_of(atlas, name);
        const SpriteUv     uv     = atlas.uv(name);

        // The rect is exact, so denormalising the corners lands on the sprite's
        // own first and last texel. An off-by-one here is the classic atlas
        // bug: a one-texel fringe of the neighbouring sprite on every block.
        CHECK(static_cast<u32>(uv.u0 * side) == sprite.x);
        CHECK(static_cast<u32>(uv.v0 * side) == sprite.y);
        CHECK(static_cast<u32>(uv.u1 * side) == sprite.x + sprite.width);
        CHECK(static_cast<u32>(uv.v1 * side) == sprite.y + sprite.height);

        CHECK(texel(level0, sprite.x, sprite.y) == colour);
        CHECK(texel(level0, sprite.x + sprite.width - 1, sprite.y + sprite.height - 1) == colour);
    }
}

TEST_CASE("mip levels halve, and stop where vanilla's default stops", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));
    add_texture(source, "block/green", 16, 16, solid(16, 16, kGreen));

    AtlasBuilder builder(source);
    const auto   atlas = builder.add("block/red").add("block/green").build().value();

    // Four halvings below level 0 is vanilla's default and exactly what a 16x16
    // sprite survives: 16 >> 4 is 1.
    REQUIRE(atlas.mip_level() == 4);
    REQUIRE(atlas.mips().size() == 5);

    u32 width = atlas.width();
    for (u32 level = 0; level < atlas.mips().size(); ++level) {
        CHECK(atlas.mip(level).width == width);
        CHECK(atlas.mip(level).height == width);
        CHECK(atlas.mip(level).rgba.size() == static_cast<usize>(width) * width * 4);
        width /= 2;
    }

    // A level past the end clamps rather than reading off the end of the array.
    CHECK(atlas.mip(99).width == atlas.mips().back().width);
}

TEST_CASE("mip averaging is weighted by alpha, so cutouts keep their colour", "[atlas]") {
    // One opaque green texel per 2x2 group, the other three fully transparent
    // black. This is the shape of every cutout sprite in the game — leaves,
    // grass, glass — where the RGB under a transparent texel is arbitrary.
    std::vector<u8> pixels(16 * 16 * 4, 0);
    for (u32 y = 0; y < 16; y += 2) {
        for (u32 x = 0; x < 16; x += 2) {
            const usize offset = (static_cast<usize>(y) * 16 + x) * 4;
            pixels[offset + 1] = 255;
            pixels[offset + 3] = 255;
        }
    }

    MemoryAssetSource source;
    add_texture(source, "block/leaves", 16, 16, pixels);

    AtlasBuilder builder(source);
    const auto   atlas = builder.add("block/leaves").build().value();

    const AtlasSprite& sprite = sprite_of(atlas, "minecraft:block/leaves");
    const AtlasMip&    level1 = atlas.mip(1);

    for (u32 y = sprite.y / 2; y < (sprite.y + sprite.height) / 2; ++y) {
        for (u32 x = sprite.x / 2; x < (sprite.x + sprite.width) / 2; ++x) {
            const Rgba value = texel(level1, x, y);
            // A naive box filter answers 63 here — a quarter of the green — and
            // every leaf block in the distance turns near-black. Weighting the
            // colour by coverage keeps it green and lets the alpha carry the
            // fade. This assertion is the whole reason downsample() is not four
            // lines long.
            CHECK(value[0] == 0);
            CHECK(value[1] == 255);
            CHECK(value[2] == 0);
            CHECK(value[3] == 64);
        }
    }
}

TEST_CASE("a fully transparent group stays fully transparent", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/air", 16, 16, solid(16, 16, Rgba{9, 9, 9, 0}));

    AtlasBuilder builder(source);
    const auto   atlas  = builder.add("block/air").build().value();
    const auto&  sprite = sprite_of(atlas, "minecraft:block/air");

    CHECK(texel(atlas.mip(1), sprite.x / 2, sprite.y / 2) == Rgba{0, 0, 0, 0});
}

TEST_CASE("sprites are scaled up to the pack's resolution", "[atlas]") {
    // What `run/assets` looks like: Faithful 32x over the vanilla jar, so the
    // textures the pack redrew are 32x32 and the rest are still 16x16.
    MemoryAssetSource source;
    add_texture(source, "block/fine", 32, 32, solid(32, 32, kBlue));
    add_texture(source, "block/coarse", 16, 16, solid(16, 16, kRed));

    AtlasBuilder builder(source);
    const auto   atlas = builder.add("block/fine").add("block/coarse").build().value();

    const AtlasSprite& coarse = sprite_of(atlas, "minecraft:block/coarse");
    const AtlasSprite& fine   = sprite_of(atlas, "minecraft:block/fine");

    // Two widths, one sprite each: the tie goes to the wider, so a pack split
    // evenly between two resolutions sharpens rather than blurs.
    CHECK(fine.width == 32);
    CHECK(fine.height == 32);
    // Doubled by pixel replication, not resampled: same colour everywhere, and
    // the same texel density as its neighbour.
    CHECK(coarse.width == 32);
    CHECK(coarse.height == 32);
    CHECK(texel(atlas.mip(0), coarse.x, coarse.y) == kRed);
    CHECK(texel(atlas.mip(0), coarse.x + 31, coarse.y + 31) == kRed);

    // The checkerboard is 16x16 as drawn, and scales with everything else.
    CHECK(sprite_of(atlas, kMissingSprite).width == 32);
}

TEST_CASE("a texture that spans two blocks does not set the pack's resolution", "[atlas]") {
    // lava_flow and water_flow are 64 wide in a 32x pack, because a flow
    // texture is two blocks across. Reading them as the pack's resolution
    // doubles every other sprite and quadruples the atlas for nothing.
    MemoryAssetSource source;
    add_texture(source, "block/stone", 32, 32, solid(32, 32, kRed));
    add_texture(source, "block/dirt", 32, 32, solid(32, 32, kGreen));
    add_texture(source, "block/sand", 32, 32, solid(32, 32, kBlue));
    add_texture(source, "block/lava_flow", 64, 64, solid(64, 64, kRed));

    AtlasBuilder builder(source);
    const auto   atlas = builder.add("block/stone")
                             .add("block/dirt")
                             .add("block/sand")
                             .add("block/lava_flow")
                             .build()
                             .value();

    CHECK(sprite_of(atlas, "minecraft:block/stone").width == 32);
    CHECK(sprite_of(atlas, "minecraft:block/lava_flow").width == 64);
    CHECK(sprite_of(atlas, kMissingSprite).width == 32);
}

TEST_CASE("a sprite that cannot survive four halvings lowers the mip level", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/odd", 12, 12, solid(12, 12, kGreen));

    AtlasBuilder builder(source);
    const auto   atlas = builder.add("block/odd").build().value();

    // 12 is 4 x 3: two halvings and no more. Vanilla lowers the level for the
    // whole atlas rather than padding the sprite, and so do we.
    CHECK(atlas.mip_level() == 2);
    CHECK(atlas.mips().size() == 3);

    // And every rect stays aligned to what the level demands, which is what
    // stops a mip texel from straddling two sprites.
    const u32 alignment = 1U << atlas.mip_level();
    for (const AtlasSprite& sprite : atlas.sprites()) {
        INFO(sprite.name);
        CHECK(sprite.x % alignment == 0);
        CHECK(sprite.y % alignment == 0);
        CHECK(sprite.width % alignment == 0);
        CHECK(sprite.height % alignment == 0);
    }
}

TEST_CASE("asking for no mips gives one level", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));

    AtlasBuilder builder(source);
    const auto   atlas = builder.set_mip_level(0).add("block/red").build().value();

    CHECK(atlas.mip_level() == 0);
    CHECK(atlas.mips().size() == 1);
}

TEST_CASE("a missing texture becomes the checkerboard, never a hole", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));
    // Present, but not a PNG. A pack does this by shipping a text file with a
    // .png name, and it must not be more fatal than shipping nothing.
    source.add("assets/minecraft/textures/block/broken.png", "not a png at all");

    AtlasBuilder builder(source);
    const auto   atlas =
        builder.add("block/red").add("block/absent").add("block/broken").build().value();

    for (const std::string_view name : {"minecraft:block/absent", "minecraft:block/broken"}) {
        INFO(name);
        const AtlasSprite& sprite = sprite_of(atlas, name);
        CHECK(sprite.missing);
        // They share the checkerboard's rect rather than each getting a copy.
        const SpriteUv fallback = atlas.uv(kMissingSprite);
        CHECK(sprite.uv.u0 == fallback.u0);
        CHECK(sprite.uv.v0 == fallback.v0);
    }

    CHECK_FALSE(sprite_of(atlas, "minecraft:block/red").missing);

    // A name that was never even requested still textures something.
    const SpriteUv unknown = atlas.uv("minecraft:block/never_mentioned");
    CHECK(unknown.u0 == atlas.uv(kMissingSprite).u0);
    CHECK(atlas.find("minecraft:block/never_mentioned") == nullptr);

    // And the checkerboard is the checkerboard: black and magenta quadrants.
    const AtlasSprite& missing = sprite_of(atlas, kMissingSprite);
    const u32          half    = missing.width / 2;
    CHECK(texel(atlas.mip(0), missing.x, missing.y) == Rgba{0, 0, 0, 255});
    CHECK(texel(atlas.mip(0), missing.x + half, missing.y) == Rgba{248, 0, 248, 255});
    CHECK(texel(atlas.mip(0), missing.x, missing.y + half) == Rgba{248, 0, 248, 255});
    CHECK(texel(atlas.mip(0), missing.x + half, missing.y + half) == Rgba{0, 0, 0, 255});
}

TEST_CASE("an empty builder still yields an atlas with the checkerboard", "[atlas]") {
    MemoryAssetSource source;
    AtlasBuilder      builder(source);

    const auto atlas = builder.build();
    REQUIRE(atlas.has_value());
    CHECK(builder.requested_sprites() == 0);
    REQUIRE(atlas->sprites().size() == 1);
    CHECK(atlas->sprites().front().name == kMissingSprite);
}

TEST_CASE("names are canonical, so a sprite is stitched once", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));

    AtlasBuilder builder(source);
    builder.add("block/red").add("minecraft:block/red").add("block/red");

    CHECK(builder.requested_sprites() == 1);

    const auto atlas = builder.build().value();
    CHECK(atlas.sprites().size() == 2);  // The sprite and the checkerboard.
    CHECK(atlas.find("minecraft:block/red") != nullptr);
}

TEST_CASE("an animation's .mcmeta is parsed, and frame 0 is what gets stitched", "[atlas]") {
    // Three stacked 16x16 frames: red, green, blue.
    std::vector<u8> strip;
    for (const Rgba colour : {kRed, kGreen, kBlue}) {
        const auto frame = solid(16, 16, colour);
        strip.insert(strip.end(), frame.begin(), frame.end());
    }

    MemoryAssetSource source;
    add_texture(source, "block/fire", 16, 48, strip);
    add_mcmeta(source, "block/fire", R"({
        "animation": {"frametime": 3, "interpolate": true,
                      "frames": [0, {"index": 2, "time": 7}, 9]}
    })");

    AtlasBuilder builder(source);
    const auto   atlas  = builder.add("block/fire").build().value();
    const auto&  sprite = sprite_of(atlas, "minecraft:block/fire");

    REQUIRE(sprite.animation.has_value());
    const SpriteAnimation& animation = *sprite.animation;

    // No width/height in the metadata: the frame is a square of the smaller
    // dimension, which is what makes a plain vertical strip parse at all.
    CHECK(animation.frame_width == 16);
    CHECK(animation.frame_height == 16);
    CHECK(animation.default_frame_time == 3);
    CHECK(animation.interpolate);

    // `9` names a frame the texture does not have; it is dropped, not fatal.
    REQUIRE(animation.frames.size() == 2);
    CHECK(animation.frames[0].index == 0);
    CHECK(animation.frames[0].time == 3);  // Inherited from frametime.
    CHECK(animation.frames[1].index == 2);
    CHECK(animation.frames[1].time == 7);

    // ⚠️ The limitation this pins: the atlas holds the first frame of the
    // sequence and nothing else. Playing the animation means re-uploading the
    // rect on a tick, which belongs to the upload path and does not exist yet.
    CHECK(sprite.width == 16);
    CHECK(sprite.height == 16);
    CHECK(texel(atlas.mip(0), sprite.x, sprite.y) == kRed);
}

TEST_CASE("an animation with no frame list plays every cell in order", "[atlas]") {
    std::vector<u8> strip;
    for (const Rgba colour : {kRed, kGreen, kBlue}) {
        const auto frame = solid(16, 16, colour);
        strip.insert(strip.end(), frame.begin(), frame.end());
    }

    MemoryAssetSource source;
    add_texture(source, "block/fire", 16, 48, strip);
    add_mcmeta(source, "block/fire", R"({"animation": {}})");

    AtlasBuilder builder(source);
    const auto   atlas     = builder.add("block/fire").build().value();
    const auto&  animation = sprite_of(atlas, "minecraft:block/fire").animation;

    REQUIRE(animation.has_value());
    REQUIRE(animation->frames.size() == 3);
    CHECK(animation->frames[0].index == 0);
    CHECK(animation->frames[1].index == 1);
    CHECK(animation->frames[2].index == 2);
    CHECK(animation->default_frame_time == 1);
    CHECK_FALSE(animation->interpolate);
}

TEST_CASE("an explicit frame size splits a texture into a grid", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/grid", 32, 32, solid(32, 32, kGreen));
    add_mcmeta(source, "block/grid",
               R"({"animation": {"width": 16, "height": 16, "frametime": 2}})");

    AtlasBuilder builder(source);
    const auto   atlas  = builder.add("block/grid").build().value();
    const auto&  sprite = sprite_of(atlas, "minecraft:block/grid");

    REQUIRE(sprite.animation.has_value());
    CHECK(sprite.animation->frame_width == 16);
    CHECK(sprite.animation->frames.size() == 4);  // Two columns, two rows.
    CHECK(sprite.animation->frames[1].time == 2);
    CHECK(sprite.width == 16);
}

TEST_CASE("a .mcmeta without an animation leaves the texture whole", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/tall", 16, 48, solid(16, 48, kBlue));
    // A real one: several vanilla textures carry only a `texture` block.
    add_mcmeta(source, "block/tall", R"({"texture": {"blur": false, "clamp": true}})");

    AtlasBuilder builder(source);
    const auto   atlas  = builder.add("block/tall").build().value();
    const auto&  sprite = sprite_of(atlas, "minecraft:block/tall");

    CHECK_FALSE(sprite.animation.has_value());
    CHECK(sprite.width == 16);
    CHECK(sprite.height == 48);
}

TEST_CASE("malformed .mcmeta does not lose the texture", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));
    add_mcmeta(source, "block/red", "{this is not json");

    AtlasBuilder builder(source);
    const auto   atlas  = builder.add("block/red").build().value();
    const auto&  sprite = sprite_of(atlas, "minecraft:block/red");

    CHECK_FALSE(sprite.missing);
    CHECK_FALSE(sprite.animation.has_value());
    CHECK(texel(atlas.mip(0), sprite.x, sprite.y) == kRed);
}

TEST_CASE("stitching the same set twice gives the same atlas", "[atlas]") {
    MemoryAssetSource source;
    add_texture(source, "block/red", 16, 16, solid(16, 16, kRed));
    add_texture(source, "block/green", 16, 16, solid(16, 16, kGreen));
    add_texture(source, "block/blue", 32, 32, solid(32, 32, kBlue));

    AtlasBuilder forwards(source);
    forwards.add("block/red").add("block/green").add("block/blue");

    AtlasBuilder backwards(source);
    backwards.add("block/blue").add("block/green").add("block/red");

    const auto first  = forwards.build().value();
    const auto second = backwards.build().value();

    // Insertion order must not reach the result: an atlas that reshuffles
    // between runs makes every rendering diff unreadable.
    CHECK(first.width() == second.width());
    CHECK(first.mip(0).rgba == second.mip(0).rgba);
    REQUIRE(first.sprites().size() == second.sprites().size());
    for (usize i = 0; i < first.sprites().size(); ++i) {
        CHECK(first.sprites()[i].name == second.sprites()[i].name);
        CHECK(first.sprites()[i].x == second.sprites()[i].x);
        CHECK(first.sprites()[i].y == second.sprites()[i].y);
    }
}

TEST_CASE("every atlas error has a message", "[atlas]") {
    CHECK_FALSE(to_string(AtlasError::TooLarge).empty());
}
