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

#include <span>
#include <string>
#include <string_view>

namespace ov::render {

class Language;

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
