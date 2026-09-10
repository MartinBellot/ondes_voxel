// The client's interface: the HUD, the screens, and the state behind them.
//
// This is where the protocol meets the renderer, and it lives in the
// application rather than in ov_client on purpose. ov_client draws widgets and
// knows nothing about a packet; ov_netclient reads packets and knows nothing
// about a widget. The thing that mirrors the server's windows and turns a
// mouse click into a Click Container is neither, and putting it in either would
// give one of them a dependency it does not need.
//
// **The mirror is a mirror.** Every slot drawn here was last written by a
// packet from the server. A click sends Click Container and then *nothing
// happens* until the server answers — no local move, no optimistic swap, no
// reconciliation. That is not caution, it is the only way a window can be
// right: the server applies the six click modes authoritatively, and a client
// that predicted them would be right almost always and lose an item the rest of
// the time.
//
// The one exception is stated rather than hidden: in creative, Set Creative
// Slot is client-authoritative by design and the server sends nothing back, so
// what this client puts in a slot itself is what it shows. See note_creative().
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/container_screen.hpp"
#include "ov/client/creative_screen.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/hud.hpp"
#include "ov/client/item_view.hpp"
#include "ov/client/window.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/creative_tabs.hpp"
#include "ov/render/item_model.hpp"
#include "ov/render/language.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::demo {

struct InterfaceOptions {
    /// Zero means vanilla's automatic rule.
    u32 gui_scale{0};
    /// Draw the HUD at all. A measuring switch: the frame cost of the interface
    /// is the difference between this on and off on the same scene.
    bool hud{true};
    std::string language{"en_us"};
    /// The creative catalogue, produced by scripts/measure_creative_tabs.py.
    /// Absent is not fatal: the creative screen is then refused and named,
    /// because a screen with invented tabs is worse than no screen.
    std::string creative_tabs{"data/vanilla/1.20.1/creative_tabs.json"};
};

class Interface {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Interface>, std::string> create(
        rhi::Device& device, rhi::Format colour_format, const render::AssetSource& assets,
        const render::ItemModelCache& items, rhi::ImageHandle atlas, u32 atlas_width,
        u32 atlas_height, const registry::Registries* registries, u32 foliage_tint,
        const InterfaceOptions& options);

    Interface(const Interface&)            = delete;
    Interface& operator=(const Interface&) = delete;
    ~Interface();

    /// Take everything the server said this poll.
    void apply(const netclient::ClientEvents& events);

    /// Handle input. Returns true when a screen swallowed it, in which case the
    /// caller must not move the camera or break a block with the same click.
    bool update(const client::InputState& input, netclient::Client& client,
                client::Window& window, f64 delta_seconds);

    /// Record the interface. Inside its own render pass, after the terrain's.
    void draw(rhi::CommandList& cmd, u32 framebuffer_width, u32 framebuffer_height);

    [[nodiscard]] bool screen_open() const noexcept { return screen_.has_value(); }

    /// Put the player's own inventory up, or take it down. Window 0 is never
    /// opened by the server: the client decides to show it.
    void toggle_inventory(client::Window& window, netclient::Client& client);

    /// Remember a Set Creative Slot this client sent. See the header comment.
    void note_creative(i16 slot, i32 item_id, i8 count);

    // ── The creative inventory ──────────────────────────────────────────────
    //
    // Its own screen rather than a ContainerScreen, because it is not a
    // container: its cells are a catalogue, not slots, and no click on one
    // names a number to the server. What reaches the server is Set Creative
    // Slot, and only when a stack lands in the player's own inventory.

    /// True when the catalogue loaded. False means the screen is refused.
    [[nodiscard]] bool creative_available() const noexcept {
        return creative_screen_.has_value();
    }

    [[nodiscard]] bool creative_open() const noexcept { return creative_visible_; }

    /// Put the creative inventory up, or take it down. Creative only: in
    /// survival the server ignores Set Creative Slot, so the screen would be a
    /// catalogue that hands out nothing.
    void toggle_creative(client::Window& window, netclient::Client& client);

    /// Select a tab by registry id, for the scripted captures. False when
    /// there is no such tab.
    [[nodiscard]] bool select_creative_tab(std::string_view id);

    /// Type into the search field, for the scripted captures.
    void creative_search(std::string_view text);

    /// Take the stack in a visible cell onto the cursor, exactly as a left
    /// click does. False when the cell is past the end of the page.
    [[nodiscard]] bool creative_take(i32 cell);

    /// Put what the cursor holds into a window-0 slot and tell the server.
    void creative_put(netclient::Client& client, i16 slot);

    /// A one-line description of the creative page, for the scripted checks.
    [[nodiscard]] std::string describe_creative() const;

    /// Which hotbar slot is selected, 0..8.
    [[nodiscard]] i32 selected() const noexcept { return hud_.selected; }

    [[nodiscard]] const client::GuiStats& stats() const noexcept;

    [[nodiscard]] const client::HudState& hud() const noexcept { return hud_; }

    /// A one-line description of the open window, for the scripted checks.
    [[nodiscard]] std::string describe_window() const;

    /// Window 0, as a line per non-empty slot. The round-trip proof reads this.
    [[nodiscard]] std::string describe_inventory() const;

    /// Send one click, as if the player had made it. For the scripted checks:
    /// the same path a mouse takes, so what it proves is what a player would
    /// get.
    void click_slot(netclient::Client& client, i16 slot, i32 button, bool shift, i32 hotbar_key);

    /// Spread what the cursor holds over a run of slots — mode 5, in its three
    /// phases: start on slot −999, one packet per slot, end on slot −999.
    ///
    /// The server divides the stack evenly and keeps the remainder on the
    /// cursor; sixty-four stone over three slots is 21, 21, 21 and one left in
    /// hand. Used by the gesture in update() and by the scripted check, so what
    /// the script proves is what a hand on the mouse gets.
    void drag_over(netclient::Client& client, std::span<const i16> slots, bool right);

    /// The window id the server last opened, or 0 for the player's own.
    [[nodiscard]] u8 window_id() const noexcept;

    [[nodiscard]] const std::vector<net::ItemStack>& window_slots() const noexcept {
        return window_slots_;
    }

    /// Close whatever is open, telling the server.
    void close(client::Window& window, netclient::Client& client);

private:
    Interface() = default;

    /// The registry name of an item id, or empty.
    [[nodiscard]] std::string_view item_name(i32 item_id) const noexcept;

    /// The creative screen's own input. Split out because it shares nothing
    /// with the container path: no click mode, no state id, no waiting.
    bool update_creative(const client::InputState& input, netclient::Client& client,
                         client::Window& window);

    /// Refill `views_` from a slot vector, for drawing.
    void build_views(const std::vector<net::ItemStack>& slots);

    void refresh_hotbar();

    std::unique_ptr<client::Gui> gui_;
    client::HudTextures          textures_;
    std::optional<client::ItemRenderer> items_;
    render::Language                    language_;
    const registry::Registries*         registries_{nullptr};
    std::optional<registry::RegistryId> item_registry_;

    InterfaceOptions options_;

    client::HudState hud_;

    /// Window 0, always 46 slots. The player's inventory, whatever else is open.
    std::vector<net::ItemStack> inventory_;
    /// The open window's slots, when one is open.
    std::vector<net::ItemStack> window_slots_;
    /// What the cursor holds, as the server last said.
    net::ItemStack carried_;
    /// The last state id the server sent for the open window. Sent back with
    /// every click: a stale one makes a server resynchronise instead of acting.
    i32 state_id_{0};

    std::optional<client::ContainerScreen> screen_;

    /// The catalogue, and the screen that reads it. Both absent when the file
    /// is missing, which is reported once rather than drawn as an empty page.
    std::optional<render::CreativeTabs>   creative_tabs_;
    std::optional<client::CreativeScreen> creative_screen_;
    client::GuiTexture                    creative_sheet_{client::GuiTexture::Invalid};
    bool                                  creative_visible_{false};
    /// True while the scrollbar handle is being dragged.
    bool creative_scrolling_{false};
    /// Set when the screen is the player's own inventory rather than one the
    /// server opened, because closing it must not send Close Container for a
    /// window the server never opened.
    bool own_inventory_{false};
    client::GuiTexture background_{client::GuiTexture::Invalid};
    /// Backgrounds, by resource location, uploaded on first use.
    std::vector<std::pair<std::string, client::GuiTexture>> backgrounds_;

    /// Reused every frame. A vector built inside draw() would allocate in the
    /// frame, which is the one thing the tick and the frame both forbid.
    std::vector<client::ItemStackView> views_;

    /// Mouse position in GUI pixels, from the last update.
    f32 mouse_x_{0.0F};
    f32 mouse_y_{0.0F};

    // ── The drag gesture ────────────────────────────────────────────────────
    //
    // A press with a full cursor does not act at once: vanilla waits to see
    // whether the pointer moves to a second slot. It it does, the whole thing
    // is a drag (mode 5); if it does not, the release is an ordinary click.
    // Acting on the press instead would make every drag start by dropping the
    // stack into the first slot.
    //
    // A press with an *empty* cursor is not deferred — it is a pickup and there
    // is nothing to spread — which keeps the path this client was first proven
    // on exactly as it was.
    bool             press_pending_{false};
    bool             press_right_{false};
    i16              press_slot_{-1};
    bool             dragging_{false};
    std::vector<i16> drag_slots_;

    /// Seconds since the interface was built, and when the last click landed.
    ///
    /// Only the double-click uses it. Wall time in a *renderer* is fine —
    /// nothing here is simulated and nothing here has to be reproducible; the
    /// determinism rule is about the tick, and this is a gesture.
    f32 clock_{0.0F};
    f32 last_click_time_{-1.0F};
    i16 last_click_slot_{-1};

    f32 previous_health_{20.0F};
};

}  // namespace ov::demo
