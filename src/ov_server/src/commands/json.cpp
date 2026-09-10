#include "json.hpp"

#include <fmt/format.h>

namespace ov::server::cmd {

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    for (const auto& [name, value] : object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

namespace {

constexpr usize kMaxDepth = 256;

class Parser {
public:
    explicit Parser(std::string_view text) : text_{text} {}

    std::expected<JsonValue, JsonError> value(usize depth, std::string& path) {
        if (depth > kMaxDepth) {
            return fail("Nesting too deep", path);
        }
        skip_whitespace();
        if (at_end()) {
            return fail_at("End of input", path);
        }
        const char c = text_[pos_];
        switch (c) {
        case '{':
            return object(depth, path);
        case '[':
            return array(depth, path);
        case '"':
            return string_value(path);
        default:
            return literal(path);
        }
    }

    [[nodiscard]] usize position() const noexcept { return pos_; }

private:
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

    void skip_whitespace() {
        while (!at_end() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                             text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    /// Gson's location suffix. Its column is one past the character it has
    /// just consumed, 1-based — two past the offending index on line one.
    [[nodiscard]] std::string location(usize offending, const std::string& path) const {
        usize line       = 1;
        usize line_start = 0;
        for (usize i = 0; i < offending && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                line_start = i + 1;
            }
        }
        return fmt::format(" at line {} column {} path {}", line, offending - line_start + 2, path);
    }

    std::unexpected<JsonError> fail(std::string_view what, const std::string& path) const {
        return std::unexpected{JsonError{std::string{what} + location(pos_, path), pos_}};
    }

    std::unexpected<JsonError> fail_at(std::string_view what, const std::string& path) const {
        return std::unexpected{JsonError{std::string{what} + location(pos_, path), pos_}};
    }

    std::unexpected<JsonError> malformed(const std::string& path) const {
        return fail("Use JsonReader.setLenient(true) to accept malformed JSON", path);
    }

    std::expected<std::string, JsonError> raw_string(const std::string& path) {
        ++pos_;  // opening quote
        std::string out;
        while (!at_end()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_end()) {
                break;
            }
            const char escape = text_[pos_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
            case '\'':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (pos_ + 4 > text_.size()) {
                    return fail("Unterminated escape sequence", path);
                }
                u32 code = 0;
                for (usize i = 0; i < 4; ++i) {
                    const char h = text_[pos_ + i];
                    code <<= 4U;
                    if (h >= '0' && h <= '9') {
                        code |= static_cast<u32>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        code |= static_cast<u32>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        code |= static_cast<u32>(h - 'A' + 10);
                    } else {
                        return fail(fmt::format("\\u{}", text_.substr(pos_, 4)), path);
                    }
                }
                pos_ += 4;
                // Surrogate pairs are joined; a lone one becomes U+FFFD.
                if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= text_.size() &&
                    text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                    u32  low = 0;
                    bool ok  = true;
                    for (usize i = 0; i < 4; ++i) {
                        const char h = text_[pos_ + 2 + i];
                        low <<= 4U;
                        if (h >= '0' && h <= '9') {
                            low |= static_cast<u32>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            low |= static_cast<u32>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            low |= static_cast<u32>(h - 'A' + 10);
                        } else {
                            ok = false;
                        }
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10U) + (low - 0xDC00);
                        pos_ += 6;
                    }
                }
                if (code >= 0xD800 && code <= 0xDFFF) {
                    code = 0xFFFD;
                }
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else if (code < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                return fail("Invalid escape sequence", path);
            }
        }
        return fail("Unterminated string", path);
    }

    std::expected<JsonValue, JsonError> string_value(const std::string& path) {
        auto text = raw_string(path);
        if (!text) {
            return std::unexpected{text.error()};
        }
        JsonValue out;
        out.type   = JsonValue::Type::String;
        out.string = std::move(*text);
        return out;
    }

    std::expected<JsonValue, JsonError> literal(const std::string& path) {
        const usize start = pos_;
        while (!at_end()) {
            const char c = text_[pos_];
            if (c == ',' || c == ']' || c == '}' || c == ':' || c == ' ' || c == '\t' ||
                c == '\n' || c == '\r' || c == '{' || c == '[' || c == '"' || c == '\'' ||
                c == '/' || c == '\\' || c == ';' || c == '#' || c == '=') {
                break;
            }
            ++pos_;
        }
        const std::string_view word = text_.substr(start, pos_ - start);
        JsonValue              out;
        if (word == "true" || word == "false") {
            out.type    = JsonValue::Type::Bool;
            out.boolean = word == "true";
            return out;
        }
        if (word == "null") {
            out.type = JsonValue::Type::Null;
            return out;
        }
        if (is_number(word)) {
            out.type   = JsonValue::Type::Number;
            out.string = std::string{word};
            return out;
        }
        pos_ = start;
        return malformed(path);
    }

    [[nodiscard]] static bool is_number(std::string_view word) {
        usize i = 0;
        if (i < word.size() && word[i] == '-') {
            ++i;
        }
        const usize digits_start = i;
        while (i < word.size() && word[i] >= '0' && word[i] <= '9') {
            ++i;
        }
        if (i == digits_start) {
            return false;
        }
        if (word[digits_start] == '0' && i - digits_start > 1) {
            return false;
        }
        if (i < word.size() && word[i] == '.') {
            ++i;
            const usize fraction = i;
            while (i < word.size() && word[i] >= '0' && word[i] <= '9') {
                ++i;
            }
            if (i == fraction) {
                return false;
            }
        }
        if (i < word.size() && (word[i] == 'e' || word[i] == 'E')) {
            ++i;
            if (i < word.size() && (word[i] == '+' || word[i] == '-')) {
                ++i;
            }
            const usize exponent = i;
            while (i < word.size() && word[i] >= '0' && word[i] <= '9') {
                ++i;
            }
            if (i == exponent) {
                return false;
            }
        }
        return i == word.size();
    }

    std::expected<JsonValue, JsonError> array(usize depth, std::string& path) {
        ++pos_;
        JsonValue out;
        out.type = JsonValue::Type::Array;
        skip_whitespace();
        if (!at_end() && text_[pos_] == ']') {
            ++pos_;
            return out;
        }
        const usize base = path.size();
        for (usize index = 0;; ++index) {
            path.resize(base);
            path += fmt::format("[{}]", index);
            auto element = value(depth + 1, path);
            if (!element) {
                return element;
            }
            out.array.push_back(std::move(*element));
            skip_whitespace();
            if (at_end()) {
                path.resize(base);
                return fail("Unterminated array", path);
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                path.resize(base);
                return out;
            }
            path.resize(base);
            return fail("Unterminated array", path);
        }
    }

    std::expected<JsonValue, JsonError> object(usize depth, std::string& path) {
        ++pos_;
        JsonValue out;
        out.type = JsonValue::Type::Object;
        const usize base = path.size();
        skip_whitespace();
        if (!at_end() && text_[pos_] == '}') {
            ++pos_;
            return out;
        }
        while (true) {
            skip_whitespace();
            path.resize(base);
            path += ".";
            if (at_end()) {
                return fail("End of input", path);
            }
            if (text_[pos_] != '"') {
                // An unquoted name is exactly the case Gson answers with its
                // lenient hint, and the one the capture recorded.
                return malformed(path);
            }
            auto name = raw_string(path);
            if (!name) {
                return std::unexpected{name.error()};
            }
            path.resize(base);
            path += "." + *name;
            skip_whitespace();
            if (at_end() || text_[pos_] != ':') {
                return fail("Expected ':'", path);
            }
            ++pos_;
            auto member = value(depth + 1, path);
            if (!member) {
                return member;
            }
            out.object.emplace_back(std::move(*name), std::move(*member));
            skip_whitespace();
            if (at_end()) {
                return fail("Unterminated object", path);
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                path.resize(base);
                return out;
            }
            return fail("Unterminated object", path);
        }
    }

    std::string_view text_;
    usize            pos_{0};
};

}  // namespace

std::expected<JsonValue, JsonError> parse_json(std::string_view text, usize& consumed) {
    Parser      parser{text};
    std::string path{"$"};
    auto        result = parser.value(0, path);
    consumed           = parser.position();
    return result;
}

}  // namespace ov::server::cmd
