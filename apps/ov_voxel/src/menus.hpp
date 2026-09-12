// The game's menus: title, world list, Create World, direct connection,
// options, pause, death — and the F3 screen.
//
// This is the application-side half: it owns the state machine, the
// options.txt the menus edit, the list of saves, and a GUI of its own. The
// geometry of every screen is ov_client's (menu_layouts.hpp), measured on the
// running 1.20.1 client; the widgets are ov_client's (menu_widgets.hpp),
// drawn from the pack's own sheets. See docs/provenance/ecrans.md.
//
// The menus never start a server or open a socket. They answer a frame's
// input with a MenuAction — play this world, connect there, respawn, save and
// quit — and main() does it: the only place that owns the client, the
// session and the integrated server.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/debug_overlay.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/menu_layouts.hpp"
#include "ov/client/menu_widgets.hpp"
#include "ov/client/options_file.hpp"
#include "ov/client/text_field.hpp"
#include "ov/client/window.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/language.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ov::demo {

enum class MenuScreen : u8 {
    None,
    Title,
    SelectWorld,
    CreateWorld,
    DirectConnect,
    Options,
    Video,
    Sounds,
    Controls,
    Mouse,
    KeyBinds,
    Language,
    Pause,
    Death,
    /// A line on the dirt: "Connecting to the server…", "Saving world".
    Message,
};

[[nodiscard]] std::string_view to_string(MenuScreen screen) noexcept;

/// A world in the saves folder, as its level.dat describes it.
struct SavedWorld {
    std::filesystem::path path;
    std::string           folder;
    std::string           name;
    /// Milliseconds since the epoch; 0 when level.dat does not say.
    i64  last_played{0};
    i32  game_type{0};
    bool generated{false};
    i64  seed{0};
    /// level.dat's allowCommands: the list's line says "Cheats".
    bool allow_commands{false};  // ── allow-commands ──
    /// Data.Version.Name ("1.20.1"): the line ends "Version: 1.20.1".
    std::string version_name;
};

/// Every folder under `saves` with a level.dat that reads, newest first.
[[nodiscard]] std::vector<SavedWorld> list_worlds(const std::filesystem::path& saves);

/// A folder name for a new world, vanilla's way: the name with the
/// characters a file system refuses replaced by `_`, and ` (1)`, ` (2)`…
/// until it is free.
[[nodiscard]] std::string world_folder_for(const std::filesystem::path& saves,
                                           std::string_view name);

struct MenuAction {
    enum class Kind : u8 {
        None,
        /// Host `world` and play it. `seed` is set for a new generated world.
        Play,
        /// Join `address` (host:port).
        Connect,
        /// Back to the game from the pause menu.
        Resume,
        /// Leave the world: save and quit, or disconnect.
        Leave,
        /// The death screen's Respawn.
        Respawn,
        /// Close the window.
        Quit,
    };
    Kind                  kind{Kind::None};
    std::filesystem::path world;
    std::string           world_name;
    std::optional<i64>    seed;
    bool                  create{false};
    bool                  survival{true};
    /// A new world's "Allow Cheats", for its level.dat. ── allow-commands ──
    bool                  allow_commands{false};
    std::string           address;
};

struct MenusConfig {
    std::filesystem::path saves{"run/saves"};
    std::filesystem::path options_file{"run/options.txt"};
    /// The pack's root (holding assets/), for the list of languages.
    std::filesystem::path assets_root{"run/assets"};
    /// Empty: options.txt's language. Set by --lang.
    std::string           language;
    /// Replaces options.txt's scale when non-zero (the --gui-scale flag).
    u32 gui_scale_override{0};
    /// The random seed a blank seed field gives. From the caller, so the menus
    /// hold no clock and no global generator.
    i64 random_seed{0};
};

class Menus {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Menus>, std::string> create(
        rhi::Device& device, rhi::Format colour_format, const render::AssetSource& assets,
        const MenusConfig& config);

    Menus(const Menus&)            = delete;
    Menus& operator=(const Menus&) = delete;
    ~Menus();

    void                     open(MenuScreen screen);
    [[nodiscard]] MenuScreen screen() const noexcept { return screen_; }
    [[nodiscard]] bool       any_open() const noexcept { return screen_ != MenuScreen::None; }

    /// Whether a world is behind the menus, and whether it is hosted here:
    /// decides the backgrounds, the pause menu's last button, and where
    /// Escape from the options goes.
    void set_in_game(bool in_game, bool integrated);

    /// Put the death screen up. `cause_json` is Combat Death's component,
    /// rendered in the menus' language.
    void show_death(std::string_view cause_json, i32 score, bool hardcore);

    /// A one-line message screen, or back to nothing with an empty line.
    void show_message(std::string line);

    /// This frame's input. Returns what main() must do; keys and clicks the
    /// menus took are not the game's.
    MenuAction update(const client::InputState& input, client::Window& window, f64 delta_seconds);

    /// Record the menus into the current pass. `world_behind`: the terrain was
    /// drawn this frame (the in-game dim instead of the dirt or panorama).
    void draw(rhi::CommandList& cmd, u32 framebuffer_width, u32 framebuffer_height);

    // ── Options ─────────────────────────────────────────────────────────────
    [[nodiscard]] const client::GameOptions& options() const noexcept { return options_; }
    /// True once after the options changed; main() applies them.
    [[nodiscard]] bool take_options_changed() noexcept;
    /// ── sound ── True once after a button was pressed: main() plays the click.
    [[nodiscard]] bool take_click() noexcept {
        const bool was = clicked_;
        clicked_       = false;
        return was;
    }
    [[nodiscard]] u32  gui_scale(u32 framebuffer_width, u32 framebuffer_height) const noexcept;

    // ── F3 ──────────────────────────────────────────────────────────────────
    void toggle_debug() noexcept { debug_ = !debug_; }
    [[nodiscard]] bool debug() const noexcept { return debug_; }
    void               set_debug_info(client::DebugInfo info) { debug_info_ = std::move(info); }

    // ── Scripted, through the path a click takes ────────────────────────────
    /// Press the widget with this id, as a click on its centre would.
    [[nodiscard]] MenuAction press(std::string_view id);
    /// Type into the focused text box.
    void type(std::string_view text);
    /// Select a row of the world list, as a click on it would.
    void select_world(i32 row) noexcept {
        selected_world_ = row;
        dirty_          = true;
    }
    /// Every widget of the current screen: id, rectangle, text. For the
    /// layout check against the vanilla client.
    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string translate(std::string_view key) const;

private:
    Menus() = default;

    void       rebuild();
    MenuAction activate(const client::Widget& widget);
    MenuAction back();
    void       draw_background(f32 width, f32 height);
    void       draw_title_screen(f32 width, f32 height);
    void       draw_panorama(f32 width, f32 height);
    void       save_options();
    void       set_slider(std::string_view id, f64 value);
    [[nodiscard]] std::string option_label(std::string_view id) const;
    [[nodiscard]] f64         slider_value(std::string_view id) const;
    [[nodiscard]] client::TextField* focused_field() noexcept;

    std::unique_ptr<client::Gui> gui_;
    client::MenuTextures         textures_;
    render::Language             language_;
    /// The pack, for reloading the language when the player picks another.
    /// Outlives the menus: main() owns both.
    const render::AssetSource* assets_{nullptr};
    MenusConfig                  config_;

    MenuScreen                  screen_{MenuScreen::None};
    /// Where Escape and Done go from the options family: Title or Pause.
    MenuScreen                  options_parent_{MenuScreen::Title};
    std::vector<client::Widget> widgets_;
    std::string                 title_;
    bool                        in_game_{false};
    bool                        integrated_{false};
    f32                         width_{854.0F};
    f32                         height_{480.0F};
    f32                         mouse_x_{0.0F};
    f32                         mouse_y_{0.0F};
    f64                         clock_{0.0};
    std::string                 dragging_;

    client::GameOptions options_;
    client::OptionsFile options_file_;
    bool                options_changed_{false};
    bool                clicked_{false};  // ── sound ──

    // Death
    std::string death_cause_;
    i32         death_score_{0};
    bool        hardcore_{false};
    f64         death_opened_at_{0.0};

    std::string message_;

    // Select World
    std::vector<SavedWorld> worlds_;
    i32                     selected_world_{-1};
    f64                     last_world_click_{-1.0};
    f32                     world_scroll_{0.0F};

    // Create World
    i32               create_tab_{0};
    client::TextField world_name_{32};
    client::TextField seed_field_{32};
    // ── allow-commands ── the game mode and Allow Cheats, decided together as
    // the vanilla screen decides them (menu_layouts.hpp).
    client::CreateGameMode create_mode_{client::CreateGameMode::Survival};
    client::AllowCheats    create_cheats_;
    bool              create_flat_{false};
    std::string       focus_;

    // Direct Connection
    client::TextField address_{128};

    // Key Binds
    std::string waiting_for_key_;
    f32         keys_scroll_{0.0F};

    // Language
    std::vector<std::pair<std::string, std::string>> languages_;
    f32                                              language_scroll_{0.0F};

    bool              debug_{false};
    client::DebugInfo debug_info_;
    std::string       clipboard_;
    bool              dirty_{true};
    u32               last_scale_{0};
};

}  // namespace ov::demo
