// The chat: the messages over the hotbar, the box you type into, the command
// completion above it, and the titles and action bar the server can put up.
//
// Every number here was **measured on the running 1.20.1 client**
// (scripts/chat_screen_oracle.java; docs/provenance/chat-client.md), not
// remembered. The ones that surprise:
//
//   • a message stays **ten seconds**, fully opaque for nine; its opacity is
//     `clamp((1 − age/200)·10, 0, 1)²` in ticks, sampled at 45 ages and matched
//     exactly — the wiki's "3 seconds" is not 1.20.1;
//   • the chat is 320 GUI pixels wide, 10 lines closed and 20 open, 9 pixels a
//     line, 40 pixels above the bottom; 100 messages are kept;
//   • a line is broken at a space (the space is dropped), a word longer than
//     the line is cut, and every continuation line starts with one space;
//   • the wheel scrolls 7 lines, Page Up 19 (a page less one);
//   • what is sent is trimmed and its runs of spaces collapsed, and the
//     history keeps that — commands with their slash;
//   • T and / open the box and their own character is never typed into it;
//     Enter does *not* open it (vanilla's Java edition; the wiki's "or Return"
//     is Bedrock's).
//
// Nothing here knows a packet or a GPU. The application feeds components in
// and calls draw(); the tests drive the rest directly.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/command_suggestions.hpp"
#include "ov/client/text_field.hpp"
#include "ov/render/font.hpp"
#include "ov/render/text_component.hpp"

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

class Gui;
struct KeyEvent;

namespace chat_layout {
/// ChatComponent.MAX_CHAT_HISTORY.
inline constexpr usize kMaxMessages = 100;
/// getWidth() with the default "Width" of 320 px and scale 1.
inline constexpr f32 kWidth = 320.0F;
/// getLinesPerPage(), closed and open.
inline constexpr usize kLinesClosed = 10;
inline constexpr usize kLinesOpen   = 20;
/// getLineHeight() at line spacing 0.
inline constexpr f32 kLineHeight = 9.0F;
/// ChatComponent.BOTTOM_MARGIN: the bottom of the lowest line above the
/// screen's bottom edge.
inline constexpr f32 kBottomMargin = 40.0F;
/// A message's life, in ticks.
inline constexpr i32 kFadeTicks = 200;
/// ChatScreen.MOUSE_SCROLL_SPEED.
inline constexpr i32 kScrollLines = 7;
/// The box: EditBox maxLength and its text colour.
inline constexpr usize kMaxInput        = 256;
inline constexpr u32   kInputTextColour = 0xE0E0E0;
/// EditBox.CURSOR_INSERT_COLOR, the bar drawn when the cursor is mid-text.
inline constexpr u32 kCursorBarColour = 0xFFD0D0D0;
/// The box is at (4, height − 12), as wide as the screen less 4.
inline constexpr f32 kInputX      = 4.0F;
inline constexpr f32 kInputBottom = 12.0F;
/// The suggestion list: 12-pixel rows, at most 10 shown, 3 pixels over the box.
inline constexpr f32   kSuggestionRow = 12.0F;
inline constexpr usize kSuggestionRows = 10;
inline constexpr f32   kSuggestionGap = 3.0F;
/// The action bar, in ticks, and the last ticks it fades over.
inline constexpr i32 kOverlayTicks = 60;
/// Title times when the server sets none: fade in, stay, fade out.
inline constexpr i32 kTitleFadeIn  = 10;
inline constexpr i32 kTitleStay    = 70;
inline constexpr i32 kTitleFadeOut = 20;
}  // namespace chat_layout

/// The opacity of a message `age_ticks` old, closed chat.
[[nodiscard]] f64 chat_opacity(i32 age_ticks) noexcept;

/// Trim and collapse runs of whitespace — what is sent and what history keeps.
[[nodiscard]] std::string normalize_chat_message(std::string_view text);

/// A line of runs.
using RunLine = std::vector<render::StyledRun>;

/// Break a message into lines at most `width` GUI pixels wide, as vanilla
/// does (see the header). Continuation lines start with one space, which is
/// not counted in the width they were broken to.
[[nodiscard]] std::vector<RunLine> wrap_runs(const RunLine& runs, f32 width,
                                             const render::Font& font);

/// Fold each run's bold/italic/underline/strikethrough/obfuscated into `§`
/// codes at the front of its text, which is what the GUI's text path draws.
/// The colour stays in `rgb`, exact. Done once when a line is stored, so that
/// drawing it allocates nothing.
void encode_styles(RunLine& runs);

/// Draw an *encoded* line of runs at (x, y); returns the pen after it.
/// `alpha` is 0..255; a run without a colour is white.
f32 draw_runs(Gui& gui, f32 x, f32 y, const RunLine& runs, u32 alpha, bool shadow = true,
              f32 size = 1.0F);

/// The width of a line of runs, bold included.
[[nodiscard]] f32 runs_width(const RunLine& runs, const render::Font& font);

// ── The messages ─────────────────────────────────────────────────────────────

class ChatLog {
public:
    struct Line {
        RunLine runs;
        i32     added_tick{0};
        bool    end_of_entry{false};
    };

    /// A message, wrapped, at tick `tick`. The oldest go once there are 100.
    void add(const RunLine& message, i32 tick, const render::Font& font);
    void clear();

    /// Newest first, as they are drawn from the bottom up.
    [[nodiscard]] const std::deque<Line>& lines() const noexcept { return lines_; }
    [[nodiscard]] usize                   message_count() const noexcept { return messages_; }

    /// Scroll by whole lines, clamped; positive is towards older lines.
    void scroll(i32 lines, usize lines_per_page);
    void reset_scroll() noexcept { scroll_ = 0; }
    [[nodiscard]] i32 scroll_position() const noexcept { return scroll_; }

    /// Closed (`focused` false): the last 10 lines younger than 10 s, fading.
    /// Open: 20 lines from the scroll position, opaque.
    void draw(Gui& gui, bool focused, i32 now_tick) const;

    /// The plain text of every message, oldest first — for the scripted checks.
    [[nodiscard]] std::vector<std::string> plain_lines() const;

private:
    std::deque<Line> lines_;
    /// Messages, so that the 100-message limit is on messages and not lines.
    std::deque<usize> message_lines_;
    usize             messages_{0};
    i32               scroll_{0};
};

// ── The box, and the completion above it ─────────────────────────────────────

/// What the owner must do after a key.
struct ChatAction {
    enum class Kind : u8 { None, Close, SendMessage, SendCommand, RequestSuggestions };
    Kind        kind{Kind::None};
    /// SendMessage: the normalized text. SendCommand: without the slash.
    /// RequestSuggestions: the text up to the cursor, slash included.
    std::string text;
    i32         transaction{0};
};

class ChatInput {
public:
    ChatInput() : field_(chat_layout::kMaxInput) {}

    /// Open with `initial` ("" for T, "/" for the slash key).
    void open(std::string_view initial);

    [[nodiscard]] TextField&       field() noexcept { return field_; }
    [[nodiscard]] const TextField& field() const noexcept { return field_; }

    void set_commands(CommandTree tree) { tree_ = std::move(tree); }
    [[nodiscard]] const CommandTree& commands() const noexcept { return tree_; }

    /// Characters typed while open.
    void type(std::string_view utf8);

    /// A key while open. `history` is what was sent, oldest first.
    ChatAction key(const KeyEvent& event, std::string& clipboard,
                   std::vector<std::string>& history);

    /// Call after any change: recomputes the local completion, and returns a
    /// request when the server must be asked.
    ChatAction refresh();

    /// A Command Suggestions Response. Ignored unless it answers the last
    /// request.
    void on_suggestions(i32 transaction, i32 start, i32 length, std::vector<std::string> matches);

    // What draw() shows.
    struct Suggestions {
        bool                     visible{false};
        usize                    start{0};
        std::vector<std::string> items;
        usize                    current{0};
        usize                    offset{0};
    };
    [[nodiscard]] const Suggestions& suggestions() const noexcept { return suggestions_; }

    /// The grey remainder of the current suggestion, drawn after the text.
    [[nodiscard]] std::string ghost() const;

    /// Draw the box, its text, cursor, ghost, suggestions and usage. Not
    /// const: it keeps the field scrolled so the cursor stays in the box.
    void draw(Gui& gui, bool cursor_on);

    /// The history index, vanilla's historyPos.
    [[nodiscard]] usize history_position() const noexcept { return history_pos_; }

private:
    void apply_suggestion(usize index);

    TextField   field_;
    CommandTree tree_;
    Suggestions suggestions_;
    /// The field revision the list was computed for, and whether Tab has
    /// already applied the current entry (the next Tab moves on).
    u64  computed_for_{~0ULL};
    bool applied_{false};
    /// Shown as soon as it arrives — false only for the bare slash, which
    /// vanilla lists on Tab alone.
    bool want_visible_{false};
    i32  transaction_{0};
    bool waiting_{false};
    usize       history_pos_{0};
    std::string history_buffer_;
    bool        history_started_{false};
    /// Set by ↑/↓ through the history, consumed by the next refresh(): a
    /// recalled line shows no list (measured), a typed one does.
    bool recalled_{false};
};

// ── The action bar and the titles ────────────────────────────────────────────

class TitleOverlay {
public:
    void set_title(RunLine runs);
    void set_subtitle(RunLine runs) { subtitle_ = std::move(runs); }
    void set_times(i32 fade_in, i32 stay, i32 fade_out);
    void clear(bool reset);
    void set_action_bar(RunLine runs);

    /// Advance by whole ticks.
    void tick(i32 ticks);

    /// ⚠️ The action bar's height was measured in creative only (h − 70).
    void draw(Gui& gui) const;

    [[nodiscard]] i32 title_time() const noexcept { return title_time_; }
    [[nodiscard]] i32 action_bar_time() const noexcept { return overlay_time_; }

private:
    RunLine title_;
    RunLine subtitle_;
    RunLine overlay_;
    i32     fade_in_{chat_layout::kTitleFadeIn};
    i32     stay_{chat_layout::kTitleStay};
    i32     fade_out_{chat_layout::kTitleFadeOut};
    i32     title_time_{0};
    i32     overlay_time_{0};
};

}  // namespace ov::client
