// Chat components, as the vanilla server writes them.
//
// A client localises what it is sent: the server sends
// `{"translate":"commands.time.set","with":["1000"]}` and the client prints
// "Set the time to 1000" in whatever language it is set to. So feedback is
// built as translatable components with Mojang's exact keys, never as English.
//
// The JSON is written in **the order the vanilla jar writes it**, which was
// read off its own packets (`scripts/capture_commands.py`), not guessed:
//
//     style   bold, italic, underlined, strikethrough, obfuscated, color,
//             insertion, clickEvent, hoverEvent, font
//     then    "extra"
//     then    the content — "text", or "translate" followed by "with"
//
// `{"color":"red","extra":[…],"text":""}` is an error line, and its key order
// is the one thing a byte comparison with vanilla would trip on first.
//
// Nothing here is a public header; it lives in src/ and nothing outside
// ov_server includes it.
#pragma once

#include "json.hpp"

#include "ov/base/types.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server::cmd {

struct Text;

struct ClickEvent {
    std::string action;
    std::string value;
};

struct HoverEvent {
    enum class Action : u8 { ShowText, ShowItem, ShowEntity };
    Action action{Action::ShowText};
    /// ShowText: the one component shown. ShowEntity: its name, if any.
    std::vector<Text> text;
    /// ShowItem: the item id. ShowEntity: the entity's uuid.
    std::string id;
    /// ShowEntity: the entity type.
    std::string entity_type;
    /// ShowItem: omitted from the JSON when 1, as vanilla does.
    i32 count{1};
    /// ShowItem: the stack's NBT as SNBT.
    std::optional<std::string> tag;
};

struct Style {
    std::optional<bool>        bold;
    std::optional<bool>        italic;
    std::optional<bool>        underlined;
    std::optional<bool>        strikethrough;
    std::optional<bool>        obfuscated;
    std::optional<std::string> color;
    std::optional<std::string> insertion;
    std::optional<ClickEvent>  click;
    /// Zero or one. A vector only because `HoverEvent` holds `Text`.
    std::vector<HoverEvent>    hover;
    std::optional<std::string> font;

    [[nodiscard]] bool empty() const noexcept;
};

struct Text {
    enum class Kind : u8 {
        /// {"text": …}
        Literal,
        /// {"translate": …, "with": […]}
        Translatable,
        /// {"keybind": …}
        Keybind,
        /// A bare JSON string. Vanilla writes a translation argument that is a
        /// plain string, or a plain unstyled literal, this way.
        Raw,
        /// {"selector": …}, resolved to names before it is sent.
        Selector,
        /// {"score": …} and {"nbt": …}: resolved before sending, to nothing on
        /// this server — there is no scoreboard and no NBT source to read.
        Score,
        Nbt,
    };

    Kind        kind{Kind::Literal};
    std::string text;
    std::string key;
    std::optional<std::string> fallback;
    std::vector<Text> with;
    Style             style;
    std::vector<Text> extra;

    [[nodiscard]] static Text literal(std::string text);
    [[nodiscard]] static Text translatable(std::string key, std::vector<Text> with = {});
    [[nodiscard]] static Text raw(std::string text);

    /// `Component.empty().append(inner)`: what vanilla wraps a styled line in.
    [[nodiscard]] static Text wrap(Text inner);

    Text& append(Text child) {
        extra.push_back(std::move(child));
        return *this;
    }
    Text& color(std::string name) {
        style.color = std::move(name);
        return *this;
    }
};

/// A translation table, for the one place the server has to render English
/// itself: the console of a dedicated server. Loaded from the resource pack
/// the player's client uses (`run/assets/.../lang/en_us.json`), never
/// committed; without it the console prints keys, which is still readable.
class Lang {
public:
    [[nodiscard]] static std::optional<Lang> load(const std::filesystem::path& path);

    [[nodiscard]] const std::string* find(std::string_view key) const;
    [[nodiscard]] bool               has(std::string_view key) const { return find(key) != nullptr; }

    void set(std::string key, std::string value) { entries_[std::move(key)] = std::move(value); }

private:
    std::unordered_map<std::string, std::string> entries_;
};

/// The component as JSON, byte for byte the way the vanilla server writes it.
[[nodiscard]] std::string to_json(const Text& text);

/// A component read from JSON, the way vanilla's deserializer reads it: a
/// string is a literal, an array is its first element with the rest appended,
/// an unknown colour is dropped. The error is the message vanilla puts in
/// `argument.component.invalid`.
[[nodiscard]] std::expected<Text, std::string> text_from_json(const JsonValue& json);

/// The component as plain text, translated through `lang` when one is given.
[[nodiscard]] std::string plain(const Text& text, const Lang* lang);

/// Append `value` as a JSON string literal, escaped the way Gson escapes it
/// with HTML escaping off: quotes, backslashes and control characters.
void append_json_string(std::string& out, std::string_view value);

/// Java's `Float.toString` / `Double.toString`: shortest digits, ".0" on whole
/// numbers, scientific notation outside [1e-3, 1e7). "90.0", "0.5", "1.0E7".
[[nodiscard]] std::string java_float_string(f32 value);
[[nodiscard]] std::string java_double_string(f64 value);

/// Java's `String.format("%f")`: six decimals.
[[nodiscard]] std::string java_fixed6(f64 value);

}  // namespace ov::server::cmd
