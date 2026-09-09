// The bitmap font: 1.20.1's `font/default.json`, resolved into glyphs.
//
// Without text there is no interface. A hotbar can be drawn from sprites, but
// an item count, a level number, a container title and a chat line cannot, and
// every one of them is part of what makes the game legible. So this comes
// first.
//
// The whole of the format is data, which is why it lives here and not in
// ov_client: a font is a JSON file naming providers, each of which contributes
// codepoints. 1.20.1's default font is three references — a `space` provider
// for U+0020 and U+200C, and two `bitmap` providers, `nonlatin_european.png`
// and `ascii.png`, plus `accented.png` — resolving to 2414 glyphs.
//
// ⚠️ **An advance is not declared, it is measured.** The JSON says where a
// glyph is, never how wide it is. Vanilla scans the glyph's own pixels for the
// rightmost column that is not transparent and takes that, plus one column of
// spacing, scaled by `height / cell_height`. That is why `i` is two pixels wide
// and `m` is six, and why nothing here has a width table in it: a width table
// would be a copy of an answer the pixels already give, and it would be wrong
// the moment a resource pack redraws a glyph. See docs/provenance/interface.md
// for the comparison of the vanilla jar's own answer with Faithful 32x's —
// they differ on 51 of 2414 glyphs, none of them below U+0100.
//
// ⚠️ **1.20.1's jar has no CJK.** `font/include/unifont.json` in the jar is an
// empty provider list; the unifont data is downloaded into the assets object
// store separately and is not part of the pack. So a font loaded from the jar
// covers Latin, Greek, Cyrillic, Hebrew, Armenian and a pile of symbols, and
// stops there. A codepoint with no glyph is reported as missing rather than
// silently drawn as a blank.
#pragma once

#include "ov/base/resource_location.hpp"
#include "ov/base/types.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/texture_image.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::render {

enum class FontError : u8 {
    /// The font definition, or a file it references, is not in the pack stack.
    NotFound,
    /// The JSON is not a font definition.
    Malformed,
    /// A provider type this loader does not implement. Named rather than
    /// skipped: a font missing half its glyphs because a provider was ignored
    /// looks like a broken renderer, not like a missing feature.
    UnsupportedProvider,
    /// `reference` providers nested deeper than kMaxReferenceDepth, or a loop.
    ReferenceLoop,
    /// A `bitmap` provider whose `chars` rows are not all the same length, or
    /// whose texture does not divide into that grid.
    BadGrid,
};

[[nodiscard]] std::string_view to_string(FontError error) noexcept;

/// One glyph's place in a page, and what it costs to draw.
///
/// Everything is in GUI pixels — the unit the interface is laid out in, before
/// the GUI scale multiplies it. A glyph of the default font is 8 GUI pixels
/// tall whatever the resolution of the pack that drew it, which is exactly
/// what `height / cell_height` scaling buys.
struct Glyph {
    /// Index into Font::pages(). A page is one PNG, uploaded whole.
    u16 page{0};
    /// The glyph's rect inside that page, normalised to 0..1, v downward.
    f32 u0{0.0F};
    f32 v0{0.0F};
    f32 u1{0.0F};
    f32 v1{0.0F};
    /// How far the pen moves after drawing it, spacing included.
    f32 advance{0.0F};
    /// The quad's size in GUI pixels.
    f32 width{0.0F};
    f32 height{0.0F};
    /// Distance from the top of the line to the baseline, in GUI pixels. 7 for
    /// the default font, which is what puts a descender below the line.
    f32 ascent{0.0F};
};

/// One PNG a provider drew from, decoded and ready to upload.
struct FontPage {
    /// The resource location it came from, for logs and for a stable order.
    std::string     name;
    TextureImage    image;
};

/// The sixteen colours `§0`..`§f` name, as 0xRRGGBB.
///
/// These are the *foreground* values. Vanilla draws the drop shadow in the
/// same colour with each channel divided by four, which is why the shadow of
/// white text is dark grey and not black.
[[nodiscard]] u32 formatting_colour(char code) noexcept;

/// What `§` codes are in force at a point in a string.
struct TextStyle {
    /// 0xRRGGBB, or the caller's own colour when no `§` code has set one.
    u32  colour{0xFFFFFF};
    bool has_colour{false};
    bool bold{false};
    bool italic{false};
    bool underline{false};
    bool strikethrough{false};
    /// ⚠️ Parsed and carried, never drawn. `§k` replaces each glyph with a
    /// random one of the same advance every frame; it needs a per-frame RNG
    /// the interface does not have yet, and a static replacement would be a
    /// different effect wearing its name.
    bool obfuscated{false};

    void apply(char code) noexcept;
};

/// The default font of 1.20.1, loaded from a resource pack stack.
class Font {
public:
    /// A `reference` provider chain deeper than this is a loop in practice.
    static constexpr u32 kMaxReferenceDepth = 8;

    /// Vanilla's line height: 8 pixels of glyph plus one of leading.
    static constexpr f32 kLineHeight = 9.0F;

    /// Where the baseline sits below the top of a line, in GUI pixels.
    ///
    /// It is 7 because the default font's own providers declare `ascent: 7`,
    /// and it is what lets `accented.png` — twelve pixels tall with an ascent
    /// of ten — hang its accents three pixels above the line and still put its
    /// letters on it. A glyph's top is `baseline - ascent`.
    static constexpr f32 kBaseline = 7.0F;

    /// The drop shadow's offset, in GUI pixels, right and down.
    static constexpr f32 kShadowOffset = 1.0F;

    /// Bold costs one extra pixel of advance, because vanilla draws the glyph
    /// twice, one pixel apart.
    static constexpr f32 kBoldExtraAdvance = 1.0F;

    [[nodiscard]] static std::expected<Font, FontError> load(
        const AssetSource& source, const ResourceLocation& font);

    /// `minecraft:default`, the font everything but a few signs uses.
    [[nodiscard]] static std::expected<Font, FontError> load_default(const AssetSource& source);

    /// Null when the font has no glyph for this codepoint.
    [[nodiscard]] const Glyph* glyph(char32_t codepoint) const noexcept;

    /// The pen advance of one codepoint, bold included. Zero for a codepoint
    /// with no glyph, which is what makes a missing glyph take no room rather
    /// than a random amount.
    [[nodiscard]] f32 advance(char32_t codepoint, bool bold = false) const noexcept;

    /// The width of a UTF-8 string in GUI pixels, `§` codes obeyed and not
    /// counted.
    [[nodiscard]] f32 width(std::string_view text) const noexcept;

    /// The longest prefix of `text` that fits in `limit` GUI pixels, in bytes.
    /// Used to trim a container title, never to wrap.
    [[nodiscard]] usize fit(std::string_view text, f32 limit) const noexcept;

    [[nodiscard]] const std::vector<FontPage>& pages() const noexcept { return pages_; }

    [[nodiscard]] usize glyph_count() const noexcept { return glyphs_.size(); }

    /// Every codepoint with a glyph, ascending. For the width oracle, which
    /// has to compare the whole table rather than a sample.
    [[nodiscard]] std::vector<char32_t> codepoints() const;

private:
    std::vector<FontPage>                pages_;
    std::unordered_map<char32_t, Glyph>  glyphs_;
};

/// Decode one UTF-8 codepoint at `offset`, advancing it.
///
/// Returns U+FFFD and advances by one on a malformed sequence: text arrives
/// from a server and a bad byte must not stop a line from being drawn.
[[nodiscard]] char32_t next_codepoint(std::string_view text, usize& offset) noexcept;

/// Encode one codepoint as UTF-8, appended to `out`.
void append_utf8(std::string& out, char32_t codepoint);

}  // namespace ov::render
