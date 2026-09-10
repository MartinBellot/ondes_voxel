#include "string_reader.hpp"

#include <charconv>
#include <cmath>
#include <limits>

namespace ov::server::cmd {

std::optional<i64> parse_java_long(std::string_view text, i64 min, i64 max) noexcept {
    if (text.empty() || text.front() == '+') {
        return std::nullopt;
    }
    i64        value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < min || value > max) {
        return std::nullopt;
    }
    return value;
}

std::optional<f64> parse_java_double(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    // Only digits, '.', '-' reach here (the reader's number alphabet). Java
    // accepts "1.", ".5", "-.5" and refuses ".", "-", "1.2.3" and "1-2".
    usize i = 0;
    if (text[i] == '-') {
        ++i;
    }
    usize digits = 0;
    usize dots   = 0;
    for (usize j = i; j < text.size(); ++j) {
        if (text[j] == '.') {
            ++dots;
        } else if (text[j] >= '0' && text[j] <= '9') {
            ++digits;
        } else {
            return std::nullopt;
        }
    }
    if (digits == 0 || dots > 1) {
        return std::nullopt;
    }
    std::string normalised{text.substr(i)};
    if (normalised.front() == '.') {
        normalised.insert(normalised.begin(), '0');
    }
    if (normalised.back() == '.') {
        normalised.push_back('0');
    }
    f64        value = 0.0;
    const auto [ptr, ec] =
        std::from_chars(normalised.data(), normalised.data() + normalised.size(), value);
    if (ptr != normalised.data() + normalised.size() ||
        (ec != std::errc{} && ec != std::errc::result_out_of_range)) {
        return std::nullopt;
    }
    return text.front() == '-' ? -value : value;
}

void StringReader::skip_whitespace() noexcept {
    // Character.isWhitespace for the ASCII range a command can hold.
    while (can_read()) {
        const char c = peek();
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v' &&
            !(c >= 0x1C && c <= 0x1F)) {
            break;
        }
        skip();
    }
}

namespace {

template<typename T>
Parsed<T> read_integral(StringReader& reader, std::string_view expected_key,
                        std::string_view invalid_key) {
    const usize start = reader.cursor();
    while (reader.can_read() && StringReader::is_allowed_number(reader.peek())) {
        reader.skip();
    }
    const std::string_view number = reader.string().substr(start, reader.cursor() - start);
    if (number.empty()) {
        return std::unexpected{reader.error(std::string{expected_key})};
    }
    const auto value = parse_java_long(number, std::numeric_limits<T>::min(),
                                       std::numeric_limits<T>::max());
    if (!value) {
        reader.set_cursor(start);
        return std::unexpected{
            reader.error(std::string{invalid_key}, {Text::raw(std::string{number})})};
    }
    return static_cast<T>(*value);
}

}  // namespace

Parsed<i32> StringReader::read_int() {
    return read_integral<i32>(*this, "parsing.int.expected", "parsing.int.invalid");
}

Parsed<i64> StringReader::read_long() {
    return read_integral<i64>(*this, "parsing.long.expected", "parsing.long.invalid");
}

Parsed<f64> StringReader::read_double() {
    const usize start = cursor_;
    while (can_read() && is_allowed_number(peek())) {
        skip();
    }
    const std::string_view number = input_.substr(start, cursor_ - start);
    if (number.empty()) {
        return std::unexpected{error("parsing.double.expected")};
    }
    const auto value = parse_java_double(number);
    if (!value) {
        cursor_ = start;
        return std::unexpected{error("parsing.double.invalid", {Text::raw(std::string{number})})};
    }
    return *value;
}

Parsed<f32> StringReader::read_float() {
    const usize start = cursor_;
    while (can_read() && is_allowed_number(peek())) {
        skip();
    }
    const std::string_view number = input_.substr(start, cursor_ - start);
    if (number.empty()) {
        return std::unexpected{error("parsing.float.expected")};
    }
    const auto value = parse_java_double(number);
    if (!value) {
        cursor_ = start;
        return std::unexpected{error("parsing.float.invalid", {Text::raw(std::string{number})})};
    }
    // Float.parseFloat rounds the decimal once, straight to float. Going
    // through double first can differ in the last bit for a handful of
    // decimal strings; std::from_chars on float does it directly.
    std::string normalised{number.front() == '-' ? number.substr(1) : number};
    if (normalised.front() == '.') {
        normalised.insert(normalised.begin(), '0');
    }
    if (normalised.back() == '.') {
        normalised.push_back('0');
    }
    f32 direct = 0.0F;
    const auto [ptr, ec] =
        std::from_chars(normalised.data(), normalised.data() + normalised.size(), direct);
    if (ec != std::errc{} || ptr != normalised.data() + normalised.size()) {
        direct = static_cast<f32>(*value);
    } else if (number.front() == '-') {
        direct = -direct;
    }
    return direct;
}

std::string_view StringReader::read_unquoted_string() noexcept {
    const usize start = cursor_;
    while (can_read() && is_allowed_in_unquoted_string(peek())) {
        skip();
    }
    return input_.substr(start, cursor_ - start);
}

Parsed<std::string> StringReader::read_string_until(char terminator) {
    std::string out;
    bool        escaped = false;
    while (can_read()) {
        const char c = read();
        if (escaped) {
            if (c == terminator || c == '\\') {
                out.push_back(c);
                escaped = false;
            } else {
                set_cursor(cursor_ - 1);
                return std::unexpected{error("parsing.quote.escape", {Text::raw(std::string(1, c))})};
            }
        } else if (c == '\\') {
            escaped = true;
        } else if (c == terminator) {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return std::unexpected{error("parsing.quote.expected.end")};
}

Parsed<std::string> StringReader::read_quoted_string() {
    if (!can_read()) {
        return std::string{};
    }
    const char next = peek();
    if (!is_quoted_string_start(next)) {
        return std::unexpected{error("parsing.quote.expected.start")};
    }
    skip();
    return read_string_until(next);
}

Parsed<std::string> StringReader::read_string() {
    if (!can_read()) {
        return std::string{};
    }
    const char next = peek();
    if (is_quoted_string_start(next)) {
        skip();
        return read_string_until(next);
    }
    return std::string{read_unquoted_string()};
}

Parsed<bool> StringReader::read_boolean() {
    const usize start = cursor_;
    auto        value = read_string();
    if (!value) {
        return std::unexpected{value.error()};
    }
    if (value->empty()) {
        return std::unexpected{error("parsing.bool.expected")};
    }
    if (*value == "true") {
        return true;
    }
    if (*value == "false") {
        return false;
    }
    cursor_ = start;
    return std::unexpected{error("parsing.bool.invalid", {Text::raw(*value)})};
}

Parsed<void> StringReader::expect(char c) {
    if (!can_read() || peek() != c) {
        return std::unexpected{error("parsing.expected", {Text::raw(std::string(1, c))})};
    }
    skip();
    return {};
}

}  // namespace ov::server::cmd
