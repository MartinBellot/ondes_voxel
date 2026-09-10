#include "ov/client/text_field.hpp"

#include "ov/client/window.hpp"
#include "ov/render/font.hpp"

#include <algorithm>

namespace ov::client {

namespace {

[[nodiscard]] bool continuation(char c) noexcept {
    return (static_cast<u8>(c) & 0xC0U) == 0x80U;
}

/// UTF-16 code units of one codepoint: two above the BMP, one otherwise.
[[nodiscard]] usize utf16_units(char32_t codepoint) noexcept {
    return codepoint >= 0x10000U ? 2 : 1;
}

[[nodiscard]] bool is_space(char c) noexcept { return c == ' '; }

}  // namespace

bool TextField::allowed(char32_t codepoint) noexcept {
    // Measured: `é` goes in, `§` does not (the chat oracle typed "é§x" and
    // the box held "éx"). Control characters and DEL never arrive as text.
    return codepoint >= 0x20U && codepoint != 0x7FU && codepoint != 0xA7U;
}

usize TextField::next(usize at) const noexcept {
    if (at >= value_.size()) {
        return value_.size();
    }
    ++at;
    while (at < value_.size() && continuation(value_[at])) {
        ++at;
    }
    return at;
}

usize TextField::previous(usize at) const noexcept {
    if (at == 0) {
        return 0;
    }
    --at;
    while (at > 0 && continuation(value_[at])) {
        --at;
    }
    return at;
}

usize TextField::length_utf16() const noexcept {
    usize units  = 0;
    usize offset = 0;
    while (offset < value_.size()) {
        units += utf16_units(render::next_codepoint(value_, offset));
    }
    return units;
}

usize TextField::cursor_chars() const noexcept {
    usize count  = 0;
    usize offset = 0;
    while (offset < cursor_) {
        count += utf16_units(render::next_codepoint(value_, offset));
    }
    return count;
}

std::string_view TextField::selection() const noexcept {
    const usize from = std::min(cursor_, anchor_);
    const usize to   = std::max(cursor_, anchor_);
    return std::string_view(value_).substr(from, to - from);
}

void TextField::set_value(std::string_view utf8) {
    value_.clear();
    cursor_  = 0;
    anchor_  = 0;
    display_ = 0;
    (void)insert(utf8);
    ++revision_;
}

void TextField::erase_range(usize from, usize to) {
    if (from >= to) {
        return;
    }
    value_.erase(from, to - from);
    cursor_ = from;
    anchor_ = from;
    ++revision_;
}

bool TextField::insert(std::string_view utf8) {
    const bool had_selection = has_selection();
    if (had_selection) {
        erase_range(std::min(cursor_, anchor_), std::max(cursor_, anchor_));
    }
    // What is allowed, and only as much of it as the limit leaves room for.
    // A codepoint that does not fit whole is not split: the text is cut
    // before it, which is what an over-long paste gets in vanilla too.
    usize       room = max_length_ > length_utf16() ? max_length_ - length_utf16() : 0;
    std::string accepted;
    usize       offset = 0;
    while (offset < utf8.size() && room > 0) {
        const char32_t codepoint = render::next_codepoint(utf8, offset);
        if (!allowed(codepoint)) {
            continue;
        }
        const usize units = utf16_units(codepoint);
        if (units > room) {
            break;
        }
        room -= units;
        render::append_utf8(accepted, codepoint);
    }
    if (accepted.empty()) {
        return had_selection;
    }
    value_.insert(cursor_, accepted);
    cursor_ += accepted.size();
    anchor_ = cursor_;
    ++revision_;
    return true;
}

void TextField::move(i32 codepoints, bool select) {
    if (!select && has_selection() && codepoints != 0) {
        // An arrow with a selection and no shift collapses it to that side.
        cursor_ = codepoints < 0 ? std::min(cursor_, anchor_) : std::max(cursor_, anchor_);
        anchor_ = cursor_;
        return;
    }
    for (i32 i = 0; i < codepoints; ++i) {
        cursor_ = next(cursor_);
    }
    for (i32 i = 0; i > codepoints; --i) {
        cursor_ = previous(cursor_);
    }
    if (!select) {
        anchor_ = cursor_;
    }
}

usize TextField::word_boundary(usize from, i32 direction) const noexcept {
    usize at = std::min(from, value_.size());
    if (direction > 0) {
        // Past the rest of this word, then past the spaces after it: the
        // start of the next word.
        while (at < value_.size() && !is_space(value_[at])) {
            at = next(at);
        }
        while (at < value_.size() && is_space(value_[at])) {
            at = next(at);
        }
        return at;
    }
    // Back over spaces, then back to the start of the word before them.
    while (at > 0 && is_space(value_[previous(at)])) {
        at = previous(at);
    }
    while (at > 0 && !is_space(value_[previous(at)])) {
        at = previous(at);
    }
    return at;
}

void TextField::move_word(i32 direction, bool select) {
    cursor_ = word_boundary(cursor_, direction);
    if (!select) {
        anchor_ = cursor_;
    }
}

void TextField::home(bool select) {
    cursor_ = 0;
    if (!select) {
        anchor_ = cursor_;
    }
}

void TextField::end(bool select) {
    cursor_ = value_.size();
    if (!select) {
        anchor_ = cursor_;
    }
}

void TextField::select_all() {
    anchor_ = 0;
    cursor_ = value_.size();
}

bool TextField::erase(i32 direction, bool words) {
    if (has_selection()) {
        erase_range(std::min(cursor_, anchor_), std::max(cursor_, anchor_));
        return true;
    }
    const usize target = words ? word_boundary(cursor_, direction)
                               : (direction < 0 ? previous(cursor_) : next(cursor_));
    const usize from = std::min(cursor_, target);
    const usize to   = std::max(cursor_, target);
    if (from == to) {
        return false;
    }
    erase_range(from, to);
    return true;
}

FieldResult TextField::key(const KeyEvent& event, std::string& clipboard) {
    const bool shift   = event.shift;
    const bool control = event.control;
    switch (event.key) {
        case EditKey::Left:
            if (control) {
                move_word(-1, shift);
            } else {
                move(-1, shift);
            }
            return FieldResult::Moved;
        case EditKey::Right:
            if (control) {
                move_word(1, shift);
            } else {
                move(1, shift);
            }
            return FieldResult::Moved;
        case EditKey::Home:
            home(shift);
            return FieldResult::Moved;
        case EditKey::End:
            end(shift);
            return FieldResult::Moved;
        case EditKey::Backspace:
            return erase(-1, control) ? FieldResult::Edited : FieldResult::Moved;
        case EditKey::Delete:
            return erase(1, control) ? FieldResult::Edited : FieldResult::Moved;
        case EditKey::A:
            if (!control) {
                return FieldResult::Ignored;
            }
            select_all();
            return FieldResult::Moved;
        case EditKey::C:
            if (!control) {
                return FieldResult::Ignored;
            }
            clipboard = std::string(selection());
            return FieldResult::Moved;
        case EditKey::X:
            if (!control) {
                return FieldResult::Ignored;
            }
            clipboard = std::string(selection());
            return erase(-1, false) ? FieldResult::Edited : FieldResult::Moved;
        case EditKey::V:
            if (!control) {
                return FieldResult::Ignored;
            }
            return insert(clipboard) ? FieldResult::Edited : FieldResult::Moved;
        default:
            return FieldResult::Ignored;
    }
}

}  // namespace ov::client
