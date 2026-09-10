#include "ov/render/text_component.hpp"

#include "json.hpp"
#include "ov/render/language.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace ov::render {

namespace {

/// `§` in UTF-8.
constexpr std::string_view kSection = "\xC2\xA7";

struct Style {
    char colour{0};
    bool bold{false};
    bool italic{false};
    bool underlined{false};
    bool strikethrough{false};
    bool obfuscated{false};
};

/// The sixteen named colours, in code order, with the RGB vanilla gives each.
struct Named {
    std::string_view name;
    char             code;
    u32              rgb;
};

constexpr std::array<Named, 16> kColours{{
    {"black", '0', 0x000000}, {"dark_blue", '1', 0x0000AA},    {"dark_green", '2', 0x00AA00},
    {"dark_aqua", '3', 0x00AAAA}, {"dark_red", '4', 0xAA0000},  {"dark_purple", '5', 0xAA00AA},
    {"gold", '6', 0xFFAA00},  {"gray", '7', 0xAAAAAA},         {"dark_gray", '8', 0x555555},
    {"blue", '9', 0x5555FF},  {"green", 'a', 0x55FF55},        {"aqua", 'b', 0x55FFFF},
    {"red", 'c', 0xFF5555},   {"light_purple", 'd', 0xFF55FF}, {"yellow", 'e', 0xFFFF55},
    {"white", 'f', 0xFFFFFF},
}};

[[nodiscard]] char nearest_code(u32 rgb) noexcept {
    char best      = 'f';
    i64  best_dist = -1;
    for (const Named& named : kColours) {
        const i64 dr   = static_cast<i64>((rgb >> 16) & 0xFF) - static_cast<i64>((named.rgb >> 16) & 0xFF);
        const i64 dg   = static_cast<i64>((rgb >> 8) & 0xFF) - static_cast<i64>((named.rgb >> 8) & 0xFF);
        const i64 db   = static_cast<i64>(rgb & 0xFF) - static_cast<i64>(named.rgb & 0xFF);
        const i64 dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) {
            best      = named.code;
            best_dist = dist;
        }
    }
    return best;
}

/// The codes that put the pen into `style`, starting from a reset. Colour
/// first: in the legacy scheme a colour code clears the formats before it.
void emit_style(const Style& style, std::string& out) {
    out += kSection;
    out += style.colour != 0 ? style.colour : 'r';
    const auto flag = [&out](bool on, char code) {
        if (on) {
            out += kSection;
            out += code;
        }
    };
    flag(style.obfuscated, 'k');
    flag(style.bold, 'l');
    flag(style.strikethrough, 'm');
    flag(style.underlined, 'n');
    flag(style.italic, 'o');
}

class Flattener {
public:
    explicit Flattener(const Language& language) : language_(language) {}

    /// Append `value`, drawn in `inherited` unless it restyles itself.
    void walk(json::Value value, const Style& inherited, std::string& out) {
        if (!value.valid()) {
            return;
        }
        if (value.is_string()) {
            text(value.as_string(), inherited, out);
            return;
        }
        if (value.is_number() || value.is_bool()) {
            text(scalar(value), inherited, out);
            return;
        }
        if (value.is_array()) {
            // An array is its first element with the rest as its extras: they
            // inherit the first element's style.
            if (value.size() == 0) {
                return;
            }
            const Style first = restyle(value[0u], inherited);
            walk(value[0u], inherited, out);
            for (u32 i = 1; i < value.size(); ++i) {
                walk(value[i], first, out);
            }
            return;
        }
        if (!value.is_object()) {
            return;
        }
        const Style style = restyle(value, inherited);

        if (const json::Value literal = value["text"]; literal.valid()) {
            text(literal.is_string() ? literal.as_string() : std::string_view(scalar(literal)),
                 style, out);
        } else if (const json::Value key = value["translate"]; key.is_string()) {
            translated(value, key.as_string(), style, out);
        } else if (const json::Value bind = value["keybind"]; bind.is_string()) {
            const std::string_view shown = default_key_name(bind.as_string());
            text(shown.empty() ? language_.translate(bind.as_string()) : shown, style, out);
        }
        // score, selector, nbt: refused (see the header) — nothing is drawn.

        const json::Value extra = value["extra"];
        for (u32 i = 0; i < extra.size(); ++i) {
            walk(extra[i], style, out);
        }
    }

private:
    [[nodiscard]] static std::string scalar(json::Value value) {
        if (value.is_bool()) {
            return value.as_bool() ? "true" : "false";
        }
        const f64 number = value.as_number();
        if (std::floor(number) == number && std::abs(number) < 1e15) {
            return std::to_string(static_cast<i64>(number));
        }
        return std::to_string(number);
    }

    [[nodiscard]] static Style restyle(json::Value value, const Style& inherited) {
        Style style = inherited;
        if (!value.is_object()) {
            return style;
        }
        if (const json::Value colour = value["color"]; colour.is_string()) {
            const std::string_view name = colour.as_string();
            if (name.size() == 7 && name[0] == '#') {
                const auto rgb = static_cast<u32>(
                    std::strtoul(std::string(name.substr(1)).c_str(), nullptr, 16));
                style.colour = nearest_code(rgb);
            } else if (const char code = colour_code(name); code != 0) {
                style.colour = code;
            }
        }
        const auto flag = [&value](std::string_view key, bool& field) {
            if (const json::Value on = value[key]; on.is_bool()) {
                field = on.as_bool();
            }
        };
        flag("bold", style.bold);
        flag("italic", style.italic);
        flag("underlined", style.underlined);
        flag("strikethrough", style.strikethrough);
        flag("obfuscated", style.obfuscated);
        return style;
    }

    void text(std::string_view literal, const Style& style, std::string& out) {
        if (literal.empty()) {
            return;
        }
        emit_style(style, out);
        out += literal;
    }

    void translated(json::Value value, std::string_view key, const Style& style,
                    std::string& out) {
        std::string_view pattern = language_.translate(key);
        if (pattern == key) {
            // Vanilla's rule: the fallback when the key is unknown, the key
            // itself when there is no fallback.
            if (const json::Value fallback = value["fallback"]; fallback.is_string()) {
                pattern = fallback.as_string();
            }
        }
        // Each argument is flattened in its own style, then re-styled back to
        // the parent's so the pattern text after it is drawn as it should be.
        std::vector<std::string> arguments;
        const json::Value        with = value["with"];
        arguments.reserve(with.size());
        for (u32 i = 0; i < with.size(); ++i) {
            std::string argument;
            walk(with[i], style, argument);
            emit_style(style, argument);
            arguments.push_back(std::move(argument));
        }
        if (pattern.empty()) {
            return;
        }
        emit_style(style, out);
        out += format_translation(pattern, arguments);
    }

    const Language& language_;
};

/// The walk of Flattener, producing runs with exact colours. Kept separate
/// rather than folded into Flattener so that flatten_component's bytes — which
/// the creative tooltips and their tests pin — cannot move.
class RunBuilder {
public:
    explicit RunBuilder(const Language& language) : language_(language) {}

    void walk(json::Value value, const StyledRun& inherited, std::vector<StyledRun>& out,
              u32 depth = 0) {
        // Components come off the network: a defence against a hostile tree
        // nested deeper than any real message, like json::kMaxDepth.
        if (!value.valid() || depth > json::kMaxDepth) {
            return;
        }
        if (value.is_string()) {
            text(value.as_string(), inherited, out);
            return;
        }
        if (value.is_number() || value.is_bool()) {
            text(scalar(value), inherited, out);
            return;
        }
        if (value.is_array()) {
            if (value.size() == 0) {
                return;
            }
            const StyledRun first = restyle(value[0u], inherited);
            walk(value[0u], inherited, out, depth + 1);
            for (u32 i = 1; i < value.size(); ++i) {
                walk(value[i], first, out, depth + 1);
            }
            return;
        }
        if (!value.is_object()) {
            return;
        }
        const StyledRun style = restyle(value, inherited);
        if (const json::Value literal = value["text"]; literal.valid()) {
            text(literal.is_string() ? std::string(literal.as_string()) : scalar(literal), style,
                 out);
        } else if (const json::Value key = value["translate"]; key.is_string()) {
            translated(value, key.as_string(), style, out, depth);
        } else if (const json::Value bind = value["keybind"]; bind.is_string()) {
            const std::string_view shown = default_key_name(bind.as_string());
            text(shown.empty() ? language_.translate(bind.as_string()) : shown, style, out);
        }
        // score, selector, nbt: refused (see the header) — nothing is drawn.
        const json::Value extra = value["extra"];
        for (u32 i = 0; i < extra.size(); ++i) {
            walk(extra[i], style, out, depth + 1);
        }
    }

private:
    [[nodiscard]] static std::string scalar(json::Value value) {
        if (value.is_bool()) {
            return value.as_bool() ? "true" : "false";
        }
        const f64 number = value.as_number();
        if (std::floor(number) == number && std::abs(number) < 1e15) {
            return std::to_string(static_cast<i64>(number));
        }
        return std::to_string(number);
    }

    [[nodiscard]] static StyledRun restyle(json::Value value, const StyledRun& inherited) {
        StyledRun style = inherited;
        style.text.clear();
        if (!value.is_object()) {
            return style;
        }
        if (const json::Value colour = value["color"]; colour.is_string()) {
            if (const auto rgb = colour_rgb(colour.as_string())) {
                style.rgb        = *rgb;
                style.has_colour = true;
            }
        }
        const auto flag = [&value](std::string_view key, bool& field) {
            if (const json::Value on = value[key]; on.is_bool()) {
                field = on.as_bool();
            }
        };
        flag("bold", style.bold);
        flag("italic", style.italic);
        flag("underlined", style.underlined);
        flag("strikethrough", style.strikethrough);
        flag("obfuscated", style.obfuscated);
        return style;
    }

    static void text(std::string_view literal, const StyledRun& style, std::vector<StyledRun>& out) {
        if (literal.empty()) {
            return;
        }
        if (!out.empty() && out.back().same_style(style)) {
            out.back().text += literal;
            return;
        }
        StyledRun run = style;
        run.text      = std::string(literal);
        out.push_back(std::move(run));
    }

    void translated(json::Value value, std::string_view key, const StyledRun& style,
                    std::vector<StyledRun>& out, u32 depth) {
        std::string_view pattern = language_.translate(key);
        if (pattern == key) {
            if (const json::Value fallback = value["fallback"]; fallback.is_string()) {
                pattern = fallback.as_string();
            }
        }
        const json::Value with = value["with"];
        // The pattern, split where format_translation would substitute: each
        // placeholder draws its argument's own runs, which inherit this
        // component's style; the text between them is drawn in that style.
        usize next = 0;
        usize from = 0;
        const auto argument = [&](usize index) {
            walk(with[static_cast<u32>(index)], style, out, depth + 1);
        };
        for (usize i = 0; i < pattern.size(); ++i) {
            if (pattern[i] != '%' || i + 1 >= pattern.size()) {
                continue;
            }
            if (pattern[i + 1] == '%') {
                text(pattern.substr(from, i + 1 - from), style, out);
                from = i + 2;
                ++i;
                continue;
            }
            if (pattern[i + 1] == 's') {
                text(pattern.substr(from, i - from), style, out);
                if (next < with.size()) {
                    argument(next);
                } else {
                    text("%s", style, out);
                }
                ++next;
                from = i + 2;
                ++i;
                continue;
            }
            usize j     = i + 1;
            usize index = 0;
            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
                index = index * 10 + static_cast<usize>(pattern[j] - '0');
                ++j;
            }
            if (j + 1 < pattern.size() && j > i + 1 && pattern[j] == '$' && pattern[j + 1] == 's') {
                text(pattern.substr(from, i - from), style, out);
                if (index >= 1 && index <= with.size()) {
                    argument(index - 1);
                } else {
                    text(pattern.substr(i, j + 2 - i), style, out);
                }
                from = j + 2;
                i    = j + 1;
            }
        }
        text(pattern.substr(std::min(from, pattern.size())), style, out);
    }

    const Language& language_;
};

}  // namespace

std::optional<u32> colour_rgb(std::string_view name) noexcept {
    if (name.size() == 7 && name[0] == '#') {
        u32 rgb = 0;
        for (usize i = 1; i < 7; ++i) {
            const char c = name[i];
            u32        digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<u32>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
            rgb = rgb * 16 + digit;
        }
        return rgb;
    }
    for (const Named& named : kColours) {
        if (named.name == name) {
            return named.rgb;
        }
    }
    return std::nullopt;
}

std::vector<StyledRun> component_runs(std::string_view json_text, const Language& language) {
    std::vector<StyledRun> out;
    auto                   document = json::Document::parse(json_text);
    if (!document) {
        out.push_back(StyledRun{std::string(json_text)});
        return out;
    }
    RunBuilder builder(language);
    builder.walk(document->root(), StyledRun{}, out);
    return out;
}

std::string plain_text(std::span<const StyledRun> runs) {
    std::string out;
    for (const StyledRun& run : runs) {
        out += run.text;
    }
    return out;
}

char colour_code(std::string_view name) noexcept {
    for (const Named& named : kColours) {
        if (named.name == name) {
            return named.code;
        }
    }
    return 0;
}

std::string format_translation(std::string_view pattern, std::span<const std::string> arguments) {
    std::string out;
    out.reserve(pattern.size());
    usize next = 0;
    for (usize i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        if (pattern[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        if (pattern[i + 1] == 's') {
            if (next < arguments.size()) {
                out += arguments[next];
            } else {
                out += "%s";
            }
            ++next;
            ++i;
            continue;
        }
        // %<n>$s
        usize j     = i + 1;
        usize index = 0;
        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
            index = index * 10 + static_cast<usize>(pattern[j] - '0');
            ++j;
        }
        if (j + 1 < pattern.size() && j > i + 1 && pattern[j] == '$' && pattern[j + 1] == 's') {
            if (index >= 1 && index <= arguments.size()) {
                out += arguments[index - 1];
            } else {
                out.append(pattern.substr(i, j + 2 - i));
            }
            i = j + 1;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

std::string_view default_key_name(std::string_view keybind) noexcept {
    // 1.20.1's defaults, from the Controls screen: C saves a toolbar, X loads
    // one, and the hotbar slots are the number keys.
    if (keybind == "key.saveToolbarActivator") {
        return "C";
    }
    if (keybind == "key.loadToolbarActivator") {
        return "X";
    }
    constexpr std::string_view kHotbar = "key.hotbar.";
    if (keybind.starts_with(kHotbar) && keybind.size() == kHotbar.size() + 1) {
        return keybind.substr(kHotbar.size());
    }
    if (keybind == "key.inventory") {
        return "E";
    }
    if (keybind == "key.drop") {
        return "Q";
    }
    if (keybind == "key.chat") {
        return "T";
    }
    return {};
}

std::string flatten_component(std::string_view json_text, const Language& language) {
    auto document = json::Document::parse(json_text);
    if (!document) {
        return std::string(json_text);
    }
    std::string out;
    Flattener   flattener(language);
    flattener.walk(document->root(), Style{}, out);
    return out;
}

std::string strip_formatting(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        if (text.substr(i, kSection.size()) == kSection && i + kSection.size() < text.size()) {
            i += kSection.size();
            continue;
        }
        out += text[i];
    }
    return out;
}

}  // namespace ov::render
