// A single-line text field: the chat box, and the creative search box.
//
// Vanilla has one widget for both (its EditBox) and so does this client. What
// it does was **measured on the running 1.20.1 client** by
// scripts/chat_screen_oracle.java rather than remembered: the chat's field
// holds 256 characters and silently stops there, `§` cannot be typed at all,
// Home/End/arrows move a cursor that is a *character* index, and a field wider
// than its box scrolls so that the cursor stays visible (`displayPos` 104
// after 256 letters in an 850-pixel box). See docs/provenance/chat-client.md.
//
// Everything is UTF-8 here and every cursor is a byte offset that always sits
// on a codepoint boundary; the limit, however, is counted the way vanilla
// counts it — in UTF-16 code units, Java's `String.length()` — because that is
// also the unit the protocol's 256-character limit is written in.
//
// No GPU, no GLFW: the field is a value type the tests drive directly, and the
// keys it understands arrive as the window's KeyEvents.
#pragma once

#include "ov/base/types.hpp"

#include <string>
#include <string_view>

namespace ov::client {

struct KeyEvent;

/// What a key did to a field, so the owner knows whether to look at it again.
enum class FieldResult : u8 {
    /// Not an editing key: the owner may give it a meaning (Enter, Tab, Up…).
    Ignored,
    /// Consumed, and the text did not change (a cursor move, a copy).
    Moved,
    /// Consumed, and the text changed.
    Edited,
};

class TextField {
public:
    explicit TextField(usize max_length) : max_length_(max_length) {}

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    /// Replace the whole text, cut to the limit, cursor at the end, nothing
    /// selected — what vanilla's setValue does.
    void set_value(std::string_view utf8);

    /// Insert at the cursor, replacing the selection. Disallowed characters
    /// (below U+0020, U+007F, `§`) are dropped, and the text is cut where the
    /// limit is reached. Returns true when anything changed.
    bool insert(std::string_view utf8);

    /// The cursor and the other end of the selection, as byte offsets. Equal
    /// when nothing is selected.
    [[nodiscard]] usize cursor() const noexcept { return cursor_; }
    [[nodiscard]] usize anchor() const noexcept { return anchor_; }
    [[nodiscard]] bool  has_selection() const noexcept { return cursor_ != anchor_; }
    [[nodiscard]] std::string_view selection() const noexcept;

    /// The cursor as a character index, vanilla's `cursorPos`.
    [[nodiscard]] usize cursor_chars() const noexcept;

    /// Move by `codepoints` (negative is left). With `select`, the anchor stays.
    void move(i32 codepoints, bool select);
    /// To the start of the previous (`direction` < 0) or next word.
    void move_word(i32 direction, bool select);
    void home(bool select);
    void end(bool select);
    void select_all();

    /// Backspace (`direction` < 0) or Delete, by a character or, with `words`,
    /// to the word boundary. A selection is deleted whole instead.
    bool erase(i32 direction, bool words);

    /// The byte offset of the next word start after (or before) `from`.
    [[nodiscard]] usize word_boundary(usize from, i32 direction) const noexcept;

    /// A key, as the field reads it: arrows, Home/End, Backspace/Delete, and
    /// Ctrl (Cmd on macOS) with A, C, X, V. The clipboard is passed in rather
    /// than owned, so the tests can give it one and the window can give it the
    /// system's.
    FieldResult key(const KeyEvent& event, std::string& clipboard);

    /// Length in UTF-16 code units — the unit of the limit.
    [[nodiscard]] usize length_utf16() const noexcept;

    [[nodiscard]] usize max_length() const noexcept { return max_length_; }

    /// Keep the cursor inside a box `width` GUI pixels wide: the first byte
    /// drawn. `measure` gives the width of a UTF-8 string.
    template<typename Measure>
    usize scroll_into_view(f32 width, Measure&& measure) {
        if (display_ > value_.size() || display_ > cursor_) {
            display_ = cursor_ < display_ ? cursor_ : 0;
        }
        while (display_ < cursor_ &&
               measure(std::string_view(value_).substr(display_, cursor_ - display_)) > width) {
            display_ = next(display_);
        }
        return display_;
    }

    [[nodiscard]] usize display_start() const noexcept { return display_; }

    /// Bumped on every change of the text. An owner that derives something
    /// from the text (suggestions) compares it rather than the string.
    [[nodiscard]] u64 revision() const noexcept { return revision_; }

    /// Vanilla's rule for what may be typed into chat.
    [[nodiscard]] static bool allowed(char32_t codepoint) noexcept;

private:
    [[nodiscard]] usize next(usize at) const noexcept;
    [[nodiscard]] usize previous(usize at) const noexcept;
    void                erase_range(usize from, usize to);

    std::string value_;
    usize       max_length_;
    usize       cursor_{0};
    usize       anchor_{0};
    usize       display_{0};
    u64         revision_{0};
};

}  // namespace ov::client
