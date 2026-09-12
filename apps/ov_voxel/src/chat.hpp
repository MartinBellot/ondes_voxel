// The chat, wired: packets in, keys in, packets out.
//
// ov_client's chat (log, box, completion, titles) knows neither packets nor
// sockets; ov_netclient knows packets and no widget. This is the one place
// that knows both, and it is small on purpose: it decorates a Player Chat
// Message with the chat type the codec named, turns keys into Chat Message,
// Chat Command and Command Suggestions Request, and hands everything else on.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/chat.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/window.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/chat_types.hpp"
#include "ov/render/font.hpp"
#include "ov/render/language.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::demo {

class ChatUi {
public:
    /// Take what the server said this poll. `font` wraps the lines.
    void apply(const netclient::ClientEvents& events, const render::Font& font,
               const render::Language& language);

    /// Keys, characters and the wheel. True when the chat took them — open,
    /// or opened by this very frame's T or slash, whose character is then not
    /// typed. `screen_open`: T and / do nothing over another screen.
    bool update(const client::InputState& input, netclient::Client& client, client::Window& window,
                f64 delta_seconds, bool screen_open);

    void draw(client::Gui& gui);

    [[nodiscard]] bool open() const noexcept { return open_; }

    // ── Scripted, through the same paths the keys take ──────────────────────
    void open_box(std::string_view initial, client::Window* window);
    void type(std::string_view text, netclient::Client& client);
    /// Put `text` in the box and press Enter.
    void submit(std::string_view text, netclient::Client& client, client::Window* window);

    /// Every message, oldest first, as plain text.
    [[nodiscard]] std::vector<std::string> transcript() const { return log_.plain_lines(); }

    /// The Commands packet as it arrived, re-encoded (the encoder is
    /// round-tripped byte for byte in test_chat.cpp of ov_protocol).
    [[nodiscard]] const std::optional<net::CommandGraphWire>& commands() const noexcept {
        return commands_;
    }

    [[nodiscard]] const client::ChatInput& input() const noexcept { return input_; }

    /// Ticks since the newest line arrived, −1 when there is none — so a fade
    /// capture states the age it shows instead of assuming it.
    [[nodiscard]] i32 newest_age() const noexcept {
        return log_.lines().empty() ? -1 : ticks_ - log_.lines().front().added_tick;
    }

    /// ── hud ── the title's remaining ticks, as the real client's titleTime.
    [[nodiscard]] i32 title_time() const noexcept { return titles_.title_time(); }

private:
    void run(const client::ChatAction& action, netclient::Client& client, client::Window* window);
    void close(client::Window* window);
    void add(std::string_view json, const render::Font& font, const render::Language& language);
    [[nodiscard]] std::optional<std::string> decorate(i32 chat_type, const std::string& sender,
                                                      const std::optional<std::string>& target,
                                                      const std::string& content) const;

    std::vector<net::ChatDecoration>     decorations_;
    std::optional<net::CommandGraphWire> commands_;
    client::ChatLog                      log_;
    client::ChatInput                    input_;
    client::TitleOverlay                 titles_;
    std::vector<std::string>             history_;
    std::string                          clipboard_;
    bool                                 open_{false};
    /// Wall time, for the fade and the cursor blink only: a rendering clock,
    /// never the simulation's.
    f64 clock_{0.0};
    i32 ticks_{0};
    f64 opened_at_{0.0};
};

}  // namespace ov::demo
