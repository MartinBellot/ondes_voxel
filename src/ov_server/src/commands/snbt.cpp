#include "snbt.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>

namespace ov::server::cmd {
namespace {

constexpr usize kMaxDepth = 512;

[[nodiscard]] std::string_view type_name(nbt::TagType type) {
    switch (type) {
    case nbt::TagType::Byte:
        return "TAG_Byte";
    case nbt::TagType::Short:
        return "TAG_Short";
    case nbt::TagType::Int:
        return "TAG_Int";
    case nbt::TagType::Long:
        return "TAG_Long";
    case nbt::TagType::Float:
        return "TAG_Float";
    case nbt::TagType::Double:
        return "TAG_Double";
    case nbt::TagType::ByteArray:
        return "TAG_Byte_Array";
    case nbt::TagType::String:
        return "TAG_String";
    case nbt::TagType::List:
        return "TAG_List";
    case nbt::TagType::Compound:
        return "TAG_Compound";
    case nbt::TagType::IntArray:
        return "TAG_Int_Array";
    case nbt::TagType::LongArray:
        return "TAG_Long_Array";
    default:
        return "TAG_End";
    }
}

[[nodiscard]] bool digits_only(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

/// `[-+]?(?:0|[1-9][0-9]*)`
[[nodiscard]] bool is_integer_literal(std::string_view s) {
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        s.remove_prefix(1);
    }
    if (!digits_only(s)) {
        return false;
    }
    return s == "0" || s.front() != '0';
}

/// `[-+]?(?:[0-9]+[.]|[0-9]*[.][0-9]+)(?:e[-+]?[0-9]+)?` — with `allow_bare`
/// the fraction may be absent entirely (`[0-9]+[.]?`), which is the float and
/// the suffixed double form.
[[nodiscard]] bool is_decimal_literal(std::string_view s, bool allow_bare) {
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        s.remove_prefix(1);
    }
    std::string_view mantissa = s;
    std::string_view exponent;
    const auto       e = s.find_first_of("eE");
    if (e != std::string_view::npos) {
        mantissa = s.substr(0, e);
        exponent = s.substr(e + 1);
        if (!exponent.empty() && (exponent.front() == '-' || exponent.front() == '+')) {
            exponent.remove_prefix(1);
        }
        if (!digits_only(exponent)) {
            return false;
        }
    }
    const auto dot = mantissa.find('.');
    if (dot == std::string_view::npos) {
        return allow_bare && digits_only(mantissa);
    }
    const std::string_view before = mantissa.substr(0, dot);
    const std::string_view after  = mantissa.substr(dot + 1);
    if (!before.empty() && !digits_only(before)) {
        return false;
    }
    if (!after.empty() && !digits_only(after)) {
        return false;
    }
    return !before.empty() || !after.empty();
}

/// What an unquoted word is: a number with or without a suffix, a boolean,
/// or — failing all of those — a string. Out-of-range numbers are strings
/// too, which is vanilla's `NumberFormatException` fallback.
nbt::Tag type_word(std::string_view word) {
    const auto lower_suffix = [&](char suffix) {
        return !word.empty() && std::tolower(static_cast<unsigned char>(word.back())) == suffix;
    };
    const auto body = word.substr(0, word.empty() ? 0 : word.size() - 1);
    const auto integral = [&](std::string_view text, i64 min, i64 max) -> std::optional<i64> {
        std::string_view digits = text;
        if (!digits.empty() && digits.front() == '+') {
            digits.remove_prefix(1);
        }
        return parse_java_long(digits, min, max);
    };
    const auto decimal = [&](std::string_view text) -> std::optional<f64> {
        std::string cleaned{text};
        if (!cleaned.empty() && cleaned.front() == '+') {
            cleaned.erase(cleaned.begin());
        }
        const bool negative = !cleaned.empty() && cleaned.front() == '-';
        if (negative) {
            cleaned.erase(cleaned.begin());
        }
        if (!cleaned.empty() && cleaned.front() == '.') {
            cleaned.insert(cleaned.begin(), '0');
        }
        const auto e = cleaned.find_first_of("eE");
        if (e != std::string::npos && e > 0 && cleaned[e - 1] == '.') {
            cleaned.insert(e, "0");
        } else if (!cleaned.empty() && cleaned.back() == '.') {
            cleaned.push_back('0');
        }
        f64        value = 0.0;
        const auto [ptr, ec] =
            std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value);
        if (ptr != cleaned.data() + cleaned.size() ||
            (ec != std::errc{} && ec != std::errc::result_out_of_range)) {
            return std::optional<f64>{};
        }
        return std::optional<f64>{negative ? -value : value};
    };
    if (lower_suffix('f') && is_decimal_literal(body, true)) {
        if (const auto v = decimal(body)) {
            return nbt::Tag{static_cast<f32>(*v)};
        }
    }
    if (lower_suffix('b') && is_integer_literal(body)) {
        if (const auto v = integral(body, -128, 127)) {
            return nbt::Tag{static_cast<i8>(*v)};
        }
    }
    if (lower_suffix('l') && is_integer_literal(body)) {
        if (const auto v = integral(body, std::numeric_limits<i64>::min(),
                                    std::numeric_limits<i64>::max())) {
            return nbt::Tag{*v};
        }
    }
    if (lower_suffix('s') && is_integer_literal(body)) {
        if (const auto v = integral(body, -32768, 32767)) {
            return nbt::Tag{static_cast<i16>(*v)};
        }
    }
    if (is_integer_literal(word)) {
        if (const auto v = integral(word, std::numeric_limits<i32>::min(),
                                    std::numeric_limits<i32>::max())) {
            return nbt::Tag{static_cast<i32>(*v)};
        }
    }
    if (lower_suffix('d') && is_decimal_literal(body, true)) {
        if (const auto v = decimal(body)) {
            return nbt::Tag{*v};
        }
    }
    if (is_decimal_literal(word, false)) {
        if (const auto v = decimal(word)) {
            return nbt::Tag{*v};
        }
    }
    if (word == "true") {
        return nbt::Tag::make_bool(true);
    }
    if (word == "false") {
        return nbt::Tag::make_bool(false);
    }
    return nbt::Tag{std::string{word}};
}

class SnbtParser {
public:
    explicit SnbtParser(StringReader& reader) : reader_{reader} {}

    Parsed<nbt::Tag> value(usize depth) {
        if (depth > kMaxDepth) {
            return std::unexpected{reader_.error("argument.nbt.expected.value")};
        }
        reader_.skip_whitespace();
        if (!reader_.can_read()) {
            return std::unexpected{reader_.error("argument.nbt.expected.value")};
        }
        const char c = reader_.peek();
        if (c == '{') {
            return compound(depth);
        }
        if (c == '[') {
            return list_or_array(depth);
        }
        return typed_value();
    }

    Parsed<nbt::Tag> compound(usize depth) {
        if (auto ok = expect('{'); !ok) {
            return std::unexpected{ok.error()};
        }
        nbt::Tag out = nbt::Tag::make_compound();
        reader_.skip_whitespace();
        while (reader_.can_read() && reader_.peek() != '}') {
            const usize start = reader_.cursor();
            auto        key   = read_key();
            if (!key) {
                return std::unexpected{key.error()};
            }
            if (key->empty()) {
                reader_.set_cursor(start);
                return std::unexpected{reader_.error("argument.nbt.expected.key")};
            }
            if (auto ok = expect(':'); !ok) {
                return std::unexpected{ok.error()};
            }
            auto element = value(depth + 1);
            if (!element) {
                return element;
            }
            out.put(std::move(*key), std::move(*element));
            if (!has_separator()) {
                break;
            }
            if (!reader_.can_read()) {
                return std::unexpected{reader_.error("argument.nbt.expected.key")};
            }
        }
        if (auto ok = expect('}'); !ok) {
            return std::unexpected{ok.error()};
        }
        return out;
    }

private:
    Parsed<void> expect(char c) {
        reader_.skip_whitespace();
        return reader_.expect(c);
    }

    Parsed<std::string> read_key() {
        reader_.skip_whitespace();
        if (!reader_.can_read()) {
            return std::unexpected{reader_.error("argument.nbt.expected.key")};
        }
        return reader_.read_string();
    }

    bool has_separator() {
        reader_.skip_whitespace();
        if (reader_.can_read() && reader_.peek() == ',') {
            reader_.skip();
            reader_.skip_whitespace();
            return true;
        }
        return false;
    }

    Parsed<nbt::Tag> typed_value() {
        reader_.skip_whitespace();
        const usize start = reader_.cursor();
        if (StringReader::is_quoted_string_start(reader_.peek())) {
            auto text = reader_.read_quoted_string();
            if (!text) {
                return std::unexpected{text.error()};
            }
            return nbt::Tag{std::move(*text)};
        }
        const std::string_view word = reader_.read_unquoted_string();
        if (word.empty()) {
            reader_.set_cursor(start);
            return std::unexpected{reader_.error("argument.nbt.expected.value")};
        }
        return type_word(word);
    }

    Parsed<nbt::Tag> list_or_array(usize depth) {
        if (reader_.can_read(3) && !StringReader::is_quoted_string_start(reader_.peek(1)) &&
            reader_.peek(2) == ';') {
            return array();
        }
        return list(depth);
    }

    Parsed<nbt::Tag> list(usize depth) {
        if (auto ok = expect('['); !ok) {
            return std::unexpected{ok.error()};
        }
        reader_.skip_whitespace();
        if (!reader_.can_read()) {
            return std::unexpected{reader_.error("argument.nbt.expected.value")};
        }
        std::vector<nbt::Tag> items;
        nbt::TagType          element_type = nbt::TagType::End;
        while (reader_.peek() != ']') {
            const usize start   = reader_.cursor();
            auto        element = value(depth + 1);
            if (!element) {
                return element;
            }
            if (element_type == nbt::TagType::End) {
                element_type = element->type();
            } else if (element->type() != element_type) {
                reader_.set_cursor(start);
                return std::unexpected{reader_.error(
                    "argument.nbt.list.mixed",
                    {Text::raw(std::string{type_name(element->type())}),
                     Text::raw(std::string{type_name(element_type)})})};
            }
            items.push_back(std::move(*element));
            if (!has_separator()) {
                break;
            }
            if (!reader_.can_read()) {
                return std::unexpected{reader_.error("argument.nbt.expected.value")};
            }
        }
        if (auto ok = expect(']'); !ok) {
            return std::unexpected{ok.error()};
        }
        nbt::Tag out = nbt::Tag::make_list(element_type);
        for (nbt::Tag& item : items) {
            (void)out.push(std::move(item));
        }
        return out;
    }

    Parsed<nbt::Tag> array() {
        if (auto ok = expect('['); !ok) {
            return std::unexpected{ok.error()};
        }
        const usize start = reader_.cursor();
        const char  kind  = reader_.read();
        reader_.read();  // ';'
        reader_.skip_whitespace();
        if (!reader_.can_read()) {
            return std::unexpected{reader_.error("argument.nbt.expected.value")};
        }
        nbt::TagType wanted{};
        std::string  array_name;
        if (kind == 'B') {
            wanted     = nbt::TagType::Byte;
            array_name = "TAG_Byte_Array";
        } else if (kind == 'L') {
            wanted     = nbt::TagType::Long;
            array_name = "TAG_Long_Array";
        } else if (kind == 'I') {
            wanted     = nbt::TagType::Int;
            array_name = "TAG_Int_Array";
        } else {
            reader_.set_cursor(start);
            return std::unexpected{
                reader_.error("argument.nbt.array.invalid", {Text::raw(std::string(1, kind))})};
        }
        std::vector<i64> values;
        while (reader_.peek() != ']') {
            const usize element_start = reader_.cursor();
            auto        element       = value(1);
            if (!element) {
                return element;
            }
            if (element->type() != wanted) {
                reader_.set_cursor(element_start);
                return std::unexpected{reader_.error(
                    "argument.nbt.array.mixed",
                    {Text::raw(std::string{type_name(element->type())}), Text::raw(array_name)})};
            }
            values.push_back(element->as_i64());
            if (!has_separator()) {
                break;
            }
            if (!reader_.can_read()) {
                return std::unexpected{reader_.error("argument.nbt.expected.value")};
            }
        }
        if (auto ok = expect(']'); !ok) {
            return std::unexpected{ok.error()};
        }
        if (kind == 'B') {
            nbt::Tag::ByteArray bytes;
            for (const i64 v : values) {
                bytes.push_back(static_cast<u8>(static_cast<i8>(v)));
            }
            return nbt::Tag{std::move(bytes)};
        }
        if (kind == 'I') {
            nbt::Tag::IntArray ints;
            for (const i64 v : values) {
                ints.push_back(static_cast<i32>(v));
            }
            return nbt::Tag{std::move(ints)};
        }
        nbt::Tag::LongArray longs{values.begin(), values.end()};
        return nbt::Tag{std::move(longs)};
    }

    StringReader& reader_;
};

[[nodiscard]] bool is_simple_key(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '+' || c == '-';
    });
}

/// Vanilla's `quoteAndEscape`: the quote is whichever of `"` and `'` the text
/// does not start needing, backslashes and the chosen quote escaped.
std::string quote_and_escape(std::string_view text) {
    std::string body;
    char        quote = 0;
    for (const char c : text) {
        if (c == '\\') {
            body.push_back('\\');
        } else if (c == '"' || c == '\'') {
            if (quote == 0) {
                quote = c == '"' ? '\'' : '"';
            }
            if (quote == c) {
                body.push_back('\\');
            }
        }
        body.push_back(c);
    }
    if (quote == 0) {
        quote = '"';
    }
    return std::string(1, quote) + body + std::string(1, quote);
}

void write_snbt(std::string& out, const nbt::Tag& tag) {
    switch (tag.type()) {
    case nbt::TagType::Byte:
        out += std::to_string(*tag.get_if<i8>()) + "b";
        break;
    case nbt::TagType::Short:
        out += std::to_string(*tag.get_if<i16>()) + "s";
        break;
    case nbt::TagType::Int:
        out += std::to_string(*tag.get_if<i32>());
        break;
    case nbt::TagType::Long:
        out += std::to_string(*tag.get_if<i64>()) + "L";
        break;
    case nbt::TagType::Float:
        out += java_float_string(*tag.get_if<f32>()) + "f";
        break;
    case nbt::TagType::Double:
        out += java_double_string(*tag.get_if<f64>()) + "d";
        break;
    case nbt::TagType::String:
        out += quote_and_escape(tag.as_string());
        break;
    case nbt::TagType::ByteArray: {
        out += "[B;";
        const auto& bytes = *tag.get_if<nbt::Tag::ByteArray>();
        for (usize i = 0; i < bytes.size(); ++i) {
            out += (i == 0 ? "" : ",") + std::to_string(static_cast<i8>(bytes[i])) + "B";
        }
        out += "]";
        break;
    }
    case nbt::TagType::IntArray: {
        out += "[I;";
        const auto& ints = *tag.get_if<nbt::Tag::IntArray>();
        for (usize i = 0; i < ints.size(); ++i) {
            out += (i == 0 ? "" : ",") + std::to_string(ints[i]);
        }
        out += "]";
        break;
    }
    case nbt::TagType::LongArray: {
        out += "[L;";
        const auto& longs = *tag.get_if<nbt::Tag::LongArray>();
        for (usize i = 0; i < longs.size(); ++i) {
            out += (i == 0 ? "" : ",") + std::to_string(longs[i]) + "L";
        }
        out += "]";
        break;
    }
    case nbt::TagType::List: {
        out += "[";
        const auto* items = tag.list();
        for (usize i = 0; items != nullptr && i < items->size(); ++i) {
            if (i > 0) {
                out += ",";
            }
            write_snbt(out, (*items)[i]);
        }
        out += "]";
        break;
    }
    case nbt::TagType::Compound: {
        out += "{";
        std::vector<const nbt::CompoundEntry*> entries;
        for (const nbt::CompoundEntry& entry : *tag.compound()) {
            entries.push_back(&entry);
        }
        std::ranges::sort(entries, [](const nbt::CompoundEntry* a, const nbt::CompoundEntry* b) {
            return a->name < b->name;
        });
        for (usize i = 0; i < entries.size(); ++i) {
            if (i > 0) {
                out += ",";
            }
            out += is_simple_key(entries[i]->name) ? entries[i]->name
                                                   : quote_and_escape(entries[i]->name);
            out += ":";
            write_snbt(out, entries[i]->value);
        }
        out += "}";
        break;
    }
    default:
        out += "END";
        break;
    }
}

}  // namespace

Parsed<nbt::Tag> read_snbt_compound(StringReader& reader) {
    SnbtParser parser{reader};
    return parser.compound(0);
}

Parsed<nbt::Tag> read_snbt_value(StringReader& reader) {
    SnbtParser parser{reader};
    return parser.value(0);
}

std::string to_snbt(const nbt::Tag& tag) {
    std::string out;
    write_snbt(out, tag);
    return out;
}

bool snbt_matches(const nbt::Tag& pattern, const nbt::Tag& tag) {
    if (pattern.type() == nbt::TagType::Compound) {
        if (tag.type() != nbt::TagType::Compound) {
            return false;
        }
        for (const nbt::CompoundEntry& entry : *pattern.compound()) {
            const nbt::Tag* found = tag.find(entry.name);
            if (found == nullptr || !snbt_matches(entry.value, *found)) {
                return false;
            }
        }
        return true;
    }
    if (pattern.type() == nbt::TagType::List && tag.type() == nbt::TagType::List) {
        const auto* wanted = pattern.list();
        const auto* have   = tag.list();
        if (wanted->empty()) {
            return have->empty();
        }
        return std::ranges::all_of(*wanted, [&](const nbt::Tag& w) {
            return std::ranges::any_of(*have, [&](const nbt::Tag& h) { return snbt_matches(w, h); });
        });
    }
    return pattern == tag;
}

}  // namespace ov::server::cmd
