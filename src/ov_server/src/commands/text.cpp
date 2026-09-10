#include "text.hpp"

#include "json.hpp"

#include "ov/io/file.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <cstdio>

namespace ov::server::cmd {

bool Style::empty() const noexcept {
    return !bold && !italic && !underlined && !strikethrough && !obfuscated && !color &&
           !insertion && !click && hover.empty() && !font;
}

Text Text::literal(std::string text) {
    Text out;
    out.kind = Kind::Literal;
    out.text = std::move(text);
    return out;
}

Text Text::translatable(std::string key, std::vector<Text> with) {
    Text out;
    out.kind = Kind::Translatable;
    out.key  = std::move(key);
    out.with = std::move(with);
    return out;
}

Text Text::raw(std::string text) {
    Text out;
    out.kind = Kind::Raw;
    out.text = std::move(text);
    return out;
}

Text Text::wrap(Text inner) {
    Text out = literal("");
    out.extra.push_back(std::move(inner));
    return out;
}

void append_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (usize i = 0; i < value.size(); ++i) {
        const char c = value[i];
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < value.size() &&
                       static_cast<unsigned char>(value[i + 1]) == 0x80 &&
                       (static_cast<unsigned char>(value[i + 2]) == 0xA8 ||
                        static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
                // U+2028 and U+2029: Gson escapes the two line terminators
                // JavaScript refuses inside a string.
                out += static_cast<unsigned char>(value[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
                i += 2;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

namespace {

void write_text(std::string& out, const Text& text);

void write_bool_field(std::string& out, bool& first, std::string_view name,
                      const std::optional<bool>& value) {
    if (!value) {
        return;
    }
    out += first ? "" : ",";
    first = false;
    out.push_back('"');
    out += name;
    out += "\":";
    out += *value ? "true" : "false";
}

void write_string_field(std::string& out, bool& first, std::string_view name,
                        std::string_view value) {
    out += first ? "" : ",";
    first = false;
    out.push_back('"');
    out += name;
    out += "\":";
    append_json_string(out, value);
}

void write_hover(std::string& out, const HoverEvent& hover) {
    out += "{\"action\":";
    switch (hover.action) {
    case HoverEvent::Action::ShowText:
        out += "\"show_text\",\"contents\":";
        if (hover.text.empty()) {
            out += "{\"text\":\"\"}";
        } else {
            write_text(out, hover.text.front());
        }
        break;
    case HoverEvent::Action::ShowItem:
        out += "\"show_item\",\"contents\":{\"id\":";
        append_json_string(out, hover.id);
        if (hover.count != 1) {
            out += fmt::format(",\"count\":{}", hover.count);
        }
        if (hover.tag) {
            out += ",\"tag\":";
            append_json_string(out, *hover.tag);
        }
        out.push_back('}');
        break;
    case HoverEvent::Action::ShowEntity:
        out += "\"show_entity\",\"contents\":{\"type\":";
        append_json_string(out, hover.entity_type);
        out += ",\"id\":";
        append_json_string(out, hover.id);
        if (!hover.text.empty()) {
            out += ",\"name\":";
            write_text(out, hover.text.front());
        }
        out.push_back('}');
        break;
    }
    out.push_back('}');
}

void write_text(std::string& out, const Text& text) {
    if (text.kind == Text::Kind::Raw) {
        append_json_string(out, text.text);
        return;
    }
    out.push_back('{');
    bool first = true;
    const Style& style = text.style;
    write_bool_field(out, first, "bold", style.bold);
    write_bool_field(out, first, "italic", style.italic);
    write_bool_field(out, first, "underlined", style.underlined);
    write_bool_field(out, first, "strikethrough", style.strikethrough);
    write_bool_field(out, first, "obfuscated", style.obfuscated);
    if (style.color) {
        write_string_field(out, first, "color", *style.color);
    }
    if (style.insertion) {
        write_string_field(out, first, "insertion", *style.insertion);
    }
    if (style.click) {
        out += first ? "" : ",";
        first = false;
        out += "\"clickEvent\":{\"action\":";
        append_json_string(out, style.click->action);
        out += ",\"value\":";
        append_json_string(out, style.click->value);
        out.push_back('}');
    }
    if (!style.hover.empty()) {
        out += first ? "" : ",";
        first = false;
        out += "\"hoverEvent\":";
        write_hover(out, style.hover.front());
    }
    if (style.font) {
        write_string_field(out, first, "font", *style.font);
    }
    if (!text.extra.empty()) {
        out += first ? "" : ",";
        first = false;
        out += "\"extra\":[";
        for (usize i = 0; i < text.extra.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            write_text(out, text.extra[i]);
        }
        out.push_back(']');
    }
    switch (text.kind) {
    case Text::Kind::Literal:
        write_string_field(out, first, "text", text.text);
        break;
    case Text::Kind::Translatable:
        write_string_field(out, first, "translate", text.key);
        if (text.fallback) {
            write_string_field(out, first, "fallback", *text.fallback);
        }
        if (!text.with.empty()) {
            out += ",\"with\":[";
            for (usize i = 0; i < text.with.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                write_text(out, text.with[i]);
            }
            out.push_back(']');
        }
        break;
    case Text::Kind::Keybind:
        write_string_field(out, first, "keybind", text.text);
        break;
    case Text::Kind::Selector:
        // Only reached for a component nobody resolved; written as vanilla
        // writes an unresolved one, so that the omission is visible.
        write_string_field(out, first, "selector", text.text);
        break;
    case Text::Kind::Score:
    case Text::Kind::Nbt:
    case Text::Kind::Raw:
        write_string_field(out, first, "text", "");
        break;
    }
    out.push_back('}');
}

/// `String.format` with `%s` and `%1$s`, the only two forms the language
/// files use. A missing argument prints as nothing, as vanilla's does.
std::string format_translation(std::string_view pattern, const std::vector<std::string>& args) {
    std::string out;
    usize       next = 0;
    for (usize i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            out.push_back(pattern[i]);
            continue;
        }
        if (pattern[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }
        if (pattern[i + 1] == 's') {
            if (next < args.size()) {
                out += args[next];
            }
            ++next;
            ++i;
            continue;
        }
        // %1$s
        usize j     = i + 1;
        usize index = 0;
        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
            index = index * 10 + static_cast<usize>(pattern[j] - '0');
            ++j;
        }
        if (j + 1 < pattern.size() && pattern[j] == '$' && pattern[j + 1] == 's' && index > 0) {
            if (index - 1 < args.size()) {
                out += args[index - 1];
            }
            i = j + 1;
            continue;
        }
        out.push_back(pattern[i]);
    }
    return out;
}

void write_plain(std::string& out, const Text& text, const Lang* lang) {
    switch (text.kind) {
    case Text::Kind::Literal:
    case Text::Kind::Raw:
    case Text::Kind::Keybind:
    case Text::Kind::Selector:
        out += text.text;
        break;
    case Text::Kind::Score:
    case Text::Kind::Nbt:
        break;
    case Text::Kind::Translatable: {
        std::vector<std::string> args;
        args.reserve(text.with.size());
        for (const Text& arg : text.with) {
            args.push_back(plain(arg, lang));
        }
        const std::string* pattern = lang != nullptr ? lang->find(text.key) : nullptr;
        if (pattern != nullptr) {
            out += format_translation(*pattern, args);
        } else if (text.fallback) {
            out += format_translation(*text.fallback, args);
        } else {
            // No table: the key, then its arguments, which is still readable
            // and never silently drops what the command said.
            out += text.key;
            if (!args.empty()) {
                out += " [";
                for (usize i = 0; i < args.size(); ++i) {
                    out += i == 0 ? "" : ", ";
                    out += args[i];
                }
                out += "]";
            }
        }
        break;
    }
    }
    for (const Text& child : text.extra) {
        write_plain(out, child, lang);
    }
}

/// Java's shortest-digits algorithm, for the digits and the exponent. The
/// C++ shortest round trip is used for the digits; Java 17's own algorithm is
/// not always shortest, and the few values where the two disagree (1.0E23 is
/// the famous one) do not occur in anything a command prints.
template<typename T>
std::string java_number_string(T value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    if (value == 0) {
        return std::signbit(value) ? "-0.0" : "0.0";
    }
    std::array<char, 64> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
    std::string scientific{buffer.data(), result.ptr};
    // d[.ddd]e±XX
    const bool        negative = scientific.front() == '-';
    const std::string body     = negative ? scientific.substr(1) : scientific;
    const auto        e        = body.find('e');
    std::string       digits;
    for (usize i = 0; i < e; ++i) {
        if (body[i] != '.') {
            digits.push_back(body[i]);
        }
    }
    const int exponent = std::stoi(body.substr(e + 1));
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }
    std::string out = negative ? "-" : "";
    const T     magnitude = std::abs(value);
    if (magnitude >= static_cast<T>(1e-3) && magnitude < static_cast<T>(1e7)) {
        if (exponent >= 0) {
            const auto whole = static_cast<usize>(exponent) + 1;
            std::string integer_part = digits.substr(0, std::min(whole, digits.size()));
            while (integer_part.size() < whole) {
                integer_part.push_back('0');
            }
            std::string fraction = whole < digits.size() ? digits.substr(whole) : "0";
            out += integer_part + "." + fraction;
        } else {
            out += "0.";
            out += std::string(static_cast<usize>(-exponent - 1), '0');
            out += digits;
        }
        return out;
    }
    out += digits.substr(0, 1);
    out += ".";
    out += digits.size() > 1 ? digits.substr(1) : "0";
    out += "E" + std::to_string(exponent);
    return out;
}

constexpr std::array<std::string_view, 16> kColourNames{
    "black", "dark_blue", "dark_green", "dark_aqua",    "dark_red", "dark_purple", "gold",   "gray",
    "dark_gray", "blue", "green",      "aqua",         "red",      "light_purple", "yellow", "white"};

/// TextColor.parseColor: one of the sixteen names, or `#RRGGBB`.
bool valid_colour(std::string_view colour) {
    if (std::ranges::find(kColourNames, colour) != kColourNames.end()) {
        return true;
    }
    if (colour.size() != 7 || colour.front() != '#') {
        return false;
    }
    return std::ranges::all_of(colour.substr(1), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

std::string json_text(const JsonValue& value) {
    switch (value.type) {
    case JsonValue::Type::String:
    case JsonValue::Type::Number:
        return value.string;
    case JsonValue::Type::Bool:
        return value.boolean ? "true" : "false";
    default:
        return {};
    }
}

/// Gson's own rendering of an element, for "Don't know how to turn … into a
/// Component".
std::string json_source(const JsonValue& value) {
    std::string out;
    switch (value.type) {
    case JsonValue::Type::Null:
        return "null";
    case JsonValue::Type::Bool:
    case JsonValue::Type::Number:
        return json_text(value);
    case JsonValue::Type::String:
        append_json_string(out, value.string);
        return out;
    case JsonValue::Type::Array:
        out += "[";
        for (usize i = 0; i < value.array.size(); ++i) {
            out += (i == 0 ? "" : ",") + json_source(value.array[i]);
        }
        return out + "]";
    case JsonValue::Type::Object:
        out += "{";
        for (usize i = 0; i < value.object.size(); ++i) {
            out += i == 0 ? "" : ",";
            append_json_string(out, value.object[i].first);
            out += ":" + json_source(value.object[i].second);
        }
        return out + "}";
    }
    return out;
}

std::optional<bool> json_flag(const JsonValue& object, std::string_view key) {
    const JsonValue* v = object.find(key);
    if (v == nullptr) {
        return std::nullopt;
    }
    if (v->type == JsonValue::Type::Bool) {
        return v->boolean;
    }
    if (v->type == JsonValue::Type::String) {
        return v->string == "true";
    }
    return std::nullopt;
}

void read_style(const JsonValue& object, Style& style) {
    style.bold          = json_flag(object, "bold");
    style.italic        = json_flag(object, "italic");
    style.underlined    = json_flag(object, "underlined");
    style.strikethrough = json_flag(object, "strikethrough");
    style.obfuscated    = json_flag(object, "obfuscated");
    if (const JsonValue* colour = object.find("color");
        colour != nullptr && colour->type == JsonValue::Type::String && valid_colour(colour->string)) {
        style.color = colour->string;
    }
    if (const JsonValue* insertion = object.find("insertion");
        insertion != nullptr && insertion->type == JsonValue::Type::String) {
        style.insertion = insertion->string;
    }
    if (const JsonValue* click = object.find("clickEvent");
        click != nullptr && click->type == JsonValue::Type::Object) {
        const JsonValue* action = click->find("action");
        const JsonValue* value  = click->find("value");
        static constexpr std::array<std::string_view, 5> kActions{
            "open_url", "run_command", "suggest_command", "change_page", "copy_to_clipboard"};
        if (action != nullptr && value != nullptr && action->type == JsonValue::Type::String &&
            std::ranges::find(kActions, action->string) != kActions.end()) {
            style.click = ClickEvent{action->string, json_text(*value)};
        }
    }
    if (const JsonValue* hover = object.find("hoverEvent");
        hover != nullptr && hover->type == JsonValue::Type::Object) {
        const JsonValue* action   = hover->find("action");
        const JsonValue* contents = hover->find("contents");
        if (contents == nullptr) {
            contents = hover->find("value");
        }
        if (action != nullptr && action->type == JsonValue::Type::String && contents != nullptr) {
            HoverEvent event;
            if (action->string == "show_text") {
                if (auto t = text_from_json(*contents)) {
                    event.action = HoverEvent::Action::ShowText;
                    event.text.push_back(std::move(*t));
                    style.hover.push_back(std::move(event));
                }
            } else if (action->string == "show_item" && contents->type == JsonValue::Type::Object) {
                const JsonValue* id = contents->find("id");
                if (id != nullptr && id->type == JsonValue::Type::String) {
                    event.action = HoverEvent::Action::ShowItem;
                    event.id     = id->string;
                    if (const JsonValue* count = contents->find("count")) {
                        event.count =
                            static_cast<i32>(std::strtol(json_text(*count).c_str(), nullptr, 10));
                    }
                    if (const JsonValue* tag = contents->find("tag");
                        tag != nullptr && tag->type == JsonValue::Type::String) {
                        event.tag = tag->string;
                    }
                    style.hover.push_back(std::move(event));
                }
            } else if (action->string == "show_entity" &&
                       contents->type == JsonValue::Type::Object) {
                const JsonValue* type = contents->find("type");
                const JsonValue* id   = contents->find("id");
                if (type != nullptr && id != nullptr) {
                    event.action      = HoverEvent::Action::ShowEntity;
                    event.entity_type = json_text(*type);
                    event.id          = json_text(*id);
                    if (const JsonValue* name = contents->find("name")) {
                        if (auto t = text_from_json(*name)) {
                            event.text.push_back(std::move(*t));
                        }
                    }
                    style.hover.push_back(std::move(event));
                }
            }
        }
    }
    if (const JsonValue* font = object.find("font");
        font != nullptr && font->type == JsonValue::Type::String) {
        style.font = font->string;
    }
}

/// A translation argument that is a plain unstyled literal travels as the bare
/// string — `"with":["5"]`, as the capture shows.
Text unwrap_argument(Text text) {
    if (text.kind == Text::Kind::Literal && text.style.empty() && text.extra.empty()) {
        return Text::raw(std::move(text.text));
    }
    return text;
}

}  // namespace

std::expected<Text, std::string> text_from_json(const JsonValue& json) {
    if (json.type == JsonValue::Type::String || json.type == JsonValue::Type::Number ||
        json.type == JsonValue::Type::Bool) {
        return Text::literal(json_text(json));
    }
    if (json.type == JsonValue::Type::Array) {
        if (json.array.empty()) {
            return std::unexpected{std::string{"Unexpected empty array of components"}};
        }
        auto base = text_from_json(json.array.front());
        if (!base) {
            return base;
        }
        for (usize i = 1; i < json.array.size(); ++i) {
            auto next = text_from_json(json.array[i]);
            if (!next) {
                return next;
            }
            base->extra.push_back(std::move(*next));
        }
        return base;
    }
    if (json.type != JsonValue::Type::Object) {
        return std::unexpected{"Don't know how to turn " + json_source(json) + " into a Component"};
    }
    Text out;
    if (const JsonValue* text = json.find("text")) {
        out = Text::literal(json_text(*text));
    } else if (const JsonValue* key = json.find("translate")) {
        out = Text::translatable(json_text(*key));
        if (const JsonValue* fallback = json.find("fallback");
            fallback != nullptr && fallback->type == JsonValue::Type::String) {
            out.fallback = fallback->string;
        }
        if (const JsonValue* with = json.find("with");
            with != nullptr && with->type == JsonValue::Type::Array) {
            for (const JsonValue& argument : with->array) {
                auto converted = text_from_json(argument);
                if (!converted) {
                    return converted;
                }
                out.with.push_back(unwrap_argument(std::move(*converted)));
            }
        }
    } else if (const JsonValue* selector = json.find("selector")) {
        out.kind = Text::Kind::Selector;
        out.text = json_text(*selector);
    } else if (json.find("score") != nullptr) {
        out.kind = Text::Kind::Score;
    } else if (const JsonValue* keybind = json.find("keybind")) {
        out.kind = Text::Kind::Keybind;
        out.text = json_text(*keybind);
    } else if (json.find("nbt") != nullptr) {
        out.kind = Text::Kind::Nbt;
    } else {
        return std::unexpected{"Don't know how to turn " + json_source(json) + " into a Component"};
    }
    if (const JsonValue* extra = json.find("extra")) {
        if (extra->type != JsonValue::Type::Array || extra->array.empty()) {
            return std::unexpected{std::string{"Unexpected empty array of components"}};
        }
        for (const JsonValue& child : extra->array) {
            auto converted = text_from_json(child);
            if (!converted) {
                return converted;
            }
            out.extra.push_back(std::move(*converted));
        }
    }
    read_style(json, out.style);
    return out;
}

std::string to_json(const Text& text) {
    std::string out;
    write_text(out, text);
    return out;
}

std::string plain(const Text& text, const Lang* lang) {
    std::string out;
    write_plain(out, text, lang);
    return out;
}

std::optional<Lang> Lang::load(const std::filesystem::path& path) {
    const auto bytes = io::read_file(path);
    if (!bytes) {
        return std::nullopt;
    }
    const std::string_view source{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    usize                  consumed = 0;
    const auto             parsed   = parse_json(source, consumed);
    if (!parsed || parsed->type != JsonValue::Type::Object) {
        return std::nullopt;
    }
    Lang lang;
    for (const auto& [key, value] : parsed->object) {
        if (value.type == JsonValue::Type::String) {
            lang.entries_.emplace(key, value.string);
        }
    }
    return lang;
}

const std::string* Lang::find(std::string_view key) const {
    const auto it = entries_.find(std::string{key});
    return it == entries_.end() ? nullptr : &it->second;
}

std::string java_float_string(f32 value) { return java_number_string(value); }
std::string java_double_string(f64 value) { return java_number_string(value); }

std::string java_fixed6(f64 value) { return fmt::format("{:.6f}", value); }

}  // namespace ov::server::cmd
