// Chat components, flattened into the one kind of string the GUI draws.
//
// A tooltip line in 1.20.1 is a *component*: a tree of text, translation keys
// with arguments, and styles (`{"translate":"item.minecraft.potion.effect.
// night_vision","color":"aqua"}`). The GUI's text path already understands
// vanilla's legacy `§` codes — colour, bold, italic, underline, strikethrough
// — so the cheapest faithful rendering is to walk the tree once, translate it
// with the active language, and emit a `§`-coded UTF-8 line. The tree is never
// kept: a tooltip is built when the catalogue loads, not in a frame.
//
// What is refused and named rather than approximated silently:
//   • `score`, `selector` and `nbt` components are drawn as nothing — they
//     need a server to resolve and no creative tooltip uses them;
//   • a hex colour (`#RRGGBB`) has no legacy code, so it is drawn with the
//     nearest of the sixteen named colours. No 1.20.1 creative tooltip uses
//     one; the fallback exists so a resource pack cannot make a line vanish.
#pragma once

#include "ov/base/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

class Language;

// ── Styled runs: the same walk, keeping exact colours ───────────────────────
//
// A chat line cannot go through flatten_component: `/tellraw` sends `#3080ff`
// and a `§` string can only say one of sixteen colours. So chat keeps the
// component as runs — a stretch of text and the style it is drawn in — with
// the colour as the number the server sent. The walk is the one above: the
// same inheritance, the same translation rules, the same refusals.

/// One stretch of text in one style.
struct StyledRun {
    /// Plain UTF-8. A `§` that arrived inside the text is left in it, and the
    /// GUI's text path obeys it, as vanilla's font renderer does.
    std::string text;
    /// 0xRRGGBB, meaningful only when `has_colour`.
    u32  rgb{0xFFFFFF};
    bool has_colour{false};
    bool bold{false};
    bool italic{false};
    bool underlined{false};
    bool strikethrough{false};
    bool obfuscated{false};

    [[nodiscard]] bool same_style(const StyledRun& other) const noexcept {
        return rgb == other.rgb && has_colour == other.has_colour && bold == other.bold &&
               italic == other.italic && underlined == other.underlined &&
               strikethrough == other.strikethrough && obfuscated == other.obfuscated;
    }

    friend bool operator==(const StyledRun&, const StyledRun&) = default;
};

/// A component, given as its JSON text, as runs in reading order. Adjacent
/// runs of one style are merged. Malformed JSON is one unstyled run of its own
/// text, so a line this cannot read looks obviously wrong rather than empty.
[[nodiscard]] std::vector<StyledRun> component_runs(std::string_view json,
                                                    const Language& language);

/// The runs' text, joined.
[[nodiscard]] std::string plain_text(std::span<const StyledRun> runs);

/// A colour as a component names it: one of the sixteen, or `#RRGGBB`.
[[nodiscard]] std::optional<u32> colour_rgb(std::string_view name) noexcept;

/// Flatten a component, given as its JSON text, into a `§`-coded line.
///
/// Malformed JSON is returned as its own text, so a line this cannot read looks
/// obviously wrong rather than empty.
[[nodiscard]] std::string flatten_component(std::string_view json, const Language& language);

/// The same line with every `§` code removed: what vanilla's search indexes.
[[nodiscard]] std::string strip_formatting(std::string_view text);

/// The legacy formatting code of a named colour — `gray` is `7` — or 0 when the
/// name is not one of the sixteen.
[[nodiscard]] char colour_code(std::string_view name) noexcept;

/// Substitute `%s`, `%1$s` and `%%` in a translation pattern, as Java's
/// `String.format` does for the subset translations use. A missing argument
/// leaves the placeholder as written, which is also what vanilla shows.
[[nodiscard]] std::string format_translation(std::string_view              pattern,
                                             std::span<const std::string> arguments);

/// What a keybind component shows: the key bound to it by default in 1.20.1.
/// Only the bindings a creative tooltip names are listed; any other is shown
/// as its translated name.
[[nodiscard]] std::string_view default_key_name(std::string_view keybind) noexcept;

}  // namespace ov::render
