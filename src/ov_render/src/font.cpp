#define OV_LOG_CATEGORY "render"

#include "ov/render/font.hpp"

#include "ov/base/log.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::render {

namespace {

/// `minecraft:default` -> `assets/minecraft/font/default.json`.
[[nodiscard]] std::string font_asset_path(const ResourceLocation& location) {
    std::string path = "assets/";
    path += location.name_space();
    path += "/font/";
    path += location.path();
    path += ".json";
    return path;
}

/// The section sign, in UTF-8. `§` is U+00A7, two bytes, and writing it as a
/// literal in a source file is how an encoding accident becomes a silent
/// failure to parse any formatting code at all.
constexpr char kSectionFirst  = static_cast<char>(0xC2);
constexpr char kSectionSecond = static_cast<char>(0xA7);

/// Vanilla's sixteen colours, in `§0`..`§f` order.
///
/// Taken from the chat colour table of the protocol documentation for this
/// version. They are also the ones the client hard-codes, so they are not a
/// resource-pack value and cannot be read out of the assets.
constexpr std::array<u32, 16> kColours{
    0x000000,  // 0 black
    0x0000AA,  // 1 dark_blue
    0x00AA00,  // 2 dark_green
    0x00AAAA,  // 3 dark_aqua
    0xAA0000,  // 4 dark_red
    0xAA00AA,  // 5 dark_purple
    0xFFAA00,  // 6 gold
    0xAAAAAA,  // 7 gray
    0x555555,  // 8 dark_gray
    0x5555FF,  // 9 blue
    0x55FF55,  // a green
    0x55FFFF,  // b aqua
    0xFF5555,  // c red
    0xFF55FF,  // d light_purple
    0xFFFF55,  // e yellow
    0xFFFFFF,  // f white
};

struct Loader {
    const AssetSource*     source;
    std::vector<FontPage>* pages;
    std::unordered_map<char32_t, Glyph>* glyphs;

    [[nodiscard]] std::expected<void, FontError> load(const ResourceLocation& font, u32 depth);
    [[nodiscard]] std::expected<void, FontError> bitmap(const json::Value& provider);
    [[nodiscard]] std::expected<void, FontError> space(const json::Value& provider);
};

/// Vanilla's own measurement, and the only place a glyph's width comes from.
///
/// The rightmost column of the cell that has any alpha at all, plus one. A
/// blank cell gives zero, and the caller turns that into an advance of one —
/// which is what a bitmap provider does with a codepoint whose art is empty.
[[nodiscard]] u32 opaque_columns(const TextureImage& image, u32 x0, u32 y0, u32 cell_width,
                                 u32 cell_height) noexcept {
    for (u32 x = cell_width; x-- > 0;) {
        for (u32 y = 0; y < cell_height; ++y) {
            if (image.rgba[image.index(x0 + x, y0 + y) + 3] != 0) {
                return x + 1;
            }
        }
    }
    return 0;
}

std::expected<void, FontError> Loader::space(const json::Value& provider) {
    const json::Value advances = provider["advances"];
    if (!advances.is_object()) {
        return std::unexpected(FontError::Malformed);
    }
    for (u32 i = 0; i < advances.size(); ++i) {
        const std::string_view key = advances.key_at(i);
        usize                  offset = 0;
        const char32_t         codepoint = next_codepoint(key, offset);
        if (glyphs->contains(codepoint)) {
            continue;
        }
        Glyph glyph;
        glyph.advance = static_cast<f32>(advances.value_at(i).as_number());
        // No page, no rect: a space provider contributes an advance and no
        // pixels, and drawing it as a zero-area quad would be a wasted vertex
        // six times a line.
        glyph.width  = 0.0F;
        glyph.height = 0.0F;
        glyphs->emplace(codepoint, glyph);
    }
    return {};
}

std::expected<void, FontError> Loader::bitmap(const json::Value& provider) {
    const auto file = ResourceLocation::parse(provider["file"].as_string());
    if (!file) {
        return std::unexpected(FontError::Malformed);
    }

    const json::Value rows = provider["chars"];
    if (!rows.is_array() || rows.size() == 0) {
        return std::unexpected(FontError::Malformed);
    }

    // Rows are UTF-8, so "the same length" is a count of codepoints, not of
    // bytes. `nonlatin_european.png` has sixteen codepoints a row and most of
    // them are two or three bytes each, so comparing byte lengths would reject
    // every vanilla font there is.
    std::vector<std::vector<char32_t>> grid;
    grid.reserve(rows.size());
    for (u32 r = 0; r < rows.size(); ++r) {
        const std::string_view    row = rows[r].as_string();
        std::vector<char32_t>     line;
        usize                     offset = 0;
        while (offset < row.size()) {
            line.push_back(next_codepoint(row, offset));
        }
        grid.push_back(std::move(line));
    }
    const usize columns = grid.front().size();
    if (columns == 0) {
        return std::unexpected(FontError::BadGrid);
    }
    for (const auto& line : grid) {
        if (line.size() != columns) {
            return std::unexpected(FontError::BadGrid);
        }
    }

    // The page may already be loaded: `accented.png` and `ascii.png` are one
    // provider each, but a pack is free to draw two providers from one file.
    u16 page_index = 0;
    bool found     = false;
    for (u16 i = 0; i < static_cast<u16>(pages->size()); ++i) {
        if ((*pages)[i].name == file->full()) {
            page_index = i;
            found      = true;
            break;
        }
    }
    if (!found) {
        // `file` carries its own extension, so the path is built rather than
        // derived: texture_asset_path would produce `ascii.png.png`.
        std::string path = "assets/";
        path += file->name_space();
        path += "/textures/";
        path += file->path();
        auto image = load_texture_path(*source, path);
        if (!image) {
            OV_LOG_WARN("font page {}: {}", file->full(), to_string(image.error()));
            return std::unexpected(FontError::NotFound);
        }
        page_index = static_cast<u16>(pages->size());
        pages->push_back(FontPage{file->full(), std::move(*image)});
    }

    const TextureImage& image = (*pages)[page_index].image;
    if (image.width % columns != 0 || image.height % grid.size() != 0) {
        return std::unexpected(FontError::BadGrid);
    }
    const u32 cell_width  = image.width / static_cast<u32>(columns);
    const u32 cell_height = image.height / static_cast<u32>(grid.size());
    if (cell_width == 0 || cell_height == 0) {
        return std::unexpected(FontError::BadGrid);
    }

    const auto height = static_cast<f32>(provider["height"].as_number(8.0));
    const auto ascent = static_cast<f32>(provider["ascent"].as_number(0.0));
    // What one source texel is worth in GUI pixels. Faithful's ascii.png is
    // 256², vanilla's is 128², and this one ratio is the whole of what makes
    // the two produce the same advances.
    const f32 scale = height / static_cast<f32>(cell_height);

    for (u32 r = 0; r < grid.size(); ++r) {
        for (u32 c = 0; c < columns; ++c) {
            const char32_t codepoint = grid[r][c];
            // U+0000 is the format's own "nothing here" filler.
            if (codepoint == 0 || glyphs->contains(codepoint)) {
                continue;
            }
            const u32 x0 = c * cell_width;
            const u32 y0 = r * cell_height;
            const u32 m  = opaque_columns(image, x0, y0, cell_width, cell_height);

            Glyph glyph;
            glyph.page = page_index;
            glyph.u0   = static_cast<f32>(x0) / static_cast<f32>(image.width);
            glyph.v0   = static_cast<f32>(y0) / static_cast<f32>(image.height);
            glyph.u1   = static_cast<f32>(x0 + cell_width) / static_cast<f32>(image.width);
            glyph.v1   = static_cast<f32>(y0 + cell_height) / static_cast<f32>(image.height);
            // Vanilla's rounding, and it is a rounding rather than a ceiling:
            // (int)(0.5 + m * scale) + 1. The +1 is the column of spacing
            // between two glyphs, which is why every advance here is one more
            // than the ink is wide.
            glyph.advance = std::floor(0.5F + static_cast<f32>(m) * scale) + 1.0F;
            glyph.width   = static_cast<f32>(cell_width) * scale;
            glyph.height  = height;
            glyph.ascent  = ascent;
            glyphs->emplace(codepoint, glyph);
        }
    }
    return {};
}

std::expected<void, FontError> Loader::load(const ResourceLocation& font, u32 depth) {
    if (depth >= Font::kMaxReferenceDepth) {
        return std::unexpected(FontError::ReferenceLoop);
    }
    const auto bytes = source->read(font_asset_path(font));
    if (!bytes) {
        return std::unexpected(FontError::NotFound);
    }
    const auto document = json::Document::parse(*bytes);
    if (!document) {
        return std::unexpected(FontError::Malformed);
    }
    const json::Value providers = document->root()["providers"];
    if (!providers.is_array()) {
        return std::unexpected(FontError::Malformed);
    }

    for (u32 i = 0; i < providers.size(); ++i) {
        const json::Value      provider = providers[i];
        const std::string_view type     = provider["type"].as_string();
        if (type == "reference") {
            const auto referenced = ResourceLocation::parse(provider["id"].as_string());
            if (!referenced) {
                return std::unexpected(FontError::Malformed);
            }
            if (auto result = load(*referenced, depth + 1); !result) {
                return result;
            }
        } else if (type == "bitmap") {
            if (auto result = bitmap(provider); !result) {
                return result;
            }
        } else if (type == "space") {
            if (auto result = space(provider); !result) {
                return result;
            }
        } else {
            // Refused and named. `unihex` is the one this will meet first: it
            // is what a 1.20 pack uses for CJK, and 1.20.1's own jar happens
            // to ship an empty provider list for it because the unifont data
            // is downloaded into the assets object store instead.
            OV_LOG_ERROR("font {}: provider type '{}' is not implemented", font.full(), type);
            return std::unexpected(FontError::UnsupportedProvider);
        }
    }
    return {};
}

}  // namespace

std::string_view to_string(FontError error) noexcept {
    switch (error) {
        case FontError::NotFound:
            return "the font definition or one of its pages is missing";
        case FontError::Malformed:
            return "the font definition is not shaped like one";
        case FontError::UnsupportedProvider:
            return "the font uses a provider type this loader does not implement";
        case FontError::ReferenceLoop:
            return "the font's references nest too deeply, or loop";
        case FontError::BadGrid:
            return "a bitmap provider's rows do not divide its texture evenly";
    }
    return "unknown";
}

u32 formatting_colour(char code) noexcept {
    if (code >= '0' && code <= '9') {
        return kColours[static_cast<usize>(code - '0')];
    }
    const char lower = (code >= 'A' && code <= 'F') ? static_cast<char>(code + ('a' - 'A')) : code;
    if (lower >= 'a' && lower <= 'f') {
        return kColours[static_cast<usize>(lower - 'a') + 10];
    }
    return 0xFFFFFF;
}

void TextStyle::apply(char code) noexcept {
    const char lower = (code >= 'A' && code <= 'Z') ? static_cast<char>(code + ('a' - 'A')) : code;
    switch (lower) {
        case 'k':
            obfuscated = true;
            return;
        case 'l':
            bold = true;
            return;
        case 'm':
            strikethrough = true;
            return;
        case 'n':
            underline = true;
            return;
        case 'o':
            italic = true;
            return;
        case 'r':
            *this = TextStyle{};
            return;
        default:
            break;
    }
    if ((lower >= '0' && lower <= '9') || (lower >= 'a' && lower <= 'f')) {
        // A colour code resets every decoration, which is the rule that makes
        // "§lbold §rplain" and "§lbold §fplain" behave the same.
        const u32 chosen = formatting_colour(lower);
        *this            = TextStyle{};
        colour           = chosen;
        has_colour       = true;
    }
}

char32_t next_codepoint(std::string_view text, usize& offset) noexcept {
    if (offset >= text.size()) {
        return 0;
    }
    const auto first = static_cast<u8>(text[offset]);
    const auto trail = [&](usize count) -> char32_t {
        if (offset + count >= text.size()) {
            return 0xFFFD;
        }
        char32_t value = first & (0xFFU >> (count + 2));
        for (usize i = 1; i <= count; ++i) {
            const auto byte = static_cast<u8>(text[offset + i]);
            if ((byte & 0xC0U) != 0x80U) {
                return 0xFFFD;
            }
            value = (value << 6) | (byte & 0x3FU);
        }
        offset += count + 1;
        return value;
    };

    if (first < 0x80U) {
        ++offset;
        return first;
    }
    if ((first & 0xE0U) == 0xC0U) {
        const usize    start = offset;
        const char32_t value = trail(1);
        if (offset == start) {
            ++offset;
        }
        return value;
    }
    if ((first & 0xF0U) == 0xE0U) {
        const usize    start = offset;
        const char32_t value = trail(2);
        if (offset == start) {
            ++offset;
        }
        return value;
    }
    if ((first & 0xF8U) == 0xF0U) {
        const usize    start = offset;
        const char32_t value = trail(3);
        if (offset == start) {
            ++offset;
        }
        return value;
    }
    ++offset;
    return 0xFFFD;
}

void append_utf8(std::string& out, char32_t codepoint) {
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

std::expected<Font, FontError> Font::load(const AssetSource&      source,
                                          const ResourceLocation& font) {
    Font   self;
    Loader loader{&source, &self.pages_, &self.glyphs_};
    if (auto result = loader.load(font, 0); !result) {
        return std::unexpected(result.error());
    }
    OV_LOG_INFO("font {}: {} glyphs across {} page(s)", font.full(), self.glyphs_.size(),
                self.pages_.size());
    return self;
}

std::expected<Font, FontError> Font::load_default(const AssetSource& source) {
    const auto location = ResourceLocation::parse("minecraft:default");
    if (!location) {
        return std::unexpected(FontError::Malformed);
    }
    return load(source, *location);
}

const Glyph* Font::glyph(char32_t codepoint) const noexcept {
    const auto found = glyphs_.find(codepoint);
    return found == glyphs_.end() ? nullptr : &found->second;
}

f32 Font::advance(char32_t codepoint, bool bold) const noexcept {
    const Glyph* found = glyph(codepoint);
    if (found == nullptr) {
        return 0.0F;
    }
    return found->advance + (bold ? kBoldExtraAdvance : 0.0F);
}

f32 Font::width(std::string_view text) const noexcept {
    f32       total = 0.0F;
    TextStyle style;
    usize     offset = 0;
    while (offset < text.size()) {
        if (text[offset] == kSectionFirst && offset + 2 < text.size() &&
            text[offset + 1] == kSectionSecond) {
            style.apply(text[offset + 2]);
            offset += 3;
            continue;
        }
        total += advance(next_codepoint(text, offset), style.bold);
    }
    return total;
}

usize Font::fit(std::string_view text, f32 limit) const noexcept {
    f32       total = 0.0F;
    TextStyle style;
    usize     offset = 0;
    while (offset < text.size()) {
        if (text[offset] == kSectionFirst && offset + 2 < text.size() &&
            text[offset + 1] == kSectionSecond) {
            style.apply(text[offset + 2]);
            offset += 3;
            continue;
        }
        const usize    before    = offset;
        const char32_t codepoint = next_codepoint(text, offset);
        total += advance(codepoint, style.bold);
        if (total > limit) {
            return before;
        }
    }
    return text.size();
}

std::vector<char32_t> Font::codepoints() const {
    std::vector<char32_t> out;
    out.reserve(glyphs_.size());
    for (const auto& [codepoint, glyph] : glyphs_) {
        (void)glyph;
        out.push_back(codepoint);
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace ov::render
