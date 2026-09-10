#define OV_LOG_CATEGORY "chat"

#include "chat.hpp"

#include "ov/base/log.hpp"
#include "ov/render/text_component.hpp"

#include <cmath>

namespace ov::demo {

namespace {

/// A JSON string literal, for the plain body of a Player Chat Message.
std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<u8>(c) < 0x20) {
                    constexpr std::string_view kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<u8>(c) >> 4U) & 0xFU];
                    out += kHex[static_cast<u8>(c) & 0xFU];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

/// Half a second of blink, on and off: vanilla's six ticks.
constexpr f64 kBlinkSeconds = 0.3;

}  // namespace

std::optional<std::string> ChatUi::decorate(i32 chat_type, const std::string& sender,
                                            const std::optional<std::string>& target,
                                            const std::string& content) const {
    for (const net::ChatDecoration& decoration : decorations_) {
        if (decoration.id != chat_type) {
            continue;
        }
        std::string json = "{";
        if (!decoration.style_json.empty()) {
            json += decoration.style_json + ",";
        }
        json += "\"translate\":" + quote(decoration.translation_key) + ",\"with\":[";
        for (usize i = 0; i < decoration.parameters.size(); ++i) {
            const std::string& parameter = decoration.parameters[i];
            json += i == 0 ? "" : ",";
            if (parameter == "sender") {
                json += sender;
            } else if (parameter == "target") {
                json += target ? *target : "\"\"";
            } else if (parameter == "content") {
                json += content;
            } else {
                json += "\"\"";
            }
        }
        json += "]}";
        return json;
    }
    return std::nullopt;
}

void ChatUi::add(std::string_view json, const render::Font& font,
                 const render::Language& language) {
    client::RunLine runs = render::component_runs(json, language);
    OV_LOG_INFO("[CHAT] {}", render::plain_text(runs));
    log_.add(runs, ticks_, font);
}

void ChatUi::apply(const netclient::ClientEvents& events, const render::Font& font,
                   const render::Language& language) {
    if (events.chat_types) {
        decorations_ = *events.chat_types;
        OV_LOG_INFO("chat: {} chat types from the codec", decorations_.size());
    }
    if (events.commands) {
        commands_ = *events.commands;
        input_.set_commands(client::CommandTree(*events.commands));
        OV_LOG_INFO("chat: command tree of {} nodes", events.commands->nodes.size());
    }
    for (const net::SuggestionsResponse& response : events.suggestions) {
        std::vector<std::string> matches;
        matches.reserve(response.matches.size());
        for (const net::Suggestion& suggestion : response.matches) {
            matches.push_back(suggestion.text);
        }
        input_.on_suggestions(response.transaction, response.start, response.length,
                              std::move(matches));
    }
    using Kind = netclient::ClientEvents::ChatEvent::Kind;
    for (const auto& event : events.chat) {
        switch (event.kind) {
            case Kind::System:
                if (event.overlay) {
                    titles_.set_action_bar(render::component_runs(event.json, language));
                } else {
                    add(event.json, font, language);
                }
                break;
            case Kind::Player:
            case Kind::Disguised: {
                // Player: the unsigned content when the server gave one, the
                // body as a literal otherwise. Disguised: the message itself.
                const std::string content = event.kind == Kind::Disguised
                                                ? event.json
                                                : (event.unsigned_json ? *event.unsigned_json
                                                                       : quote(event.body));
                if (auto json = decorate(event.chat_type, event.sender_json, event.target_json,
                                         content)) {
                    add(*json, font, language);
                } else {
                    // Refused and named: a chat type the codec did not list
                    // would be drawn with an invented layout otherwise.
                    OV_LOG_WARN("chat type {} is not in the codec; the message is shown bare",
                                event.chat_type);
                    add(content, font, language);
                }
                break;
            }
            case Kind::Title:
                titles_.set_title(render::component_runs(event.json, language));
                break;
            case Kind::Subtitle:
                titles_.set_subtitle(render::component_runs(event.json, language));
                break;
            case Kind::ActionBar:
                titles_.set_action_bar(render::component_runs(event.json, language));
                break;
            case Kind::TitleTimes:
                titles_.set_times(event.fade_in, event.stay, event.fade_out);
                break;
            case Kind::ClearTitles:
                titles_.clear(event.reset);
                break;
        }
    }
}

void ChatUi::open_box(std::string_view initial, client::Window* window) {
    open_      = true;
    opened_at_ = clock_;
    input_.open(initial);
    if (window != nullptr) {
        window->set_cursor_captured(false);
    }
}

void ChatUi::close(client::Window* window) {
    open_ = false;
    log_.reset_scroll();
    if (window != nullptr) {
        window->set_cursor_captured(true);
    }
}

void ChatUi::run(const client::ChatAction& action, netclient::Client& client,
                 client::Window* window) {
    using Kind = client::ChatAction::Kind;
    switch (action.kind) {
        case Kind::None:
            return;
        case Kind::Close:
            close(window);
            return;
        case Kind::SendMessage:
            client.send_chat_message(action.text);
            close(window);
            return;
        case Kind::SendCommand:
            client.send_chat_command(action.text);
            close(window);
            return;
        case Kind::RequestSuggestions:
            client.send_suggestions_request(action.transaction, action.text);
            return;
    }
}

void ChatUi::type(std::string_view text, netclient::Client& client) {
    input_.type(text);
    run(input_.refresh(), client, nullptr);
}

void ChatUi::submit(std::string_view text, netclient::Client& client, client::Window* window) {
    if (!open_) {
        open_box("", window);
    }
    input_.field().set_value(text);
    run(input_.key(client::KeyEvent{client::EditKey::Enter}, clipboard_, history_), client,
        window);
}

bool ChatUi::update(const client::InputState& input, netclient::Client& client,
                    client::Window& window, f64 delta_seconds, bool screen_open) {
    clock_ += delta_seconds;
    const auto now     = static_cast<i32>(std::floor(clock_ * 20.0));
    const i32  elapsed = now - ticks_;
    ticks_             = now;
    titles_.tick(elapsed);

    if (!open_) {
        if (screen_open) {
            return false;
        }
        bool slash = false;
        for (const client::KeyEvent& event : input.key_events) {
            slash = slash || (event.key == client::EditKey::Slash && !event.repeat);
        }
        if (input.just_pressed(client::Key::Chat) || slash) {
            // The key's own character arrives in this same poll and is
            // dropped with it — measured, T opens an empty box and / a box
            // holding one slash.
            open_box(slash ? "/" : "", &window);
            return true;
        }
        return false;
    }

    if (!input.typed.empty()) {
        input_.type(input.typed);
    }
    for (const client::KeyEvent& event : input.key_events) {
        if (!open_) {
            break;
        }
        if (event.key == client::EditKey::PageUp || event.key == client::EditKey::PageDown) {
            // A page less one: measured, Page Up moves 19 of 20.
            const auto page = static_cast<i32>(client::chat_layout::kLinesOpen) - 1;
            log_.scroll(event.key == client::EditKey::PageUp ? page : -page,
                        client::chat_layout::kLinesOpen);
            continue;
        }
        if (event.control && event.key == client::EditKey::V) {
            clipboard_ = window.clipboard();
        }
        const std::string before = clipboard_;
        const client::ChatAction action = input_.key(event, clipboard_, history_);
        if (clipboard_ != before) {
            window.set_clipboard(clipboard_);
        }
        run(action, client, &window);
    }
    if (open_ && input.scroll != 0.0) {
        // Seven lines a notch, one with Shift — ChatScreen.MOUSE_SCROLL_SPEED.
        const auto notches = static_cast<i32>(std::lround(input.scroll));
        log_.scroll(notches * (input.shift_held ? 1 : client::chat_layout::kScrollLines),
                    client::chat_layout::kLinesOpen);
    }
    if (open_) {
        run(input_.refresh(), client, &window);
    }
    return true;
}

void ChatUi::draw(client::Gui& gui) {
    titles_.draw(gui);
    log_.draw(gui, open_, ticks_);
    if (open_) {
        const bool cursor_on =
            static_cast<i64>((clock_ - opened_at_) / kBlinkSeconds) % 2 == 0;
        input_.draw(gui, cursor_on);
    }
}

}  // namespace ov::demo
