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
// The one exception is stated rather than hidden: in creative, vanilla's own
// screen moves the stacks itself and tells the server with Set Creative Mode
// Slot, which the server applies without answering. So the creative screen is
// client-authoritative here too, through client::CreativeInventory, whose every
// rule was measured on the real client. See docs/provenance/inventaire-creatif.md.
#pragma once

#include "chat.hpp"  // ── chat ──
#include "ov/base/types.hpp"
#include "ov/client/container_screen.hpp"
#include "ov/client/creative_gestures.hpp"
#include "ov/client/creative_screen.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/hud.hpp"
#include "ov/client/item_view.hpp"
#include "ov/client/saved_hotbars.hpp"
#include "ov/client/scoreboard_view.hpp"  // ── scoreboard ──
#include "ov/client/window.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/creative_items.hpp"
#include "ov/render/creative_tabs.hpp"
#include "ov/render/item_model.hpp"
#include "ov/render/language.hpp"
#include "ov/rhi/device.hpp"

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::demo {

struct InterfaceOptions {
    /// Zero means vanilla's automatic rule.
    u32 gui_scale{0};
    /// Draw the HUD at all. A measuring switch: the frame cost of the interface
    /// is the difference between this on and off on the same scene.
    bool hud{true};
    std::string language{"en_us"};
    /// The creative catalogue, produced by scripts/measure_creative_tabs.py
    /// (and by scripts/setup_vanilla.sh when it is missing). Absent is not
    /// fatal, but it is *said on screen*: the creative inventory is refused,
    /// and E shows a line naming the file and the script.
    std::string creative_tabs{"data/vanilla/1.20.1/creative_tabs.json"};
    /// Tooltips, tints and durability, asked of the real client by
    /// scripts/measure_creative_screen.py. Absent: names only.
    std::string creative_items{"data/vanilla/1.20.1/creative_items.json"};
    /// The data generator's datapack, for the item tags a '#' search reads.
    std::string item_tags{"data/vanilla/1.20.1/generated/data"};
    /// The saved hotbars, in vanilla's own file format.
    std::string hotbar_file{"run/hotbar.nbt"};
    /// Vanilla's "Operator Items Tab" setting, off by default as in vanilla.
    bool operator_tab{false};
    /// The empty-slot silhouettes of the survival page, as atlas sprites:
    /// helmet, chestplate, leggings, boots, shield.
    std::array<std::optional<render::SpriteUv>, 5> slot_icons{};
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

    // ── loading ──
    /// Cover the whole window with vanilla's loading screen — the options
    /// background tiled and darkened, one centred line — until cleared with an
    /// empty line. Nothing else is drawn while it is up, the HUD included.
    void set_loading(std::string line) { loading_line_ = std::move(line); }
    [[nodiscard]] bool loading() const noexcept { return !loading_line_.empty(); }
    /// A key from the game's own language file, so the loading lines read as
    /// the game's do in whatever language the player chose.
    [[nodiscard]] std::string translate(std::string_view key) const {
        return std::string(language_.translate(key));
    }
    std::string        loading_line_;
    client::GuiTexture loading_background_{client::GuiTexture::Invalid};
    // ── end loading ──

    /// Put the player's own inventory up, or take it down. Window 0 is never
    /// opened by the server: the client decides to show it.
    void toggle_inventory(client::Window& window, netclient::Client& client);

    /// Remember a Set Creative Slot this client sent. See the header comment.
    void note_creative(i16 slot, i32 item_id, i8 count);

    // ── The creative inventory ──────────────────────────────────────────────

    [[nodiscard]] bool creative_available() const noexcept {
        return creative_screen_.has_value();
    }

    [[nodiscard]] bool creative_open() const noexcept { return creative_visible_; }

    /// Put the creative inventory up, or take it down. Creative only.
    void toggle_creative(client::Window& window, netclient::Client& client);

    /// Select a tab by registry id, for the scripted captures.
    [[nodiscard]] bool select_creative_tab(std::string_view id);

    /// Type into the search field, for the scripted captures.
    void creative_search(std::string_view text);

    /// Take a full stack of a visible cell onto the cursor — the middle click.
    /// False when the cell is past the end of the page.
    [[nodiscard]] bool creative_take(i32 cell);

    /// Left-click a window-0 slot with what the cursor holds, and tell the
    /// server.
    void creative_put(netclient::Client& client, i16 slot);

    /// Hold the pointer at a point relative to the creative panel's corner,
    /// in GUI pixels, whatever the mouse does. For the scripted captures of a
    /// hover and its tooltip; nothing else uses it.
    void set_creative_pointer(std::optional<client::GuiPoint> panel_point) {
        pointer_override_ = panel_point;
    }

    /// A one-line description of the creative page, for the scripted checks.
    [[nodiscard]] std::string describe_creative() const;

    [[nodiscard]] i32 selected() const noexcept { return hud_.selected; }

    [[nodiscard]] const client::GuiStats& stats() const noexcept;

    [[nodiscard]] const client::HudState& hud() const noexcept { return hud_; }

    [[nodiscard]] std::string describe_window() const;

    [[nodiscard]] std::string describe_inventory() const;

    void click_slot(netclient::Client& client, i16 slot, i32 button, bool shift, i32 hotbar_key);

    void drag_over(netclient::Client& client, std::span<const i16> slots, bool right);

    [[nodiscard]] u8 window_id() const noexcept;

    [[nodiscard]] const std::vector<net::ItemStack>& window_slots() const noexcept {
        return window_slots_;
    }

    /// ── breaking ── The player's own window 0, 46 slots: armour at 5..8, the
    /// hotbar at 36..44. What is held and what the helmet carries decide how
    /// fast this client counts its own cracks.
    [[nodiscard]] const std::vector<net::ItemStack>& inventory() const noexcept {
        return inventory_;
    }

    void close(client::Window& window, netclient::Client& client);

    // ── screens ──
    /// The Video Settings screen's GUI Scale, applied at once. Zero is auto.
    void set_gui_scale(u32 scale) noexcept { options_.gui_scale = scale; }
    /// ── allow-commands ── Controls' "Operator Items Tab" changed.
    void set_operator_items_tab(bool on);
    // ── end screens ──

    // ── chat ──
    /// True while the chat box is open: the keys are letters, not moves.
    [[nodiscard]] bool chat_open() const noexcept { return chat_.open(); }
    [[nodiscard]] ChatUi& chat() noexcept { return chat_; }
    // ── end chat ──

private:
    Interface() = default;

    [[nodiscard]] std::string_view item_name(i32 item_id) const noexcept;

    bool update_creative(const client::InputState& input, netclient::Client& client,
                         client::Window& window);

    /// A stack as the item renderer draws it: name, count, tints, durability.
    [[nodiscard]] client::ItemStackView view_of(const net::ItemStack& stack) const;

    void build_views(const std::vector<net::ItemStack>& slots);

    void refresh_hotbar();

    // ── The creative model, and the mirror it is synced with ────────────────

    [[nodiscard]] client::SlotStack slot_of(const net::ItemStack& stack) const;
    [[nodiscard]] net::ItemStack    stack_of(const client::SlotStack& stack) const;
    [[nodiscard]] client::SlotStack cell_stack(const client::CreativeCell& cell) const;
    /// Copy the mirror into the model before a gesture, and back after it,
    /// sending what the gesture changed.
    void load_model();
    void store_model(netclient::Client& client);

    /// C/X + a number key, outside every screen.
    void save_hotbar(usize row);
    void load_hotbar(netclient::Client& client, usize row);

    /// The GUI point the creative screen reads as the pointer.
    [[nodiscard]] client::GuiPoint creative_pointer() const;

    std::unique_ptr<client::Gui> gui_;
    client::HudTextures          textures_;
    std::optional<client::ItemRenderer> items_;
    render::Language                    language_;
    client::ScoreboardView              scoreboard_view_;  // ── scoreboard ── the sidebar
    const registry::Registries*         registries_{nullptr};
    std::optional<registry::RegistryId> item_registry_;

    InterfaceOptions options_;

    client::HudState hud_;

    std::vector<net::ItemStack> inventory_;
    std::vector<net::ItemStack> window_slots_;
    net::ItemStack              carried_;
    i32                         state_id_{0};

    std::optional<client::ContainerScreen> screen_;

    std::optional<render::CreativeTabs>   creative_tabs_;
    std::optional<render::CreativeItems>  creative_items_;
    std::unordered_map<std::string, std::unordered_set<std::string>> item_tags_;
    std::optional<client::CreativeScreen> creative_screen_;
    client::CreativeInventory             creative_model_;
    client::CreativeEffects               effects_;
    client::SavedHotbars                  saved_hotbars_;
    client::CreativeTextures              creative_textures_;
    client::GuiTexture                    creative_sheet_{client::GuiTexture::Invalid};
    bool                                  creative_visible_{false};
    bool                                  creative_scrolling_{false};
    bool                                  hint_labels_set_{false};
    f32                                   creative_opened_at_{0.0F};
    std::optional<client::GuiPoint>       pointer_override_;
    std::vector<std::string>              tooltip_lines_;
    /// Shown above the hotbar when E is refused in creative, so the refusal
    /// is on the screen and not only in the log — the log is where the
    /// original "there is no creative inventory" hid.
    std::string refusal_;

    ChatUi chat_;  // ── chat ──

    bool own_inventory_{false};
    client::GuiTexture background_{client::GuiTexture::Invalid};
    std::vector<std::pair<std::string, client::GuiTexture>> backgrounds_;

    std::vector<client::ItemStackView> views_;

    f32 mouse_x_{0.0F};
    f32 mouse_y_{0.0F};

    bool             press_pending_{false};
    bool             press_right_{false};
    i16              press_slot_{-1};
    bool             dragging_{false};
    std::vector<i16> drag_slots_;

    f32 clock_{0.0F};
    f32 last_click_time_{-1.0F};
    i16 last_click_slot_{-1};

    f32 previous_health_{20.0F};
    /// ── allow-commands ── the permission level Entity Event 24..28 gave this
    /// player; the operator tab needs 2, and creative. Unset before any
    /// arrived (no server): the tab is then what the option says.
    std::optional<i32> op_level_;
    void refresh_operator_tab();
};

}  // namespace ov::demo
