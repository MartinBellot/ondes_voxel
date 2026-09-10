// The command reader, and the error every parser in the engine raises.
//
// Brigadier's reader, re-specified from its observable behaviour: which
// characters a number or an unquoted word may hold, how a quoted string
// escapes, and — the part vanilla's error lines depend on — **where the cursor
// is left** when something fails. `setblock 1.5 -60 0 stone` puts the cursor
// back on "1.5"; `gamemode foo` leaves it after "foo". Both are in the capture,
// and both come out of this file.
#pragma once

#include "text.hpp"

#include "ov/base/types.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ov::server::cmd {

/// Brigadier's CommandSyntaxException: a message, and — when the error has a
/// place — the input and the cursor, which is what the second, gray
/// `…<--[HERE]` line is drawn from.
struct CommandError {
    Text                 message;
    std::optional<usize> cursor;
    std::string          input;

    [[nodiscard]] static CommandError plain(Text message) {
        return CommandError{std::move(message), std::nullopt, {}};
    }
};

template<typename T>
using Parsed = std::expected<T, CommandError>;

class StringReader {
public:
    explicit StringReader(std::string_view input, usize cursor = 0)
        : input_{input}, cursor_{cursor} {}

    [[nodiscard]] std::string_view string() const noexcept { return input_; }
    [[nodiscard]] usize            cursor() const noexcept { return cursor_; }
    void                           set_cursor(usize cursor) noexcept { cursor_ = cursor; }

    [[nodiscard]] bool can_read(usize length = 1) const noexcept {
        return cursor_ + length <= input_.size();
    }
    [[nodiscard]] char peek(usize offset = 0) const noexcept {
        return cursor_ + offset < input_.size() ? input_[cursor_ + offset] : '\0';
    }
    char read() noexcept { return cursor_ < input_.size() ? input_[cursor_++] : '\0'; }
    void skip() noexcept {
        if (cursor_ < input_.size()) {
            ++cursor_;
        }
    }
    [[nodiscard]] std::string_view remaining() const noexcept { return input_.substr(cursor_); }
    [[nodiscard]] usize remaining_length() const noexcept { return input_.size() - cursor_; }

    [[nodiscard]] static bool is_allowed_number(char c) noexcept {
        return (c >= '0' && c <= '9') || c == '.' || c == '-';
    }
    [[nodiscard]] static bool is_quoted_string_start(char c) noexcept {
        return c == '"' || c == '\'';
    }
    [[nodiscard]] static bool is_allowed_in_unquoted_string(char c) noexcept {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               c == '_' || c == '-' || c == '.' || c == '+';
    }

    void skip_whitespace() noexcept;

    [[nodiscard]] Parsed<i32>  read_int();
    [[nodiscard]] Parsed<i64>  read_long();
    [[nodiscard]] Parsed<f64>  read_double();
    [[nodiscard]] Parsed<f32>  read_float();
    [[nodiscard]] std::string_view read_unquoted_string() noexcept;
    [[nodiscard]] Parsed<std::string> read_quoted_string();
    [[nodiscard]] Parsed<std::string> read_string_until(char terminator);
    [[nodiscard]] Parsed<std::string> read_string();
    [[nodiscard]] Parsed<bool> read_boolean();
    [[nodiscard]] Parsed<void> expect(char c);

    /// An error at the current cursor, in this reader's input.
    [[nodiscard]] CommandError error(Text message) const {
        return CommandError{std::move(message), cursor_, std::string{input_}};
    }
    [[nodiscard]] CommandError error(std::string key, std::vector<Text> with = {}) const {
        return error(Text::translatable(std::move(key), std::move(with)));
    }

private:
    std::string_view input_;
    usize            cursor_{0};
};

/// Parse a whole Java integer literal: an optional '-', then digits. Nullopt
/// for anything Java's `parseInt` / `parseLong` refuses, overflow included.
[[nodiscard]] std::optional<i64> parse_java_long(std::string_view text, i64 min, i64 max) noexcept;

/// Java's `Double.parseDouble` for the characters a number argument may hold.
[[nodiscard]] std::optional<f64> parse_java_double(std::string_view text) noexcept;

}  // namespace ov::server::cmd
