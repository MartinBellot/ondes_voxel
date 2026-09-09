#include "ov/render/font.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <filesystem>
#include <string>

using namespace ov;
using namespace ov::render;

namespace {

/// A 32×16 RGBA PNG: two 16×16 cells side by side, the first inked across
/// three columns, the second across six.
///
/// Hermetic on purpose. Everything about an advance is decided by two numbers
/// — the rightmost inked column and `height / cell_height` — and a fixture
/// whose ink is known exactly is the only way to test the second one without
/// a resource pack. Generated once and pasted, because nothing in this
/// repository may commit a Mojang texture and nothing in this module writes a
/// PNG.
constexpr std::array<u8, 94> kTwoGlyphPage{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x77, 0x00, 0x7D, 0x59, 0x00, 0x00, 0x00,
    0x25, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x60, 0x18, 0xF1, 0xE0,
    0x3F, 0x12, 0xA0, 0x54, 0x3F, 0x59, 0x66, 0x8D, 0x3A, 0x60, 0xD4, 0x01,
    0xA3, 0x0E, 0x18, 0x75, 0xC0, 0xA8, 0x03, 0x06, 0xDC, 0x01, 0x23, 0x1E,
    0x00, 0x00, 0x2E, 0x36, 0xAE, 0x60, 0xA7, 0x61, 0x13, 0x96, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

[[nodiscard]] MemoryAssetSource fixture_pack() {
    MemoryAssetSource pack;
    pack.add("assets/minecraft/font/fixture.json", R"({
        "providers": [
            {"type": "reference", "id": "minecraft:fixture_space"},
            {"type": "bitmap", "file": "minecraft:font/fixture.png",
             "ascent": 7, "height": 8, "chars": ["AB"]}
        ]
    })");
    pack.add("assets/minecraft/font/fixture_space.json", R"({
        "providers": [{"type": "space", "advances": {" ": 4, "‌": 0}}]
    })");
    pack.add("assets/minecraft/textures/font/fixture.png",
             std::string_view(reinterpret_cast<const char*>(kTwoGlyphPage.data()),
                              kTwoGlyphPage.size()));
    return pack;
}

[[nodiscard]] ResourceLocation location(std::string_view text) {
    auto parsed = ResourceLocation::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// The real pack, when it has been imported. Absent in CI, which has no assets
/// at all, so every test that needs it skips rather than fails.
[[nodiscard]] std::optional<DirectoryAssetSource> real_pack() {
    const std::filesystem::path assets("run/assets");
    if (!std::filesystem::is_directory(assets / "assets" / "minecraft" / "font")) {
        return std::nullopt;
    }
    return DirectoryAssetSource(assets);
}

}  // namespace

TEST_CASE("utf-8 decodes one codepoint at a time", "[font]") {
    // One byte, two, three and four: ASCII, the section sign, an arrow, an
    // emoji. The section sign matters most — every formatting code is found by
    // looking for its two bytes.
    const std::string text = "a§→\U0001F600";
    usize             offset = 0;
    CHECK(next_codepoint(text, offset) == U'a');
    CHECK(next_codepoint(text, offset) == U'§');
    CHECK(next_codepoint(text, offset) == U'→');
    CHECK(next_codepoint(text, offset) == U'\U0001F600');
    CHECK(offset == text.size());
}

TEST_CASE("a truncated utf-8 sequence yields the replacement and still advances",
          "[font]") {
    // A server can send anything. A decoder that does not advance on a bad
    // byte turns one malformed name into an infinite loop in the frame.
    const std::string text = "\xE2\x82";
    usize             offset = 0;
    CHECK(next_codepoint(text, offset) == 0xFFFD);
    CHECK(offset > 0);
}

TEST_CASE("utf-8 round-trips through append", "[font]") {
    for (const char32_t codepoint : {U'a', U'§', U'→', U'\U0001F600'}) {
        std::string encoded;
        append_utf8(encoded, codepoint);
        usize offset = 0;
        CHECK(next_codepoint(encoded, offset) == codepoint);
        CHECK(offset == encoded.size());
    }
}

TEST_CASE("formatting codes carry colour and decoration", "[font]") {
    TextStyle style;
    style.apply('l');
    CHECK(style.bold);
    style.apply('o');
    CHECK(style.italic);

    // A colour resets the decorations. That is the rule that makes
    // "§lbold §fplain" plain rather than bold.
    style.apply('c');
    CHECK(style.colour == 0xFF5555);
    CHECK(style.has_colour);
    CHECK_FALSE(style.bold);
    CHECK_FALSE(style.italic);

    style.apply('r');
    CHECK_FALSE(style.has_colour);
    CHECK(style.colour == 0xFFFFFF);

    // Upper case is the same code. Vanilla accepts §C.
    TextStyle upper;
    upper.apply('C');
    CHECK(upper.colour == 0xFF5555);
}

TEST_CASE("an advance is the inked width plus one, scaled", "[font]") {
    const auto pack = fixture_pack();
    const auto font = Font::load(pack, location("minecraft:fixture"));
    REQUIRE(font.has_value());

    // The page is 16 pixels a cell and the provider asks for 8, so one source
    // texel is half a GUI pixel. Three inked columns -> floor(0.5 + 1.5) + 1,
    // six -> floor(0.5 + 3) + 1.
    CHECK(font->advance(U'A') == 3.0F);
    CHECK(font->advance(U'B') == 4.0F);

    // The space provider contributes an advance and no pixels.
    CHECK(font->advance(U' ') == 4.0F);
    REQUIRE(font->glyph(U' ') != nullptr);
    CHECK(font->glyph(U' ')->width == 0.0F);

    // A codepoint nobody drew is missing, not blank.
    CHECK(font->glyph(U'Z') == nullptr);
    CHECK(font->advance(U'Z') == 0.0F);

    // The glyph quad is the cell, scaled to the declared height.
    REQUIRE(font->glyph(U'A') != nullptr);
    CHECK(font->glyph(U'A')->height == 8.0F);
    CHECK(font->glyph(U'A')->width == 8.0F);
    CHECK(font->glyph(U'A')->ascent == 7.0F);
}

TEST_CASE("bold costs one pixel, and a width ignores the codes it obeys", "[font]") {
    const auto pack = fixture_pack();
    const auto font = Font::load(pack, location("minecraft:fixture"));
    REQUIRE(font.has_value());

    CHECK(font->width("AB") == 7.0F);
    CHECK(font->width("§lAB") == 9.0F);
    // The two bytes of the section sign and the code itself take no room.
    CHECK(font->width("§rAB") == 7.0F);
    CHECK(font->advance(U'A', true) == 4.0F);
}

TEST_CASE("an unimplemented provider type is refused by name", "[font]") {
    MemoryAssetSource pack;
    pack.add("assets/minecraft/font/unihex.json", R"({
        "providers": [{"type": "unihex", "hex_file": "minecraft:font/unifont.zip"}]
    })");
    const auto font = Font::load(pack, location("minecraft:unihex"));
    REQUIRE_FALSE(font.has_value());
    CHECK(font.error() == FontError::UnsupportedProvider);
}

TEST_CASE("a reference loop is refused rather than followed", "[font]") {
    MemoryAssetSource pack;
    pack.add("assets/minecraft/font/loop.json",
             R"({"providers": [{"type": "reference", "id": "minecraft:loop"}]})");
    const auto font = Font::load(pack, location("minecraft:loop"));
    REQUIRE_FALSE(font.has_value());
    CHECK(font.error() == FontError::ReferenceLoop);
}

TEST_CASE("the real default font has the advances the game draws with", "[font]") {
    const auto pack = real_pack();
    if (!pack) {
        SKIP("run/assets is absent; run tools/ov_assetimport first");
    }
    const auto font = Font::load_default(*pack);
    REQUIRE(font.has_value());

    // Every one of these was read out of the pack's own pixels by
    // scripts/measure_font_widths.py, which walks the providers independently
    // of this code. They are also the widths the game has drawn since 1.7:
    // `i` two, `l` three, `t` four, `k` and `f` five, everything else six.
    struct Known {
        char32_t codepoint;
        f32      advance;
    };
    constexpr std::array kKnown{
        Known{U' ', 4.0F},  Known{U'!', 2.0F}, Known{U'i', 2.0F}, Known{U'l', 3.0F},
        Known{U't', 4.0F},  Known{U'k', 5.0F}, Known{U'f', 5.0F}, Known{U'a', 6.0F},
        Known{U'm', 6.0F},  Known{U'@', 7.0F}, Known{U'~', 7.0F}, Known{U'.', 2.0F},
        Known{U'é', 6.0F},
    };
    for (const auto& known : kKnown) {
        INFO("codepoint U+" << static_cast<u32>(known.codepoint));
        CHECK(font->advance(known.codepoint) == known.advance);
    }

    // "Hello, world!" is the sum of its parts, and nothing else.
    f32 expected = 0.0F;
    for (const char32_t codepoint : std::u32string_view(U"Hello, world!")) {
        expected += font->advance(codepoint);
    }
    CHECK(font->width("Hello, world!") == expected);

    // 1.20.1's jar has no CJK: `font/include/unifont.json` is an empty
    // provider list and the unifont data is downloaded separately. Stated as a
    // test so that the day it changes, this is what says so.
    CHECK(font->glyph(U'一') == nullptr);
    CHECK(font->glyph_count() > 2000);
}

TEST_CASE("fit trims a string to a pixel budget", "[font]") {
    const auto pack = fixture_pack();
    const auto font = Font::load(pack, location("minecraft:fixture"));
    REQUIRE(font.has_value());

    // "AB" is 3 + 4. Seven fits, six does not.
    CHECK(font->fit("AB", 7.0F) == 2);
    CHECK(font->fit("AB", 6.0F) == 1);
    CHECK(font->fit("AB", 2.0F) == 0);
}
