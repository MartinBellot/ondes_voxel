#define OV_LOG_CATEGORY "menus"

#include "menus.hpp"

#include "ov/base/log.hpp"
#include "ov/base/resource_location.hpp"
#include "ov/io/compression.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/render/font.hpp"
#include "ov/render/text_component.hpp"
#include "ov/render/texture_image.hpp"
#include "ov/world/level_dat.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iterator>
#include <numbers>

namespace ov::demo {
namespace {

constexpr f64 kDoubleClickSeconds = 0.25;
/// Vanilla keeps the death screen's buttons inactive for its first 20 ticks.
constexpr f64 kDeathButtonDelay = 1.0;

[[nodiscard]] std::optional<std::vector<u8>> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string percent(f64 value) {
    return std::to_string(static_cast<i32>(std::lround(value * 100.0)));
}

}  // namespace

std::string_view to_string(MenuScreen screen) noexcept {
    switch (screen) {
        case MenuScreen::None: return "none";
        case MenuScreen::Title: return "title";
        case MenuScreen::SelectWorld: return "select_world";
        case MenuScreen::CreateWorld: return "create_world";
        case MenuScreen::DirectConnect: return "direct_connect";
        case MenuScreen::Options: return "options";
        case MenuScreen::Video: return "video";
        case MenuScreen::Sounds: return "sounds";
        case MenuScreen::Controls: return "controls";
        case MenuScreen::Mouse: return "mouse";
        case MenuScreen::KeyBinds: return "key_binds";
        case MenuScreen::Language: return "language";
        case MenuScreen::Pause: return "pause";
        case MenuScreen::Death: return "death";
        case MenuScreen::Message: return "message";
    }
    return "unknown";
}

// ── Saves ───────────────────────────────────────────────────────────────────

std::vector<SavedWorld> list_worlds(const std::filesystem::path& saves) {
    std::vector<SavedWorld> worlds;
    std::error_code         error;
    if (!std::filesystem::is_directory(saves, error)) {
        return worlds;
    }
    for (const auto& entry : std::filesystem::directory_iterator(saves, error)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto level = entry.path() / "level.dat";
        const auto bytes = read_bytes(level);
        if (!bytes) {
            continue;
        }
        const auto raw = io::gzip_decompress(*bytes);
        if (!raw) {
            OV_LOG_WARN("{}: not gzip; the world is not listed", level.string());
            continue;
        }
        const auto document = nbt::read(*raw);
        const nbt::Tag* data = document ? document->root.find("Data") : nullptr;
        if (data == nullptr) {
            OV_LOG_WARN("{}: no Data compound; the world is not listed", level.string());
            continue;
        }
        world::LevelSettings settings;
        settings.name = entry.path().filename().string();
        world::read_level_settings(*data, settings);
        SavedWorld world;
        world.path      = entry.path();
        world.folder    = entry.path().filename().string();
        world.name      = settings.name;
        world.game_type = settings.game_type;
        world.generated = settings.generated;
        world.seed      = settings.seed;
        world.allow_commands = settings.allow_commands;  // ── allow-commands ──
        if (const nbt::Tag* version = data->find("Version")) {
            if (const nbt::Tag* name = version->find("Name")) {
                world.version_name = std::string{name->as_string()};
            }
        }
        if (const nbt::Tag* played = data->find("LastPlayed")) {
            world.last_played = played->as_i64();
        }
        if (world.last_played == 0) {
            // Our server does not write LastPlayed; the file's age orders the
            // list instead. A display order, not a game fact.
            const auto written = std::filesystem::last_write_time(level, error);
            if (!error) {
                const auto since = written - std::filesystem::file_time_type::clock::now();
                world.last_played =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch() + since)
                        .count();
            }
        }
        worlds.push_back(std::move(world));
    }
    std::ranges::sort(worlds, [](const SavedWorld& a, const SavedWorld& b) {
        return a.last_played > b.last_played;
    });
    return worlds;
}

std::string world_folder_for(const std::filesystem::path& saves, std::string_view name) {
    std::string base;
    for (const char c : name) {
        constexpr std::string_view kIllegal = "/\n\r\t\f`?*\\<>|\":";
        base.push_back(kIllegal.find(c) != std::string_view::npos || c == '\0' ? '_' : c);
    }
    while (!base.empty() && (base.back() == '.' || base.back() == ' ')) {
        base.pop_back();
    }
    if (base.empty()) {
        base = "World";
    }
    std::string     candidate = base;
    std::error_code error;
    for (i32 n = 1; std::filesystem::exists(saves / candidate, error); ++n) {
        candidate = fmt::format("{} ({})", base, n);
    }
    return candidate;
}

// ── Construction ────────────────────────────────────────────────────────────

Menus::~Menus() = default;

std::expected<std::unique_ptr<Menus>, std::string> Menus::create(
    rhi::Device& device, rhi::Format colour_format, const render::AssetSource& assets,
    const MenusConfig& config) {
    std::unique_ptr<Menus> self(new Menus);
    self->config_ = config;
    self->assets_ = &assets;

    auto gui = client::Gui::create(device, colour_format);
    if (!gui) {
        return std::unexpected(std::string(rhi::to_string(gui.error())));
    }
    self->gui_ = std::move(*gui);
    auto font  = render::Font::load_default(assets);
    if (!font) {
        return std::unexpected(std::string(render::to_string(font.error())));
    }
    if (auto set = self->gui_->set_font(std::move(*font)); !set) {
        return std::unexpected(std::string(rhi::to_string(set.error())));
    }

    const auto sheet = [&](std::string_view location) -> client::GuiTexture {
        const auto parsed = ResourceLocation::parse(location);
        if (!parsed) {
            return client::GuiTexture::Invalid;
        }
        auto image = render::load_texture(assets, *parsed);
        if (!image) {
            OV_LOG_WARN("{}: {} — drawn without it", location, render::to_string(image.error()));
            return client::GuiTexture::Invalid;
        }
        auto handle = self->gui_->add_texture(*image, location);
        return handle ? *handle : client::GuiTexture::Invalid;
    };
    self->textures_.widgets          = sheet("minecraft:gui/widgets");
    self->textures_.slider           = sheet("minecraft:gui/slider");
    self->textures_.dirt             = sheet("minecraft:gui/options_background");
    self->textures_.logo             = sheet("minecraft:gui/title/minecraft");
    self->textures_.edition          = sheet("minecraft:gui/title/edition");
    self->textures_.tab              = sheet("minecraft:gui/tab_button");
    self->textures_.accessibility    = sheet("minecraft:gui/accessibility");
    self->textures_.header_separator = sheet("minecraft:gui/header_separator");
    self->textures_.footer_separator = sheet("minecraft:gui/footer_separator");
    for (usize i = 0; i < self->textures_.panorama.size(); ++i) {
        self->textures_.panorama[i] =
            sheet(fmt::format("minecraft:gui/title/background/panorama_{}", i));
    }

    // options.txt: every line kept, the known ones typed.
    if (auto file = client::OptionsFile::load(config.options_file); file) {
        self->options_file_ = std::move(*file);
    } else {
        OV_LOG_WARN("options: {}; starting from vanilla's defaults", file.error());
    }
    std::vector<std::string> problems;
    self->options_ = client::GameOptions::from(self->options_file_, &problems);
    for (const std::string& problem : problems) {
        OV_LOG_WARN("options.txt: {}", problem);
    }
    if (!config.language.empty()) {
        self->options_.language = config.language;
    }
    auto language = render::Language::load(assets, self->options_.language);
    if (!language && self->options_.language != "en_us") {
        OV_LOG_WARN("language {} missing; en_us instead", self->options_.language);
        language = render::Language::load(assets, "en_us");
    }
    if (language) {
        self->language_ = std::move(*language);
    }
    self->world_name_.set_value(self->translate("selectWorld.newWorld"));
    return self;
}

std::string Menus::translate(std::string_view key) const {
    return std::string(language_.translate(key));
}

// ── State ───────────────────────────────────────────────────────────────────

void Menus::open(MenuScreen screen) {
    if (screen == MenuScreen::Options) {
        options_parent_ = in_game_ ? MenuScreen::Pause : MenuScreen::Title;
    }
    if (screen == MenuScreen::SelectWorld) {
        worlds_         = list_worlds(config_.saves);
        selected_world_ = -1;
        world_scroll_   = 0.0F;
        focus_          = "search";
    }
    if (screen == MenuScreen::CreateWorld) {
        create_tab_ = 0;
        focus_      = "world_name";
        world_name_.set_value(translate("selectWorld.newWorld"));
        seed_field_.set_value("");
        // ── allow-commands ── a fresh screen: Survival, cheats untouched.
        create_mode_   = client::CreateGameMode::Survival;
        create_cheats_ = client::AllowCheats{};
    }
    if (screen == MenuScreen::DirectConnect) {
        focus_ = "address";
    }
    if (screen == MenuScreen::Language) {
        languages_.clear();
        std::error_code error;
        const auto      folder = config_.assets_root / "assets" / "minecraft" / "lang";
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.path().extension() != ".json") {
                continue;
            }
            // The display name is in the file itself (`language.name`,
            // `language.region`); read with a substring search rather than a
            // whole JSON parse of 143 files of 400 KB.
            std::string       name = entry.path().stem().string();
            const auto        text = read_bytes(entry.path());
            const auto        pick = [&](std::string_view key) -> std::string {
                if (!text) {
                    return {};
                }
                const std::string_view body(reinterpret_cast<const char*>(text->data()), text->size());
                const std::string      needle = "\"" + std::string(key) + "\": \"";
                const auto             at     = body.find(needle);
                if (at == std::string_view::npos) {
                    return {};
                }
                const auto start = at + needle.size();
                const auto end   = body.find('"', start);
                return std::string(body.substr(start, end - start));
            };
            const std::string display = pick("language.name");
            const std::string region  = pick("language.region");
            languages_.emplace_back(name, display.empty() ? name : display + " (" + region + ")");
        }
        std::ranges::sort(languages_);
        language_scroll_ = 0.0F;
    }
    screen_   = screen;
    dragging_.clear();
    waiting_for_key_.clear();
    dirty_ = true;
}

void Menus::set_in_game(bool in_game, bool integrated) {
    in_game_    = in_game;
    integrated_ = integrated;
    dirty_      = true;
}

void Menus::show_death(std::string_view cause_json, i32 score, bool hardcore) {
    death_cause_     = cause_json.empty() ? std::string{}
                                          : render::flatten_component(cause_json, language_);
    death_score_     = score;
    hardcore_        = hardcore;
    death_opened_at_ = clock_;
    open(MenuScreen::Death);
}

void Menus::show_message(std::string line) {
    message_ = std::move(line);
    open(message_.empty() ? MenuScreen::None : MenuScreen::Message);
}

bool Menus::take_options_changed() noexcept {
    const bool changed = options_changed_;
    options_changed_   = false;
    return changed;
}

u32 Menus::gui_scale(u32 framebuffer_width, u32 framebuffer_height) const noexcept {
    if (config_.gui_scale_override != 0) {
        return client::auto_gui_scale(framebuffer_width, framebuffer_height,
                                      config_.gui_scale_override);
    }
    return client::effective_gui_scale(options_, framebuffer_width, framebuffer_height);
}

void Menus::save_options() {
    options_.store(options_file_);
    if (auto saved = options_file_.save(config_.options_file); !saved) {
        OV_LOG_WARN("options: {}", saved.error());
    }
}

client::TextField* Menus::focused_field() noexcept {
    if (screen_ == MenuScreen::CreateWorld) {
        if (focus_ == "world_name" && create_tab_ == 0) {
            return &world_name_;
        }
        if (focus_ == "seed" && create_tab_ == 1) {
            return &seed_field_;
        }
    }
    if (screen_ == MenuScreen::DirectConnect && focus_ == "address") {
        return &address_;
    }
    return nullptr;
}

// ── Labels ──────────────────────────────────────────────────────────────────

f64 Menus::slider_value(std::string_view id) const {
    if (id == "fov") {
        return (options_.fov - 30) / 80.0;
    }
    if (id == "render_distance") {
        return (options_.render_distance - 2) / 30.0;
    }
    if (id == "simulation_distance") {
        return (options_.simulation_distance - 5) / 27.0;
    }
    if (id == "max_fps") {
        return (options_.max_fps - 10) / 250.0;
    }
    if (id == "gamma") {
        return options_.gamma;
    }
    if (id == "sensitivity") {
        return options_.mouse_sensitivity;
    }
    if (id.starts_with("volume_")) {
        const auto name = id.substr(7);
        for (usize i = 0; i < client::kSoundCategoryNames.size(); ++i) {
            if (client::kSoundCategoryNames[i] == name) {
                return options_.volumes[i];
            }
        }
    }
    return 0.0;
}

void Menus::set_slider(std::string_view id, f64 value) {
    value = std::clamp(value, 0.0, 1.0);
    if (id == "fov") {
        options_.fov = 30 + static_cast<i32>(std::lround(value * 80.0));
    } else if (id == "render_distance") {
        options_.render_distance = 2 + static_cast<i32>(std::lround(value * 30.0));
    } else if (id == "simulation_distance") {
        options_.simulation_distance = 5 + static_cast<i32>(std::lround(value * 27.0));
    } else if (id == "max_fps") {
        options_.max_fps = 10 + 10 * static_cast<i32>(std::lround(value * 25.0));
    } else if (id == "gamma") {
        options_.gamma = value;
    } else if (id == "sensitivity") {
        options_.mouse_sensitivity = value;
    } else if (id.starts_with("volume_")) {
        const auto name = id.substr(7);
        for (usize i = 0; i < client::kSoundCategoryNames.size(); ++i) {
            if (client::kSoundCategoryNames[i] == name) {
                options_.volumes[i] = value;
            }
        }
    } else {
        return;
    }
    options_changed_ = true;
    dirty_           = true;
}

std::string Menus::option_label(std::string_view id) const {
    const auto generic = [&](std::string_view key, const std::string& value) {
        const std::array<std::string, 2> arguments{translate(key), value};
        return render::format_translation(language_.translate("options.generic_value"), arguments);
    };
    const auto percent_of = [&](std::string_view key, f64 value) {
        const std::array<std::string, 2> arguments{translate(key), percent(value)};
        return render::format_translation(language_.translate("options.percent_value"), arguments);
    };
    const auto on_off = [&](bool on) { return translate(on ? "options.on" : "options.off"); };
    if (id == "operator_tab") {  // ── allow-commands ──
        return generic("options.operatorItemsTab", on_off(options_.operator_items_tab));
    }
    if (id == "fov") {
        if (options_.fov == 70) {
            return generic("options.fov", translate("options.fov.min"));
        }
        if (options_.fov == 110) {
            return generic("options.fov", translate("options.fov.max"));
        }
        return generic("options.fov", std::to_string(options_.fov));
    }
    if (id == "render_distance" || id == "simulation_distance") {
        const i32                        chunks = id == "render_distance"
                                                      ? options_.render_distance
                                                      : options_.simulation_distance;
        const std::array<std::string, 1> count{std::to_string(chunks)};
        return generic(id == "render_distance" ? "options.renderDistance"
                                               : "options.simulationDistance",
                       render::format_translation(language_.translate("options.chunks"), count));
    }
    if (id == "max_fps") {
        if (options_.max_fps >= 260) {
            return generic("options.framerateLimit", translate("options.framerateLimit.max"));
        }
        const std::array<std::string, 1> fps{std::to_string(options_.max_fps)};
        return generic("options.framerateLimit",
                       render::format_translation(language_.translate("options.framerate"), fps));
    }
    if (id == "vsync") {
        return generic("options.vsync", on_off(options_.vsync));
    }
    if (id == "gui_scale") {
        return generic("options.guiScale", options_.gui_scale == 0
                                               ? translate("options.guiScale.auto")
                                               : std::to_string(options_.gui_scale));
    }
    if (id == "gamma") {
        const i32 value = static_cast<i32>(options_.gamma * 100.0);
        if (value == 0) {
            return generic("options.gamma", translate("options.gamma.min"));
        }
        if (value == 50) {
            return generic("options.gamma", translate("options.gamma.default"));
        }
        if (value == 100) {
            return generic("options.gamma", translate("options.gamma.max"));
        }
        return generic("options.gamma", std::to_string(value));
    }
    if (id == "sensitivity") {
        if (options_.mouse_sensitivity == 0.0) {
            return generic("options.sensitivity", translate("options.sensitivity.min"));
        }
        if (options_.mouse_sensitivity == 1.0) {
            return generic("options.sensitivity", translate("options.sensitivity.max"));
        }
        return percent_of("options.sensitivity", options_.mouse_sensitivity * 2.0);
    }
    if (id.starts_with("volume_")) {
        const std::string key   = "soundCategory." + std::string(id.substr(7));
        const f64         value = slider_value(id);
        if (value == 0.0) {
            return generic(key, translate("options.off"));
        }
        return percent_of(key, value);
    }
    return {};
}

// ── Layout ──────────────────────────────────────────────────────────────────

void Menus::rebuild() {
    dirty_ = false;
    widgets_.clear();
    title_.clear();
    const f32 w = width_;
    const f32 h = height_;
    switch (screen_) {
        case MenuScreen::None:
        case MenuScreen::Message: return;
        case MenuScreen::Title: widgets_ = client::title_layout(w, h); break;
        case MenuScreen::Pause:
            widgets_ = client::pause_layout(w, h, integrated_);
            title_   = "menu.game";
            break;
        case MenuScreen::Death: widgets_ = client::death_layout(w, h); break;
        case MenuScreen::Options:
            widgets_ = client::options_layout(w, h, in_game_);
            title_   = "options.title";
            break;
        case MenuScreen::Video:
            widgets_ = client::video_layout(w, h);
            title_   = "options.videoTitle";
            break;
        case MenuScreen::Sounds:
            widgets_ = client::sounds_layout(w, h);
            title_   = "options.sounds.title";
            break;
        case MenuScreen::Controls:
            widgets_ = client::controls_layout(w, h);
            title_   = "controls.title";
            break;
        case MenuScreen::Mouse: {
            const f32 left = std::floor(w / 2.0F) - 155.0F;
            client::Widget sensitivity;
            sensitivity.id   = "sensitivity";
            sensitivity.kind = client::WidgetKind::Slider;
            sensitivity.rect = client::Rect{left, 36.0F, 150.0F, 20.0F};
            widgets_.push_back(sensitivity);
            client::Widget done;
            done.id   = "done";
            done.rect = client::Rect{std::floor(w / 2.0F) - 100.0F, h - 27.0F, 200.0F, 20.0F};
            done.text = "gui.done";
            widgets_.push_back(done);
            title_ = "options.mouse_settings.title";
            break;
        }
        case MenuScreen::KeyBinds: {
            std::vector<client::KeyRow> rows;
            // Vanilla groups by category in its own category order.
            static constexpr std::array<std::string_view, 6> kOrder{
                "key.categories.movement", "key.categories.misc",
                "key.categories.multiplayer", "key.categories.gameplay",
                "key.categories.inventory", "key.categories.creative"};
            for (const std::string_view category : kOrder) {
                for (const client::KeyBinding& key : options_.keys) {
                    if (key.category == category) {
                        rows.push_back(client::KeyRow{key.name, key.category});
                    }
                }
            }
            widgets_ = client::key_binds_layout(w, h, rows, keys_scroll_);
            title_   = "controls.keybinds.title";
            break;
        }
        case MenuScreen::SelectWorld:
            widgets_ = client::select_world_layout(w, h);
            title_   = "selectWorld.title";
            break;
        case MenuScreen::CreateWorld:
            // No title line: the tab bar is the header, as in vanilla's
            // tabbed Create World.
            widgets_ = client::create_world_layout(w, h, create_tab_);
            break;
        case MenuScreen::DirectConnect:
            widgets_ = client::direct_connect_layout(w, h);
            title_   = "selectServer.direct";
            break;
        case MenuScreen::Language:
            widgets_ = client::language_layout(w, h);
            title_   = "options.language.title";
            break;
    }

    for (client::Widget& widget : widgets_) {
        if (widget.kind != client::WidgetKind::EditBox && !widget.text.empty()) {
            widget.text = translate(widget.text);
        }
        if (!widget.hint.empty()) {
            widget.hint = translate(widget.hint);
        }
    }

    // What is not implemented is shown and refused: inactive, as vanilla
    // draws a button that cannot be used, and named in ecrans.md.
    static constexpr std::array<std::string_view, 30> kInactive{
        "realms", "accessibility", "advancements", "stats", "feedback", "bugs", "lan",
        "reporting", "difficulty", "difficulty_lock", "online", "skin", "chat", "packs",
        "telemetry", "credits", "graphics", "chunk_updates", "smooth_lighting", "view_bobbing",
        "attack_indicator", "clouds", "fullscreen", "particles", "mipmaps", "biome_blend",
        "entity_distance", "entity_shadows", "device", "subtitles"};
    static constexpr std::array<std::string_view, 10> kInactiveMore{
        "directional", "sneak", "sprint", "auto_jump", "edit", "delete",
        "recreate", "customize", "structures", "bonus_chest"};
    for (client::Widget& widget : widgets_) {
        const bool refused =
            std::ranges::find(kInactive, widget.id) != kInactive.end() ||
            std::ranges::find(kInactiveMore, widget.id) != kInactiveMore.end() ||
            widget.id == "game_rules" || widget.id == "experiments" || widget.id == "data_packs" ||
            (screen_ == MenuScreen::Options && widget.id == "accessibility") ||
            (screen_ == MenuScreen::Options && widget.id == "chat");
        if (refused) {
            widget.active = false;
        }
        if (screen_ == MenuScreen::Title && widget.id == "language") {
            widget.active = true;
        }
    }

    for (client::Widget& widget : widgets_) {
        if (widget.kind == client::WidgetKind::Slider) {
            if (!widget.active) {
                // Not implemented: shown with its caption, greyed, like any
                // refused control — never as an empty track.
                widget.kind = client::WidgetKind::Button;
                continue;
            }
            widget.value = slider_value(widget.id);
            widget.text  = option_label(widget.id);
        }
        if (widget.id == "vsync" || widget.id == "gui_scale" ||
            widget.id == "operator_tab") {  // ── allow-commands ──
            widget.text = option_label(widget.id);
        }
    }

    switch (screen_) {
        case MenuScreen::Death: {
            const bool ready = clock_ - death_opened_at_ >= kDeathButtonDelay;
            for (client::Widget& widget : widgets_) {
                widget.active = ready;
                if (widget.id == "respawn" && hardcore_) {
                    widget.text = translate("deathScreen.spectate");
                }
            }
            break;
        }
        case MenuScreen::KeyBinds:
            for (client::Widget& widget : widgets_) {
                if (widget.id.starts_with("category:") || widget.id.starts_with("label:")) {
                    widget.text = translate(widget.text);
                }
                const bool is_key   = widget.id.starts_with("key:");
                const bool is_reset = widget.id.starts_with("reset:");
                if (!is_key && !is_reset) {
                    continue;
                }
                const std::string_view name = std::string_view(widget.id).substr(is_key ? 4 : 6);
                const client::KeyBinding* key = options_.binding(name);
                if (key == nullptr) {
                    continue;
                }
                if (is_reset) {
                    widget.active = key->code != key->default_code;
                    continue;
                }
                const std::string key_text = client::key_name(key->code);
                std::string       shown    = translate(key_text);
                if (key->code >= 0 && key->code < client::kMouseCodeBase && key_text.size() == 14) {
                    // A single-character key: vanilla prints the layout's
                    // label, upper-cased — see Window::code_label.
                    shown = key_text.substr(13);
                    std::ranges::transform(shown, shown.begin(), [](char c) {
                        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
                    });
                }
                if (waiting_for_key_ == name) {
                    widget.text   = "> §e§n" + shown + "§r <";
                    widget.colour = 0xFFFFFFFFU;
                } else {
                    widget.text = shown;
                }
            }
            break;
        case MenuScreen::SelectWorld:
            for (client::Widget& widget : widgets_) {
                if (widget.id == "play") {
                    widget.active = selected_world_ >= 0;
                }
                if (widget.id == "search") {
                    widget.selected = focus_ == "search";
                }
            }
            break;
        case MenuScreen::CreateWorld:
            for (client::Widget& widget : widgets_) {
                if (widget.id == "world_name") {
                    widget.text     = world_name_.value();
                    widget.selected = focus_ == "world_name";
                } else if (widget.id == "seed") {
                    widget.text     = seed_field_.value();
                    widget.selected = focus_ == "seed";
                } else if (widget.id == "game_mode") {
                    const std::array<std::string, 2> arguments{
                        translate("selectWorld.gameMode"),
                        translate(create_mode_ == client::CreateGameMode::Creative
                                      ? "selectWorld.gameMode.creative"
                                  : create_mode_ == client::CreateGameMode::Hardcore
                                      ? "selectWorld.gameMode.hardcore"
                                      : "selectWorld.gameMode.survival")};
                    widget.text = render::format_translation(
                        language_.translate("options.generic_value"), arguments);
                } else if (widget.id == "cheats") {  // ── allow-commands ──
                    // options.generic_value(selectWorld.allowCommands, options.on|off),
                    // the vanilla button's own component.
                    const std::array<std::string, 2> arguments{
                        translate("selectWorld.allowCommands"),
                        translate(create_cheats_.value(create_mode_) ? "options.on"
                                                                     : "options.off")};
                    widget.text = render::format_translation(
                        language_.translate("options.generic_value"), arguments);
                    widget.active = client::AllowCheats::active(create_mode_);
                } else if (widget.id == "world_type") {
                    const std::array<std::string, 2> arguments{
                        translate("selectWorld.mapType"),
                        translate(create_flat_ ? "generator.minecraft.flat"
                                               : "generator.minecraft.normal")};
                    widget.text = render::format_translation(
                        language_.translate("options.generic_value"), arguments);
                } else if (widget.id == "create") {
                    widget.active = !world_name_.value().empty();
                }
            }
            break;
        case MenuScreen::DirectConnect:
            for (client::Widget& widget : widgets_) {
                if (widget.id == "address") {
                    widget.text     = address_.value();
                    widget.selected = true;
                }
                if (widget.id == "join") {
                    widget.active = !address_.value().empty();
                }
            }
            break;
        default: break;
    }
}

// ── Input ───────────────────────────────────────────────────────────────────

MenuAction Menus::back() {
    MenuAction action;
    switch (screen_) {
        case MenuScreen::SelectWorld:
        case MenuScreen::DirectConnect: open(MenuScreen::Title); break;
        case MenuScreen::CreateWorld:
            open(list_worlds(config_.saves).empty() ? MenuScreen::Title : MenuScreen::SelectWorld);
            break;
        case MenuScreen::Options:
            save_options();
            open(options_parent_);
            break;
        case MenuScreen::Video:
        case MenuScreen::Sounds:
        case MenuScreen::Controls:
            save_options();
            open(MenuScreen::Options);
            options_parent_ = in_game_ ? MenuScreen::Pause : MenuScreen::Title;
            break;
        case MenuScreen::Language: {
            save_options();
            // The menus speak the new language at once; the in-game interface
            // takes it at the next world it joins.
            if (assets_ != nullptr) {
                if (auto language = render::Language::load(*assets_, options_.language)) {
                    language_ = std::move(*language);
                } else {
                    OV_LOG_WARN("language {}: {}", options_.language,
                                render::to_string(language.error()));
                }
            }
            const MenuScreen parent = options_parent_;
            open(MenuScreen::Options);
            options_parent_ = parent;
            break;
        }
        case MenuScreen::Mouse:
        case MenuScreen::KeyBinds:
            save_options();
            open(MenuScreen::Controls);
            break;
        case MenuScreen::Pause:
            open(MenuScreen::None);
            action.kind = MenuAction::Kind::Resume;
            break;
        case MenuScreen::Title:
        case MenuScreen::Death:
        case MenuScreen::Message:
        case MenuScreen::None: break;
    }
    return action;
}

MenuAction Menus::activate(const client::Widget& widget) {
    MenuAction        action;
    const std::string id = widget.id;
    if (id == "done") {
        return back();
    }
    switch (screen_) {
        case MenuScreen::Title:
            if (id == "singleplayer") {
                open(list_worlds(config_.saves).empty() ? MenuScreen::CreateWorld
                                                        : MenuScreen::SelectWorld);
            } else if (id == "multiplayer") {
                open(MenuScreen::DirectConnect);
            } else if (id == "options") {
                open(MenuScreen::Options);
            } else if (id == "language") {
                open(MenuScreen::Language);
                options_parent_ = MenuScreen::Title;
            } else if (id == "quit") {
                action.kind = MenuAction::Kind::Quit;
            }
            break;
        case MenuScreen::Pause:
            if (id == "resume") {
                return back();
            }
            if (id == "options") {
                open(MenuScreen::Options);
            } else if (id == "leave") {
                action.kind = MenuAction::Kind::Leave;
            }
            break;
        case MenuScreen::Death:
            if (id == "respawn") {
                action.kind = MenuAction::Kind::Respawn;
            } else if (id == "title") {
                action.kind = MenuAction::Kind::Leave;
            }
            break;
        case MenuScreen::Options:
            if (id == "video") {
                const MenuScreen parent = options_parent_;
                open(MenuScreen::Video);
                options_parent_ = parent;
            } else if (id == "sounds") {
                const MenuScreen parent = options_parent_;
                open(MenuScreen::Sounds);
                options_parent_ = parent;
            } else if (id == "controls") {
                const MenuScreen parent = options_parent_;
                open(MenuScreen::Controls);
                options_parent_ = parent;
            } else if (id == "language") {
                const MenuScreen parent = options_parent_;
                open(MenuScreen::Language);
                options_parent_ = parent;
            }
            break;
        case MenuScreen::Video:
            if (id == "vsync") {
                options_.vsync   = !options_.vsync;
                options_changed_ = true;
            } else if (id == "gui_scale") {
                const u32 most = client::auto_gui_scale(
                    static_cast<u32>(width_ * static_cast<f32>(last_scale_)),
                    static_cast<u32>(height_ * static_cast<f32>(last_scale_)), 0);
                options_.gui_scale =
                    options_.gui_scale >= static_cast<i32>(most) ? 0 : options_.gui_scale + 1;
                options_changed_ = true;
            }
            dirty_ = true;
            break;
        case MenuScreen::Controls:
            if (id == "mouse") {
                open(MenuScreen::Mouse);
            } else if (id == "keybinds") {
                open(MenuScreen::KeyBinds);
            } else if (id == "operator_tab") {  // ── allow-commands ──
                options_.operator_items_tab = !options_.operator_items_tab;
                options_changed_            = true;
                dirty_                      = true;
            }
            break;
        case MenuScreen::KeyBinds:
            if (id.starts_with("key:")) {
                waiting_for_key_ = id.substr(4);
            } else if (id.starts_with("reset:")) {
                if (client::KeyBinding* key = options_.binding(id.substr(6))) {
                    key->code        = key->default_code;
                    options_changed_ = true;
                }
            } else if (id == "reset_all") {
                for (client::KeyBinding& key : options_.keys) {
                    key.code = key.default_code;
                }
                options_changed_ = true;
            }
            dirty_ = true;
            break;
        case MenuScreen::SelectWorld:
            if (id == "search") {
                focus_ = "search";
            } else if (id == "play" && selected_world_ >= 0 &&
                       selected_world_ < static_cast<i32>(worlds_.size())) {
                const SavedWorld& world = worlds_[static_cast<usize>(selected_world_)];
                action.kind       = MenuAction::Kind::Play;
                action.world      = world.path;
                action.world_name = world.name;
                action.survival   = world.game_type != 1;
            } else if (id == "create") {
                open(MenuScreen::CreateWorld);
            } else if (id == "cancel") {
                open(MenuScreen::Title);
            }
            break;
        case MenuScreen::CreateWorld:
            if (id.starts_with("tab")) {
                create_tab_ = id[3] - '0';
                focus_      = create_tab_ == 0 ? "world_name" : (create_tab_ == 1 ? "seed" : "");
            } else if (id == "world_name" || id == "seed") {
                focus_ = id;
            } else if (id == "game_mode") {
                // ── allow-commands ── vanilla cycles Survival, Hardcore,
                // Creative. Hardcore is skipped: this server has no hardcore
                // (commandes-solo.md § 6), and a world that says hardcore and
                // is not would be worse than none.
                create_mode_ = client::next_game_mode(create_mode_);
                if (create_mode_ == client::CreateGameMode::Hardcore) {
                    create_mode_ = client::next_game_mode(create_mode_);
                }
            } else if (id == "cheats") {  // ── allow-commands ──
                create_cheats_.press(create_mode_);
            } else if (id == "world_type") {
                create_flat_ = !create_flat_;
            } else if (id == "create") {
                const std::string name = world_name_.value().empty()
                                             ? translate("selectWorld.newWorld")
                                             : world_name_.value();
                action.kind       = MenuAction::Kind::Play;
                action.create     = true;
                action.world      = config_.saves / world_folder_for(config_.saves, name);
                action.world_name = name;
                action.survival       = create_mode_ != client::CreateGameMode::Creative;
                action.allow_commands = create_cheats_.value(create_mode_);  // ── allow-commands ──
                if (!create_flat_) {
                    action.seed = client::seed_from_text(seed_field_.value())
                                      .value_or(config_.random_seed);
                }
            } else if (id == "cancel") {
                return back();
            }
            dirty_ = true;
            break;
        case MenuScreen::DirectConnect:
            if (id == "join" && !address_.value().empty()) {
                action.kind    = MenuAction::Kind::Connect;
                action.address = address_.value();
            } else if (id == "cancel") {
                open(MenuScreen::Title);
            }
            break;
        case MenuScreen::Language: break;
        default: break;
    }
    return action;
}

MenuAction Menus::press(std::string_view id) {
    if (dirty_) {
        rebuild();
    }
    for (const client::Widget& widget : widgets_) {
        if (widget.id == id) {
            if (!widget.active) {
                OV_LOG_WARN("menu: {} is inactive on {}", id, to_string(screen_));
                return {};
            }
            const client::Widget copy = widget;
            return activate(copy);
        }
    }
    OV_LOG_WARN("menu: no widget {} on {}", id, to_string(screen_));
    return {};
}

void Menus::type(std::string_view text) {
    if (client::TextField* field = focused_field()) {
        (void)field->insert(text);
        dirty_ = true;
    }
}

MenuAction Menus::update(const client::InputState& input, client::Window& window,
                         f64 delta_seconds) {
    clock_ += delta_seconds;
    const u32 fb_w  = std::max(window.framebuffer_width(), 1U);
    const u32 fb_h  = std::max(window.framebuffer_height(), 1U);
    const u32 scale = gui_scale(fb_w, fb_h);
    const f32 w     = std::ceil(static_cast<f32>(fb_w) / static_cast<f32>(scale));
    const f32 h     = std::ceil(static_cast<f32>(fb_h) / static_cast<f32>(scale));
    if (w != width_ || h != height_ || scale != last_scale_) {
        width_      = w;
        height_     = h;
        last_scale_ = scale;
        dirty_      = true;
    }
    mouse_x_ = static_cast<f32>(input.mouse_x) / static_cast<f32>(scale);
    mouse_y_ = static_cast<f32>(input.mouse_y) / static_cast<f32>(scale);

    MenuAction action;
    client::TextField* field = focused_field();
    if (input.just_pressed(client::Key::Reload) && field == nullptr) {
        toggle_debug();
    }
    if (screen_ == MenuScreen::None || screen_ == MenuScreen::Message) {
        return action;
    }
    if (screen_ == MenuScreen::Death && clock_ - death_opened_at_ < kDeathButtonDelay + 0.1) {
        dirty_ = true;  // the buttons wake up after the delay
    }

    // A key binding waiting for its key takes the next one, Escape unbinding.
    if (!waiting_for_key_.empty()) {
        if (!input.codes_pressed.empty()) {
            const i32 code = input.codes_pressed.front();
            if (client::KeyBinding* key = options_.binding(waiting_for_key_)) {
                key->code = code == 256 ? client::kUnknownKey : code;
            }
            waiting_for_key_.clear();
            options_changed_ = true;
            save_options();
            dirty_ = true;
        }
        return action;
    }

    for (const client::KeyEvent& event : input.key_events) {
        if (event.key == client::EditKey::Escape) {
            if (screen_ != MenuScreen::Title && screen_ != MenuScreen::Death) {
                return back();
            }
            continue;
        }
        if (field != nullptr) {
            if (event.key == client::EditKey::Enter) {
                if (screen_ == MenuScreen::DirectConnect) {
                    return press("join");
                }
                if (screen_ == MenuScreen::CreateWorld) {
                    return press("create");
                }
                continue;
            }
            if (event.key == client::EditKey::Tab && screen_ == MenuScreen::CreateWorld) {
                continue;
            }
            if (field->key(event, clipboard_) != client::FieldResult::Ignored) {
                dirty_ = true;
            }
        }
    }
    if (field != nullptr && !input.typed.empty()) {
        (void)field->insert(input.typed);
        dirty_ = true;
    }

    if (dirty_) {
        rebuild();
    }

    if (input.scroll != 0.0) {
        const f32 notch = static_cast<f32>(input.scroll);
        if (screen_ == MenuScreen::KeyBinds) {
            keys_scroll_ = std::max(0.0F, keys_scroll_ - notch * 10.0F);
            dirty_       = true;
        } else if (screen_ == MenuScreen::SelectWorld) {
            world_scroll_ = std::max(0.0F, world_scroll_ - notch * 18.0F);
        } else if (screen_ == MenuScreen::Language) {
            language_scroll_ = std::max(0.0F, language_scroll_ - notch * 9.0F);
        }
    }

    if (input.attack_pressed) {
        if (const client::Widget* hit = client::widget_at(widgets_, mouse_x_, mouse_y_)) {
            const client::Widget widget = *hit;
            if (widget.kind == client::WidgetKind::Slider) {
                dragging_ = widget.id;
                set_slider(widget.id, client::slider_value_at(widget.rect, mouse_x_));
            } else if (widget.kind == client::WidgetKind::EditBox) {
                focus_ = widget.id;
                dirty_ = true;
            } else if (widget.active) {
                action = activate(widget);
            }
        } else if (screen_ == MenuScreen::SelectWorld) {
            const client::ListBox box = client::select_world_list(width_, height_);
            const f32             left = std::floor(width_ / 2.0F) - box.row_width / 2.0F;
            if (mouse_y_ >= box.top && mouse_y_ < box.bottom && mouse_x_ >= left &&
                mouse_x_ < left + box.row_width) {
                const i32 row = static_cast<i32>(
                    std::floor((mouse_y_ - box.first_row + world_scroll_) / box.row_height));
                if (row >= 0 && row < static_cast<i32>(worlds_.size())) {
                    const bool twice = row == selected_world_ &&
                                       clock_ - last_world_click_ < kDoubleClickSeconds;
                    selected_world_   = row;
                    last_world_click_ = clock_;
                    dirty_            = true;
                    if (twice) {
                        rebuild();
                        action = press("play");
                    }
                }
            }
        } else if (screen_ == MenuScreen::Language) {
            const client::ListBox box  = client::language_list(width_, height_);
            const f32             left = std::floor(width_ / 2.0F) - box.row_width / 2.0F;
            if (mouse_y_ >= box.top && mouse_y_ < box.bottom && mouse_x_ >= left &&
                mouse_x_ < left + box.row_width) {
                const i32 row = static_cast<i32>(
                    std::floor((mouse_y_ - box.first_row + language_scroll_) / box.row_height));
                if (row >= 0 && row < static_cast<i32>(languages_.size())) {
                    options_.language = languages_[static_cast<usize>(row)].first;
                    options_changed_  = true;
                    dirty_            = true;
                }
            }
        }
    }
    if (!dragging_.empty()) {
        if (input.attack_held) {
            if (const client::Widget* slider = client::find_widget(widgets_, dragging_)) {
                set_slider(dragging_, client::slider_value_at(slider->rect, mouse_x_));
            }
        }
        if (input.attack_released || !input.attack_held) {
            dragging_.clear();
            save_options();
        }
    }
    if (dirty_) {
        rebuild();
    }
    return action;
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void Menus::draw_panorama(f32 width, f32 height) {
    // The cube around the camera, each face cut into a grid so the affine
    // interpolation of the GUI's quads stays close to a perspective one. The
    // camera turns slowly, as vanilla's does behind the title.
    constexpr i32 kGrid       = 12;
    const f32     yaw         = static_cast<f32>(clock_ * 0.8 * std::numbers::pi / 180.0);
    const f32     pitch       = 10.0F * std::numbers::pi_v<f32> / 180.0F;
    const f32     focal       = (height * 0.5F) / std::tan(85.0F * std::numbers::pi_v<f32> / 360.0F);
    const auto    project     = [&](f32 x, f32 y, f32 z, client::GuiPoint& out) {
        // Turn the world by the camera's yaw and pitch, then divide.
        const f32 cx = x * std::cos(yaw) - z * std::sin(yaw);
        const f32 cz = x * std::sin(yaw) + z * std::cos(yaw);
        const f32 cy = y * std::cos(pitch) - cz * std::sin(pitch);
        const f32 dz = y * std::sin(pitch) + cz * std::cos(pitch);
        // Looking down −Z: depth is −dz.
        const f32 depth = -dz;
        if (depth <= 0.05F) {
            return false;
        }
        out.x = width * 0.5F + cx / depth * focal;
        out.y = height * 0.5F - cy / depth * focal;
        return true;
    };
    // Faces as (origin, u axis, v axis) on the unit cube, seen from inside:
    // 0 front (−Z), 1 right (+X), 2 back (+Z), 3 left (−X), 4 top, 5 bottom.
    struct Face {
        std::array<f32, 3> origin;
        std::array<f32, 3> u;
        std::array<f32, 3> v;
    };
    static constexpr std::array<Face, 6> kFaces{{
        {{-1, 1, -1}, {2, 0, 0}, {0, -2, 0}},
        {{1, 1, -1}, {0, 0, 2}, {0, -2, 0}},
        {{1, 1, 1}, {-2, 0, 0}, {0, -2, 0}},
        {{-1, 1, 1}, {0, 0, -2}, {0, -2, 0}},
        {{-1, 1, 1}, {2, 0, 0}, {0, 0, -2}},
        {{-1, -1, -1}, {2, 0, 0}, {0, 0, 2}},
    }};
    gui_->fill(0.0F, 0.0F, width, height, 0xFF000000U);
    for (usize f = 0; f < kFaces.size(); ++f) {
        if (textures_.panorama[f] == client::GuiTexture::Invalid) {
            continue;
        }
        const Face& face = kFaces[f];
        for (i32 gy = 0; gy < kGrid; ++gy) {
            for (i32 gx = 0; gx < kGrid; ++gx) {
                std::array<client::GuiPoint, 4> corners{};
                std::array<client::GuiPoint, 4> uvs{};
                bool                            visible = true;
                static constexpr std::array<std::array<i32, 2>, 4> kCorner{
                    {{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
                for (usize c = 0; c < 4 && visible; ++c) {
                    const f32 s = static_cast<f32>(gx + kCorner[c][0]) / kGrid;
                    const f32 t = static_cast<f32>(gy + kCorner[c][1]) / kGrid;
                    const f32 x = face.origin[0] + face.u[0] * s + face.v[0] * t;
                    const f32 y = face.origin[1] + face.u[1] * s + face.v[1] * t;
                    const f32 z = face.origin[2] + face.u[2] * s + face.v[2] * t;
                    visible     = project(x, y, z, corners[c]);
                    uvs[c]      = client::GuiPoint{s, t};
                }
                if (visible) {
                    gui_->quad_corners(textures_.panorama[f], corners, uvs, 0xFFFFFFFFU);
                }
            }
        }
    }
}

void Menus::draw_background(f32 width, f32 height) {
    if (screen_ == MenuScreen::Title) {
        draw_panorama(width, height);
        return;
    }
    if (screen_ == MenuScreen::Death) {
        gui_->gradient(0.0F, 0.0F, width, height, 0x60500000U, 0xA0803030U);
        return;
    }
    if (in_game_ && screen_ != MenuScreen::Message) {
        gui_->gradient(0.0F, 0.0F, width, height, 0xC0101010U, 0xD0101010U);
        return;
    }
    client::draw_dirt(*gui_, textures_, client::Rect{0.0F, 0.0F, width, height});
}

void Menus::draw_title_screen(f32 width, f32 height) {
    const f32 x = std::floor(width / 2.0F) - 128.0F;
    if (textures_.logo != client::GuiTexture::Invalid) {
        gui_->blit(textures_.logo, x, 30.0F, 256.0F, 44.0F, 0.0F, 0.0F, 256.0F, 44.0F, 256.0F,
                   64.0F);
    }
    if (textures_.edition != client::GuiTexture::Invalid) {
        // The whole label: in 1.20.1's edition.png "JAVA EDITION" spans all
        // 128 logical columns and 14 rows. A 98-wide crop, from an older
        // artwork, cut it to "JAVA EDIT!" (docs/provenance/rendu-parite.md).
        // Centred under the 256-wide logo: measured ink, centring deduced.
        gui_->blit(textures_.edition, x + 64.0F, 67.0F, 128.0F, 14.0F, 0.0F, 0.0F, 128.0F, 14.0F,
                   128.0F, 16.0F);
    }
    // Not vanilla's "Minecraft 1.20.1" and copyright line: this is not
    // Minecraft, and the title screen says so (CLAUDE.md § 1, NOTICE).
    gui_->text(2.0F, height - 10.0F, "Ondes VOXEL 1.20.1", 0xFFFFFFFFU);
    const std::string_view notice = "Not an official Minecraft product";
    gui_->text_right(width - 2.0F, height - 10.0F, notice, 0xFFFFFFFFU);
    // The two icon buttons: the globe from widgets.png, the figure from
    // accessibility.png — both drawn as plain buttons here, their icons not
    // yet (named in ecrans.md).
}

void Menus::draw(rhi::CommandList& cmd, u32 framebuffer_width, u32 framebuffer_height) {
    const u32 scale = gui_scale(framebuffer_width, framebuffer_height);
    gui_->begin(framebuffer_width, framebuffer_height, scale);
    const f32 w = gui_->width();
    const f32 h = gui_->height();
    if (w != width_ || h != height_) {
        width_  = w;
        height_ = h;
        dirty_  = true;
    }
    if (dirty_) {
        rebuild();
    }
    if (debug_ && in_game_) {
        client::draw_debug_overlay(*gui_, debug_info_);
    }
    if (screen_ == MenuScreen::None) {
        gui_->flush(cmd);
        return;
    }
    draw_background(w, h);
    const auto grey = 0xFFA0A0A0U;
    switch (screen_) {
        case MenuScreen::Title: draw_title_screen(w, h); break;
        case MenuScreen::Message:
            gui_->text_centred(w * 0.5F, 70.0F, message_, 0xFFFFFFFFU);
            break;
        case MenuScreen::Death: {
            const std::string title =
                translate(hardcore_ ? "deathScreen.title.hardcore" : "deathScreen.title");
            const f32 title_w = gui_->font().width(title) * 2.0F;
            gui_->text_scaled(w * 0.5F - title_w * 0.5F, 60.0F, title, 0xFFFFFFFFU, 2.0F);
            if (!death_cause_.empty()) {
                gui_->text_centred(w * 0.5F, 85.0F, death_cause_, 0xFFFFFFFFU);
            }
            const std::array<std::string, 1> score{"§e" + std::to_string(death_score_)};
            gui_->text_centred(w * 0.5F, 100.0F,
                               translate("deathScreen.score") + ": §e" +
                                   std::to_string(death_score_),
                               0xFFFFFFFFU);
            (void)score;
            break;
        }
        case MenuScreen::SelectWorld: {
            const client::ListBox box  = client::select_world_list(w, h);
            const f32             left = std::floor(w / 2.0F) - box.row_width / 2.0F;
            for (usize i = 0; i < worlds_.size(); ++i) {
                const f32 top = box.first_row + box.row_height * static_cast<f32>(i) - world_scroll_;
                if (top < box.top || top + box.row_height > box.bottom) {
                    continue;
                }
                const SavedWorld& world = worlds_[i];
                if (static_cast<i32>(i) == selected_world_) {
                    gui_->fill(left - 2.0F, top - 2.0F, box.row_width, box.row_height, 0xFF808080U);
                    gui_->fill(left - 1.0F, top - 1.0F, box.row_width - 2.0F, box.row_height - 2.0F,
                               0xFF000000U);
                }
                gui_->fill(left, top, 32.0F, 32.0F, 0xFF303030U);
                gui_->text(left + 35.0F, top + 1.0F, world.name, 0xFFFFFFFFU);
                std::string date;
                if (world.last_played > 0) {
                    const std::time_t seconds = static_cast<std::time_t>(world.last_played / 1000);
                    std::tm           local{};
                    localtime_r(&seconds, &local);
                    std::array<char, 32> text{};
                    std::strftime(text.data(), text.size(), "%d/%m/%Y %H:%M", &local);
                    date = text.data();
                }
                gui_->text(left + 35.0F, top + 12.0F, world.folder + " (" + date + ")", 0xFF808080U);
                // ── allow-commands ── the vanilla client's line, measured
                // (commandes-solo.md § 5): the mode, ", Cheats" when the world
                // allows commands, ", Version: <Data.Version.Name>".
                std::string info = translate(world.game_type == 1 ? "gameMode.creative"
                                                                  : "gameMode.survival");
                if (world.allow_commands) {
                    info += ", " + translate("selectWorld.cheats");
                }
                if (!world.version_name.empty()) {
                    info += ", " + translate("selectWorld.version") + " " + world.version_name;
                }
                gui_->text(left + 35.0F, top + 21.0F, info, 0xFF808080U);
            }
            if (worlds_.empty()) {
                gui_->text_centred(w * 0.5F, box.top + 20.0F, translate("selectWorld.noWorlds"), grey);
            }
            break;
        }
        case MenuScreen::Language: {
            const client::ListBox box = client::language_list(w, h);
            for (usize i = 0; i < languages_.size(); ++i) {
                const f32 top =
                    box.first_row + box.row_height * static_cast<f32>(i) - language_scroll_;
                if (top < box.top || top + box.row_height > box.bottom) {
                    continue;
                }
                const bool chosen = languages_[i].first == options_.language;
                if (chosen) {
                    const f32 left = std::floor(w / 2.0F) - box.row_width / 2.0F;
                    gui_->fill(left - 2.0F, top - 2.0F, box.row_width, box.row_height, 0xFF808080U);
                    gui_->fill(left - 1.0F, top - 1.0F, box.row_width - 2.0F,
                               box.row_height - 2.0F, 0xFF000000U);
                }
                gui_->text_centred(w * 0.5F, top + 1.0F, languages_[i].second, 0xFFFFFFFFU);
            }
            gui_->text_centred(w * 0.5F, h - 56.0F, translate("options.languageWarning"), grey);
            break;
        }
        case MenuScreen::CreateWorld: {
            // The tabbed screen's frame, read off the vanilla capture: a black
            // bar behind the tabs with the header separator under it, and the
            // footer separator 36 pixels above the bottom.
            gui_->fill(0.0F, 0.0F, w, 24.0F, 0xFF000000U);
            if (textures_.header_separator != client::GuiTexture::Invalid) {
                gui_->quad(textures_.header_separator, 0.0F, 22.0F, w, 2.0F, 0.0F, 0.0F,
                           w / 32.0F, 1.0F);
            }
            if (textures_.footer_separator != client::GuiTexture::Invalid) {
                gui_->quad(textures_.footer_separator, 0.0F, h - 36.0F, w, 2.0F, 0.0F, 0.0F,
                           w / 32.0F, 1.0F);
            }
            break;
        }
        case MenuScreen::DirectConnect:
            gui_->text(std::floor(w / 2.0F) - 100.0F, 100.0F, translate("addServer.enterIp"), grey);
            break;
        default: break;
    }
    if (!title_.empty()) {
        // The world list's title sits above its search box (y 22).
        const f32 y = screen_ == MenuScreen::Pause         ? 40.0F
                      : screen_ == MenuScreen::Options     ? 15.0F
                      : screen_ == MenuScreen::SelectWorld ? 8.0F
                                                           : 20.0F;
        gui_->text_centred(w * 0.5F, y, translate(title_), 0xFFFFFFFFU);
    }
    const bool cursor_on = static_cast<i64>(clock_ / 0.3) % 2 == 0;
    client::draw_widgets(*gui_, textures_, widgets_, mouse_x_, mouse_y_, cursor_on);
    // The two image buttons of the title: the globe from widgets.png at
    // (0, 106), hovered at (0, 126); the figure from accessibility.png.
    for (const client::Widget& widget : widgets_) {
        if (widget.kind != client::WidgetKind::Icon) {
            continue;
        }
        const bool hovered = widget.rect.contains(mouse_x_, mouse_y_);
        const client::Rect& r = widget.rect;
        if (widget.id == "language" && textures_.widgets != client::GuiTexture::Invalid) {
            gui_->blit(textures_.widgets, r.x, r.y, 20.0F, 20.0F, 0.0F, hovered ? 126.0F : 106.0F,
                       20.0F, 20.0F);
        } else if (widget.id == "accessibility" &&
                   textures_.accessibility != client::GuiTexture::Invalid) {
            gui_->blit(textures_.accessibility, r.x, r.y, 20.0F, 20.0F, 0.0F,
                       hovered ? 20.0F : 0.0F, 20.0F, 20.0F, 32.0F, 64.0F);
        }
    }
    gui_->flush(cmd);
}

std::string Menus::describe() const {
    std::string out = fmt::format("menu {} gui {}x{}", to_string(screen_), width_, height_);
    for (const client::Widget& widget : widgets_) {
        out += fmt::format("\n  {} {},{} {}x{} active {} \"{}\"", widget.id, widget.rect.x,
                           widget.rect.y, widget.rect.w, widget.rect.h, widget.active, widget.text);
    }
    return out;
}

}  // namespace ov::demo
