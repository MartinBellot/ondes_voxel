#include "ov/client/chat.hpp"

#include "ov/client/gui.hpp"
#include "ov/client/window.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {

namespace {

using render::StyledRun;
namespace L = chat_layout;

/// `§` in UTF-8.
constexpr std::string_view kSection = "\xC2\xA7";

// Measured colours (docs/provenance/chat-client.md § 3).
// The black backgrounds, as the alpha vanilla gives them (read off its pixels:
// the sky behind drops to half, to 0.18 under the list); see black().
constexpr f64 kLineAlpha  = 0.5;  // × the message's opacity
constexpr f64 kInputAlpha = 128.0 / 255.0;
constexpr f64 kListAlpha  = 208.0 / 255.0;
constexpr u32 kIndicator  = 0xD0D0D0;  // the 2-pixel bar left of every line
constexpr u32 kSelected         = 0xFFFF00;
constexpr u32 kUnselected       = 0xAAAAAA;
constexpr u32 kGhost            = 0x808080;
constexpr u32 kUsage            = 0xAAAAAA;
constexpr u32 kSlash            = 0xAAAAAA;
/// The chat box's width with its margins: 4 + 320 + 8.
constexpr f32 kBoxWidth = 332.0F;
constexpr f32 kIndicatorWidth = 2.0F;
constexpr f32 kTextX          = 4.0F;

/// Black at `alpha` (0..1) as vanilla blends it, for a GPU that blends in
/// linear space.
///
/// Vanilla mixes in sRGB: a pixel `s` under black at alpha a becomes s·(1−a).
/// This RHI mixes in linear light, where the same alpha leaves it far lighter
/// (the first capture: sky 0xE0 → 0xA4, not 0x70). Asking instead for
/// a' = 1 − lin(1−a) gives lin(s)·lin(1−a), which is lin(s·(1−a)) exactly for
/// s = 1 and within a level or two for the bright skies and grass a chat sits
/// on — measured, a line's background now keeps 0.498–0.502 of the sky behind
/// it where vanilla keeps 0.502.
[[nodiscard]] u32 black(f64 alpha) noexcept {
    const f64 keep   = 1.0 - std::clamp(alpha, 0.0, 1.0);
    const f64 linear = keep <= 0.04045 ? keep / 12.92 : std::pow((keep + 0.055) / 1.055, 2.4);
    return static_cast<u32>(std::lround((1.0 - linear) * 255.0)) << 24U;
}

[[nodiscard]] bool is_space(char32_t c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// Split `line` at the space at (run, byte): left keeps what is before it, the
/// space is dropped, the rest is returned.
RunLine split_at(RunLine& line, usize run, usize byte) {
    RunLine rest;
    StyledRun tail = line[run];
    tail.text      = line[run].text.substr(byte + 1);
    line[run].text.resize(byte);
    if (!tail.text.empty()) {
        rest.push_back(std::move(tail));
    }
    for (usize i = run + 1; i < line.size(); ++i) {
        rest.push_back(std::move(line[i]));
    }
    line.resize(run + 1);
    std::erase_if(line, [](const StyledRun& r) { return r.text.empty(); });
    return rest;
}

void append(RunLine& line, const StyledRun& style, std::string_view bytes) {
    if (!line.empty() && line.back().same_style(style)) {
        line.back().text += bytes;
        return;
    }
    StyledRun run = style;
    run.text      = std::string(bytes);
    line.push_back(std::move(run));
}

}  // namespace

f64 chat_opacity(i32 age_ticks) noexcept {
    // Sampled on the running client at 45 ages: 1.0 up to 180, 0.9025 at
    // 181, 0.5625 at 185, 0.25 at 190, 0.0625 at 195, 0 from 200.
    f64 d = 1.0 - static_cast<f64>(age_ticks) / static_cast<f64>(L::kFadeTicks);
    d *= 10.0;
    d = std::clamp(d, 0.0, 1.0);
    return d * d;
}

std::string normalize_chat_message(std::string_view text) {
    std::string out;
    bool        pending_space = false;
    usize       offset        = 0;
    while (offset < text.size()) {
        const usize    at        = offset;
        const char32_t codepoint = render::next_codepoint(text, offset);
        if (is_space(codepoint)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out.append(text.substr(at, offset - at));
    }
    return out;
}

std::vector<RunLine> wrap_runs(const RunLine& runs, f32 width, const render::Font& font) {
    std::vector<RunLine> lines;
    RunLine              current;
    f32                  used = 0.0F;
    // The last space of the current line: run index and byte in that run.
    bool  has_space  = false;
    usize space_run  = 0;
    usize space_byte = 0;

    const auto finish = [&]() {
        lines.push_back(std::move(current));
        current.clear();
        used      = 0.0F;
        has_space = false;
    };

    for (const StyledRun& run : runs) {
        usize offset = 0;
        while (offset < run.text.size()) {
            const usize    at        = offset;
            const char32_t codepoint = render::next_codepoint(run.text, offset);
            if (codepoint == U'\n') {
                finish();
                continue;
            }
            // A legacy code inside the text takes no room.
            if (codepoint == 0xA7U && offset < run.text.size()) {
                ++offset;
                append(current, run, std::string_view(run.text).substr(at, offset - at));
                continue;
            }
            const f32 advance = font.advance(codepoint, run.bold);
            if (used + advance > width && !current.empty()) {
                if (codepoint == U' ') {
                    // The space that overflows is the break itself, and is
                    // dropped: measured, "…and is now" (318 px) keeps its
                    // "now" although a space after it would reach 322.
                    finish();
                    continue;
                }
                if (has_space) {
                    RunLine rest = split_at(current, space_run, space_byte);
                    lines.push_back(std::move(current));
                    current   = std::move(rest);
                    used      = runs_width(current, font);
                    has_space = false;
                } else {
                    finish();
                }
            }
            append(current, run, std::string_view(run.text).substr(at, offset - at));
            used += advance;
            if (codepoint == U' ') {
                has_space  = true;
                space_run  = current.size() - 1;
                space_byte = current.back().text.size() - 1;
            }
        }
    }
    if (!current.empty() || lines.empty()) {
        lines.push_back(std::move(current));
    }
    // Every line after the first starts with one space — measured, " ious".
    for (usize i = 1; i < lines.size(); ++i) {
        lines[i].insert(lines[i].begin(), StyledRun{" "});
    }
    return lines;
}

f32 runs_width(const RunLine& runs, const render::Font& font) {
    f32 width = 0.0F;
    for (const StyledRun& run : runs) {
        usize offset = 0;
        while (offset < run.text.size()) {
            const char32_t codepoint = render::next_codepoint(run.text, offset);
            if (codepoint == 0xA7U && offset < run.text.size()) {
                // A code takes no room; bold is the run's own flag.
                ++offset;
                continue;
            }
            width += font.advance(codepoint, run.bold);
        }
    }
    return width;
}

void encode_styles(RunLine& runs) {
    for (StyledRun& run : runs) {
        std::string codes;
        const auto  code = [&codes](bool on, char c) {
            if (on) {
                codes += kSection;
                codes += c;
            }
        };
        code(run.obfuscated, 'k');
        code(run.bold, 'l');
        code(run.strikethrough, 'm');
        code(run.underlined, 'n');
        code(run.italic, 'o');
        run.text.insert(0, codes);
    }
}

f32 draw_runs(Gui& gui, f32 x, f32 y, const RunLine& runs, u32 alpha, bool shadow, f32 size) {
    f32 pen = x;
    for (const StyledRun& run : runs) {
        const u32 argb = (alpha << 24U) | (run.has_colour ? run.rgb : 0xFFFFFFU);
        pen            = size == 1.0F ? gui.text(pen, y, run.text, argb, shadow)
                                      : gui.text_scaled(pen, y, run.text, argb, size, shadow);
    }
    return pen;
}

// ── ChatLog ──────────────────────────────────────────────────────────────────

void ChatLog::add(const RunLine& message, i32 tick, const render::Font& font) {
    std::vector<RunLine> wrapped = wrap_runs(message, L::kWidth, font);
    const usize          count   = wrapped.size();
    // Newest first: the message's *last* line goes to the front, and it is
    // the one marked as the end of the entry — as the running client holds it.
    for (usize i = 0; i < count; ++i) {
        encode_styles(wrapped[i]);
        lines_.push_front(Line{std::move(wrapped[i]), tick, i + 1 == count});
    }
    message_lines_.push_front(count);
    ++messages_;
    while (messages_ > L::kMaxMessages) {
        for (usize i = 0; i < message_lines_.back() && !lines_.empty(); ++i) {
            lines_.pop_back();
        }
        message_lines_.pop_back();
        --messages_;
    }
    if (scroll_ > 0) {
        // Scrolled up: the view stays on what it was showing.
        scroll_ += static_cast<i32>(count);
        scroll(0, L::kLinesOpen);
    }
}

void ChatLog::clear() {
    lines_.clear();
    message_lines_.clear();
    messages_ = 0;
    scroll_   = 0;
}

void ChatLog::scroll(i32 lines, usize lines_per_page) {
    const i32 limit = std::max<i32>(0, static_cast<i32>(lines_.size()) -
                                           static_cast<i32>(lines_per_page));
    scroll_ = std::clamp(scroll_ + lines, 0, limit);
}

void ChatLog::draw(Gui& gui, bool focused, i32 now_tick) const {
    const usize per_page = focused ? L::kLinesOpen : L::kLinesClosed;
    const f32   bottom   = gui.height() - L::kBottomMargin;
    const usize first    = focused ? static_cast<usize>(scroll_) : 0;
    // Two passes: every background, then every line of text. The GUI cuts a
    // batch at each change of texture, and a fill and a glyph are two
    // textures — interleaved, ten lines cost twenty draws (measured: 24 with
    // the HUD); grouped, they cost two.
    for (int pass = 0; pass < 2; ++pass) {
        for (usize i = first; i < lines_.size() && i < first + per_page; ++i) {
            const Line& line    = lines_[i];
            const f64   opacity = focused ? 1.0 : chat_opacity(now_tick - line.added_tick);
            const auto  text_a  = static_cast<u32>(255.0 * opacity);
            if (text_a <= 3) {
                continue;
            }
            const f32 top = bottom - L::kLineHeight * static_cast<f32>(i - first + 1);
            if (pass == 0) {
                gui.fill(0.0F, top, kBoxWidth, L::kLineHeight, black(kLineAlpha * opacity));
                gui.fill(0.0F, top, kIndicatorWidth, L::kLineHeight, (text_a << 24U) | kIndicator);
            } else {
                (void)draw_runs(gui, kTextX, top + 1.0F, line.runs, text_a);
            }
        }
    }
    // The scroll bar, open and taller than a page: a faint strip just right of
    // the box, as long as the page is to the whole, placed where the page is.
    // ⚠️ Only its column was measured (x 332–335, barely darker than the
    // sky); its length and travel on the running client were not reproduced.
    if (focused && lines_.size() > per_page) {
        const f32 total  = static_cast<f32>(lines_.size());
        const f32 height = L::kLineHeight * static_cast<f32>(per_page);
        const f32 length = height * static_cast<f32>(per_page) / total;
        const f32 offset = height * static_cast<f32>(scroll_) / total;
        const f32 top    = bottom - offset - length;
        gui.fill(kBoxWidth, top, 2.0F, length, 0x10000000U);
    }
}

std::vector<std::string> ChatLog::plain_lines() const {
    std::vector<std::string> out;
    std::string              message;
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
        std::string text;
        for (const StyledRun& run : it->runs) {
            text += render::strip_formatting(run.text);
        }
        // A continuation line's leading space is the indent, not the text.
        message += (!message.empty() && !text.empty() && text.front() == ' ') ? text.substr(1)
                                                                             : text;
        if (it->end_of_entry) {
            out.push_back(std::move(message));
            message.clear();
        }
    }
    return out;
}

// ── ChatInput ────────────────────────────────────────────────────────────────

void ChatInput::open(std::string_view initial) {
    field_.set_value(initial);
    suggestions_     = Suggestions{};
    computed_for_    = ~0ULL;
    applied_         = false;
    want_visible_    = false;
    waiting_         = false;
    history_started_ = false;
    history_buffer_.clear();
}

void ChatInput::type(std::string_view utf8) { (void)field_.insert(utf8); }

void ChatInput::apply_suggestion(usize index) {
    if (index >= suggestions_.items.size()) {
        return;
    }
    const std::string& value  = field_.value();
    const usize        start  = std::min(suggestions_.start, value.size());
    const usize        cursor = std::max(start, field_.cursor());
    std::string next = value.substr(0, start) + suggestions_.items[index] + value.substr(cursor);
    field_.set_value(next);
    // The list stays as it was — measured, Tab keeps the same entries — so
    // the change this made is not a reason to compute it again.
    computed_for_ = field_.revision();
}

ChatAction ChatInput::key(const KeyEvent& event, std::string& clipboard,
                          std::vector<std::string>& history) {
    ChatAction action;
    const usize n = suggestions_.items.size();
    switch (event.key) {
        case EditKey::Escape:
            if (suggestions_.visible) {
                // Measured: Escape with a list open hides the list and keeps
                // the box; the second one closes it.
                suggestions_.visible = false;
                want_visible_        = false;
                return action;
            }
            action.kind = ChatAction::Kind::Close;
            return action;

        case EditKey::Enter: {
            const std::string text = normalize_chat_message(field_.value());
            if (!text.empty() && (history.empty() || history.back() != text)) {
                history.push_back(text);
            }
            if (text.empty()) {
                action.kind = ChatAction::Kind::Close;
            } else if (text.front() == '/') {
                action.kind = ChatAction::Kind::SendCommand;
                action.text = text.substr(1);
            } else {
                action.kind = ChatAction::Kind::SendMessage;
                action.text = text;
            }
            return action;
        }

        case EditKey::Tab:
            if (!suggestions_.visible) {
                want_visible_        = true;
                suggestions_.visible = n > 0;
                applied_             = false;
                return action;
            }
            if (applied_ && n > 0) {
                suggestions_.current = (suggestions_.current + 1) % n;
            }
            apply_suggestion(suggestions_.current);
            applied_ = true;
            return action;

        case EditKey::Up:
        case EditKey::Down: {
            const i32 step = event.key == EditKey::Up ? -1 : 1;
            if (suggestions_.visible && n > 0) {
                suggestions_.current = (suggestions_.current + n + static_cast<usize>(step + 1) - 1) % n;
                if (suggestions_.current < suggestions_.offset) {
                    suggestions_.offset = suggestions_.current;
                } else if (suggestions_.current >= suggestions_.offset + L::kSuggestionRows) {
                    suggestions_.offset = suggestions_.current + 1 - L::kSuggestionRows;
                }
                applied_ = false;
                return action;
            }
            if (!history_started_) {
                history_pos_     = history.size();
                history_buffer_  = field_.value();
                history_started_ = true;
            }
            if (step < 0 && history_pos_ > 0) {
                --history_pos_;
            } else if (step > 0 && history_pos_ < history.size()) {
                ++history_pos_;
            } else {
                return action;
            }
            field_.set_value(history_pos_ < history.size() ? history[history_pos_] : history_buffer_);
            // Measured: a line recalled from history shows no list.
            want_visible_        = false;
            suggestions_.visible = false;
            recalled_            = true;
            return action;
        }

        default:
            (void)field_.key(event, clipboard);
            return action;
    }
}

ChatAction ChatInput::refresh() {
    ChatAction action;
    if (field_.revision() == computed_for_) {
        return action;
    }
    const bool recalled = recalled_;
    recalled_           = false;
    computed_for_       = field_.revision();
    const std::string& value = field_.value();
    const std::string  text  = value.substr(0, field_.cursor());
    applied_             = false;
    suggestions_.items.clear();
    suggestions_.current = 0;
    suggestions_.offset  = 0;
    suggestions_.visible = false;
    if (tree_.empty() || text.empty() || text.front() != '/') {
        waiting_ = false;
        return action;
    }
    const CommandTree::Completion completion = tree_.complete(text);
    // Shown as soon as it exists, except for the bare slash (Tab shows it)
    // and for a line recalled from history.
    want_visible_        = text != "/" && !recalled;
    suggestions_.items   = completion.matches;
    suggestions_.start   = completion.start;
    suggestions_.visible = want_visible_ && !suggestions_.items.empty();
    if (completion.ask_server) {
        ++transaction_;
        waiting_           = true;
        action.kind        = ChatAction::Kind::RequestSuggestions;
        action.text        = text;
        action.transaction = transaction_;
    }
    return action;
}

void ChatInput::on_suggestions(i32 transaction, i32 start, i32 /*length*/,
                               std::vector<std::string> matches) {
    if (!waiting_ || transaction != transaction_) {
        return;
    }
    waiting_ = false;
    // The start is in characters of the request; walk the text to a byte.
    const std::string& value = field_.value();
    usize              byte  = 0;
    for (i32 i = 0; i < start && byte < value.size(); ++i) {
        (void)render::next_codepoint(value, byte);
    }
    suggestions_.items   = std::move(matches);
    suggestions_.start   = byte;
    suggestions_.current = 0;
    suggestions_.offset  = 0;
    suggestions_.visible = want_visible_ && !suggestions_.items.empty();
}

std::string ChatInput::ghost() const {
    if (!suggestions_.visible || suggestions_.items.empty() ||
        field_.cursor() != field_.value().size()) {
        return {};
    }
    const std::string& item  = suggestions_.items[suggestions_.current];
    const usize        start = std::min(suggestions_.start, field_.value().size());
    const std::string  typed = field_.value().substr(start);
    if (typed.size() > item.size() || item.compare(0, typed.size(), typed) != 0) {
        return {};
    }
    return item.substr(typed.size());
}

void ChatInput::draw(Gui& gui, bool cursor_on) {
    const render::Font& font = gui.font();
    const f32           w    = gui.width();
    const f32           h    = gui.height();
    const f32           y    = h - L::kInputBottom;
    gui.fill(2.0F, h - 14.0F, w - 4.0F, 12.0F, black(kInputAlpha));

    const std::string& value = field_.value();
    const f32          box   = w - L::kInputX;
    const usize        first = field_.scroll_into_view(
        box, [&font](std::string_view s) { return font.width(s); });
    const usize last = first + font.fit(std::string_view(value).substr(first), box);

    // The text in its colours: a command's slash and literals grey, its
    // arguments in turn, what does not parse red; chat text E0E0E0.
    std::vector<CommandTree::Span> spans;
    if (!value.empty() && value.front() == '/') {
        spans.push_back(CommandTree::Span{0, 1, kSlash});
        for (const CommandTree::Span& span : tree_.highlight(value)) {
            spans.push_back(span);
        }
    }
    const u32 base = 0xFF000000U | L::kInputTextColour;
    f32       pen  = L::kInputX;
    f32       cursor_x = pen;
    usize     at   = first;
    const auto draw_until = [&](usize end, u32 argb) {
        end = std::min(end, last);
        if (end > at) {
            if (field_.cursor() >= at && field_.cursor() <= end) {
                cursor_x = pen + font.width(std::string_view(value).substr(at, field_.cursor() - at));
            }
            pen = gui.text(pen, y, std::string_view(value).substr(at, end - at), argb);
            at  = end;
        }
    };
    for (const CommandTree::Span& span : spans) {
        if (span.to <= at) {
            continue;
        }
        draw_until(span.from, base);
        draw_until(span.to, 0xFF000000U | span.rgb);
    }
    draw_until(last, base);
    if (field_.cursor() <= first) {
        cursor_x = L::kInputX;
    } else if (field_.cursor() >= last) {
        cursor_x = pen;
    }

    // The selection. ⚠️ Vanilla inverts what is under it in blue, a blend mode
    // the RHI does not have; a translucent blue box stands in for it.
    if (field_.has_selection()) {
        const usize from = std::clamp(std::min(field_.cursor(), field_.anchor()), first, last);
        const usize to   = std::clamp(std::max(field_.cursor(), field_.anchor()), first, last);
        const f32   x0   = L::kInputX + font.width(std::string_view(value).substr(first, from - first));
        const f32   x1   = L::kInputX + font.width(std::string_view(value).substr(first, to - first));
        gui.fill(x0, y - 1.0F, x1 - x0, 10.0F, 0x800000FFU);
    }

    const std::string rest = ghost();
    if (!rest.empty()) {
        (void)gui.text(pen, y, rest, 0xFF000000U | kGhost);
    }
    if (cursor_on) {
        if (field_.cursor() == value.size()) {
            (void)gui.text(pen, y, "_", base);
        } else {
            gui.fill(cursor_x, y - 1.0F, 1.0F, 10.0F, L::kCursorBarColour);
        }
    }

    const f32 list_bottom = h - 15.0F;
    if (suggestions_.visible && !suggestions_.items.empty()) {
        const usize rows  = std::min(suggestions_.items.size() - suggestions_.offset,
                                     L::kSuggestionRows);
        f32         width = 0.0F;
        for (usize i = 0; i < rows; ++i) {
            width = std::max(width, font.width(suggestions_.items[suggestions_.offset + i]));
        }
        const usize start = std::clamp(suggestions_.start, first, value.size());
        const f32   x = L::kInputX + font.width(std::string_view(value).substr(first, start - first)) - 1.0F;
        const f32   top = list_bottom - L::kSuggestionRow * static_cast<f32>(rows);
        // Measured with 79 entries: a list that scrolls is one pixel taller
        // at each end, and the end with more entries beyond it is dotted
        // white, every other pixel.
        const bool scrolls = suggestions_.items.size() > L::kSuggestionRows;
        const f32  pad     = scrolls ? 1.0F : 0.0F;
        gui.fill(x, top - pad, width + 1.0F, L::kSuggestionRow * static_cast<f32>(rows) + 2.0F * pad,
                 black(kListAlpha));
        if (scrolls) {
            const bool above = suggestions_.offset > 0;
            const bool below = suggestions_.offset + rows < suggestions_.items.size();
            for (f32 dot = 0.0F; dot < width + 1.0F; dot += 2.0F) {
                if (above) {
                    gui.fill(x + dot, top - 1.0F, 1.0F, 1.0F, 0xFFFFFFFFU);
                }
                if (below) {
                    gui.fill(x + dot, list_bottom, 1.0F, 1.0F, 0xFFFFFFFFU);
                }
            }
        }
        for (usize i = 0; i < rows; ++i) {
            const usize index = suggestions_.offset + i;
            const u32   rgb   = index == suggestions_.current ? kSelected : kUnselected;
            (void)gui.text(x + 1.0F, top + 2.0F + L::kSuggestionRow * static_cast<f32>(i),
                           suggestions_.items[index], 0xFF000000U | rgb);
        }
        return;
    }

    // No list: an argument position shows its usage, one bar a line.
    if (!value.empty() && value.front() == '/' && field_.cursor() == value.size()) {
        const CommandTree::Usage usage = tree_.usage(value);
        const usize start = std::clamp(usage.start, first, value.size());
        const f32   x = L::kInputX + font.width(std::string_view(value).substr(first, start - first));
        for (usize i = 0; i < usage.lines.size(); ++i) {
            const f32 top = list_bottom - L::kSuggestionRow * static_cast<f32>(usage.lines.size() - i);
            gui.fill(x - 1.0F, top, font.width(usage.lines[i]) + 2.0F, L::kSuggestionRow,
                     black(kListAlpha));
            (void)gui.text(x, top + 2.0F, usage.lines[i], 0xFF000000U | kUsage);
        }
    }
}

// ── TitleOverlay ─────────────────────────────────────────────────────────────

void TitleOverlay::set_title(RunLine runs) {
    encode_styles(runs);
    title_      = std::move(runs);
    title_time_ = fade_in_ + stay_ + fade_out_;
}

void TitleOverlay::set_times(i32 fade_in, i32 stay, i32 fade_out) {
    fade_in_  = fade_in;
    stay_     = stay;
    fade_out_ = fade_out;
    if (title_time_ > 0) {
        title_time_ = fade_in_ + stay_ + fade_out_;
    }
}

void TitleOverlay::clear(bool reset) {
    title_.clear();
    subtitle_.clear();
    title_time_ = 0;
    if (reset) {
        fade_in_  = L::kTitleFadeIn;
        stay_     = L::kTitleStay;
        fade_out_ = L::kTitleFadeOut;
    }
}

void TitleOverlay::set_action_bar(RunLine runs) {
    encode_styles(runs);
    overlay_      = std::move(runs);
    overlay_time_ = L::kOverlayTicks;
}

void TitleOverlay::tick(i32 ticks) {
    if (ticks <= 0) {
        return;
    }
    overlay_time_ = std::max(0, overlay_time_ - ticks);
    if (title_time_ > 0) {
        title_time_ = std::max(0, title_time_ - ticks);
        if (title_time_ == 0) {
            title_.clear();
            subtitle_.clear();
        }
    }
}

void TitleOverlay::draw(Gui& gui) const {
    const render::Font& font = gui.font();
    // Centred the way vanilla centres: on the integer half of the screen, less
    // `size` times the integer half of the text's own width — which is why an
    // odd-width subtitle sits one GUI pixel left of a float centre.
    const f32  cx      = std::floor(gui.width() / 2.0F);
    const f32  h       = gui.height();
    const auto centred = [&](const RunLine& runs, f32 size) {
        return cx - size * std::floor(runs_width(runs, font) / 2.0F);
    };
    if (overlay_time_ > 0 && !overlay_.empty()) {
        // Opaque for 40 ticks, then over the last 20.
        const u32 alpha = static_cast<u32>(std::min(255, overlay_time_ * 255 / 20));
        if (alpha > 8) {
            // Measured in creative: the first row of ink at h − 70, which is
            // a text origin at h − 72.
            (void)draw_runs(gui, centred(overlay_, 1.0F), h - 72.0F, overlay_, alpha);
        }
    }
    if (title_time_ > 0 && !title_.empty()) {
        const i32 t     = title_time_;
        i32       alpha = 255;
        if (t > fade_out_ + stay_) {
            alpha = fade_in_ > 0 ? (fade_in_ + stay_ + fade_out_ - t) * 255 / fade_in_ : 255;
        } else if (t < fade_out_) {
            alpha = fade_out_ > 0 ? t * 255 / fade_out_ : 255;
        }
        alpha = std::clamp(alpha, 0, 255);
        if (alpha > 8) {
            const auto a  = static_cast<u32>(alpha);
            const f32  cy = std::floor(h / 2.0F);
            (void)draw_runs(gui, centred(title_, 4.0F), cy - 40.0F, title_, a, true, 4.0F);
            if (!subtitle_.empty()) {
                (void)draw_runs(gui, centred(subtitle_, 2.0F), cy + 10.0F, subtitle_, a, true, 2.0F);
            }
        }
    }
}

}  // namespace ov::client
