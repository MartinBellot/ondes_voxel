#include "ov/client/options_file.hpp"

#include "ov/client/gui.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>

namespace ov::client {
namespace {

/// GLFW's public key codes and vanilla's names for them. The names are the
/// ones options.txt carries (`key_key.forward:key.keyboard.w`).
struct NamedKey {
    i32              code;
    std::string_view name;
};

constexpr std::array<NamedKey, 32> kNamedKeys{{
    {32, "key.keyboard.space"},        {39, "key.keyboard.apostrophe"},
    {44, "key.keyboard.comma"},        {45, "key.keyboard.minus"},
    {46, "key.keyboard.period"},       {47, "key.keyboard.slash"},
    {59, "key.keyboard.semicolon"},    {61, "key.keyboard.equal"},
    {91, "key.keyboard.left.bracket"}, {92, "key.keyboard.backslash"},
    {93, "key.keyboard.right.bracket"}, {96, "key.keyboard.grave.accent"},
    {161, "key.keyboard.world.1"},     {162, "key.keyboard.world.2"},
    {256, "key.keyboard.escape"},      {257, "key.keyboard.enter"},
    {258, "key.keyboard.tab"},         {259, "key.keyboard.backspace"},
    {260, "key.keyboard.insert"},      {261, "key.keyboard.delete"},
    {262, "key.keyboard.right"},       {263, "key.keyboard.left"},
    {264, "key.keyboard.down"},        {265, "key.keyboard.up"},
    {266, "key.keyboard.page.up"},     {267, "key.keyboard.page.down"},
    {268, "key.keyboard.home"},        {269, "key.keyboard.end"},
    {280, "key.keyboard.caps.lock"},   {281, "key.keyboard.scroll.lock"},
    {282, "key.keyboard.num.lock"},    {283, "key.keyboard.print.screen"},
}};

constexpr std::array<NamedKey, 26> kMoreKeys{{
    {284, "key.keyboard.pause"},          {330, "key.keyboard.keypad.decimal"},
    {331, "key.keyboard.keypad.divide"},  {332, "key.keyboard.keypad.multiply"},
    {333, "key.keyboard.keypad.subtract"}, {334, "key.keyboard.keypad.add"},
    {335, "key.keyboard.keypad.enter"},   {336, "key.keyboard.keypad.equal"},
    {340, "key.keyboard.left.shift"},     {341, "key.keyboard.left.control"},
    {342, "key.keyboard.left.alt"},       {343, "key.keyboard.left.win"},
    {344, "key.keyboard.right.shift"},    {345, "key.keyboard.right.control"},
    {346, "key.keyboard.right.alt"},      {347, "key.keyboard.right.win"},
    {348, "key.keyboard.menu"},           {kMouseCodeBase + 0, "key.mouse.left"},
    {kMouseCodeBase + 1, "key.mouse.right"}, {kMouseCodeBase + 2, "key.mouse.middle"},
    {kMouseCodeBase + 3, "key.mouse.4"},  {kMouseCodeBase + 4, "key.mouse.5"},
    {kMouseCodeBase + 5, "key.mouse.6"},  {kMouseCodeBase + 6, "key.mouse.7"},
    {kMouseCodeBase + 7, "key.mouse.8"},  {kUnknownKey, "key.keyboard.unknown"},
}};

constexpr i32 kKeyA  = 65;
constexpr i32 kKey0  = 48;
constexpr i32 kKeyF1 = 290;
constexpr i32 kKeypad0 = 320;

template<typename T>
[[nodiscard]] std::optional<T> parse_number(std::string_view text) {
    T          value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view text) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

/// Vanilla quotes some string options (`mainHand:"right"`) and not others
/// (`lang:en_us`). Reading accepts either.
[[nodiscard]] std::string_view unquote(std::string_view text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

}  // namespace

std::string java_double(f64 value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    if (value == 0.0) {
        return std::signbit(value) ? "-0.0" : "0.0";
    }
    std::array<char, 64> buffer{};
    const f64            magnitude = std::fabs(value);
    if (magnitude >= 1e-3 && magnitude < 1e7) {
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                          std::chars_format::fixed);
        std::string text(buffer.data(), result.ptr);
        if (text.find('.') == std::string::npos) {
            text += ".0";
        }
        return text;
    }
    const auto  result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                       std::chars_format::scientific);
    std::string text(buffer.data(), result.ptr);
    const auto  e        = text.find('e');
    std::string mantissa = text.substr(0, e);
    if (mantissa.find('.') == std::string::npos) {
        mantissa += ".0";
    }
    const i32 exponent = std::atoi(text.c_str() + e + 1);
    return mantissa + "E" + std::to_string(exponent);
}

// ── OptionsFile ─────────────────────────────────────────────────────────────

OptionsFile OptionsFile::parse(std::string_view text) {
    OptionsFile file;
    usize       start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        const usize colon = line.find(':');
        if (colon == std::string_view::npos) {
            file.entries_.push_back(Entry{std::string(line), {}, false});
        } else {
            file.entries_.push_back(
                Entry{std::string(line.substr(0, colon)), std::string(line.substr(colon + 1)), true});
        }
    }
    return file;
}

std::expected<OptionsFile, std::string> OptionsFile::load(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return OptionsFile{};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot read " + path.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str());
}

std::string OptionsFile::serialize() const {
    std::string out;
    for (const Entry& entry : entries_) {
        out += entry.key;
        if (entry.has_colon) {
            out += ':';
            out += entry.value;
        }
        out += '\n';
    }
    return out;
}

std::expected<void, std::string> OptionsFile::save(const std::filesystem::path& path) const {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected("cannot write " + temporary.string());
        }
        const std::string text = serialize();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return std::unexpected("cannot write " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        return std::unexpected("cannot replace " + path.string() + ": " + error.message());
    }
    return {};
}

std::optional<std::string_view> OptionsFile::get(std::string_view key) const {
    for (const Entry& entry : entries_) {
        if (entry.has_colon && entry.key == key) {
            return std::string_view(entry.value);
        }
    }
    return std::nullopt;
}

void OptionsFile::set(std::string_view key, std::string value) {
    for (Entry& entry : entries_) {
        if (entry.has_colon && entry.key == key) {
            entry.value = std::move(value);
            return;
        }
    }
    entries_.push_back(Entry{std::string(key), std::move(value), true});
}

// ── Keys ────────────────────────────────────────────────────────────────────

std::string key_name(i32 code) {
    if (code >= kKeyA && code < kKeyA + 26) {
        return std::string("key.keyboard.") + static_cast<char>('a' + (code - kKeyA));
    }
    if (code >= kKey0 && code < kKey0 + 10) {
        return std::string("key.keyboard.") + static_cast<char>('0' + (code - kKey0));
    }
    if (code >= kKeyF1 && code < kKeyF1 + 25) {
        return "key.keyboard.f" + std::to_string(code - kKeyF1 + 1);
    }
    if (code >= kKeypad0 && code < kKeypad0 + 10) {
        return "key.keyboard.keypad." + std::to_string(code - kKeypad0);
    }
    for (const auto& table : {std::span<const NamedKey>(kNamedKeys), std::span<const NamedKey>(kMoreKeys)}) {
        for (const NamedKey& key : table) {
            if (key.code == code) {
                return std::string(key.name);
            }
        }
    }
    return "key.keyboard.unknown";
}

i32 key_code(std::string_view name) noexcept {
    constexpr std::string_view kKeyboard = "key.keyboard.";
    if (name.starts_with(kKeyboard)) {
        const std::string_view rest = name.substr(kKeyboard.size());
        if (rest.size() == 1 && rest[0] >= 'a' && rest[0] <= 'z') {
            return kKeyA + (rest[0] - 'a');
        }
        if (rest.size() == 1 && rest[0] >= '0' && rest[0] <= '9') {
            return kKey0 + (rest[0] - '0');
        }
        if (rest.size() >= 2 && rest[0] == 'f') {
            if (const auto number = parse_number<i32>(rest.substr(1)); number && *number >= 1 &&
                                                                        *number <= 25) {
                return kKeyF1 + *number - 1;
            }
        }
        constexpr std::string_view kKeypad = "keypad.";
        if (rest.starts_with(kKeypad) && rest.size() == kKeypad.size() + 1 &&
            rest.back() >= '0' && rest.back() <= '9') {
            return kKeypad0 + (rest.back() - '0');
        }
    }
    for (const auto& table : {std::span<const NamedKey>(kNamedKeys), std::span<const NamedKey>(kMoreKeys)}) {
        for (const NamedKey& key : table) {
            if (key.name == name) {
                return key.code;
            }
        }
    }
    return kUnknownKey;
}

std::vector<KeyBinding> default_key_bindings() {
    constexpr std::string_view kMovement  = "key.categories.movement";
    constexpr std::string_view kGameplay  = "key.categories.gameplay";
    constexpr std::string_view kInventory = "key.categories.inventory";
    constexpr std::string_view kCreative  = "key.categories.creative";
    constexpr std::string_view kMulti     = "key.categories.multiplayer";
    constexpr std::string_view kMisc      = "key.categories.misc";
    const auto k = [](std::string_view key) { return key_code(key); };
    std::vector<KeyBinding> keys{
        {"key.attack", kGameplay, k("key.mouse.left"), k("key.mouse.left")},
        {"key.use", kGameplay, k("key.mouse.right"), k("key.mouse.right")},
        {"key.forward", kMovement, k("key.keyboard.w"), k("key.keyboard.w")},
        {"key.left", kMovement, k("key.keyboard.a"), k("key.keyboard.a")},
        {"key.back", kMovement, k("key.keyboard.s"), k("key.keyboard.s")},
        {"key.right", kMovement, k("key.keyboard.d"), k("key.keyboard.d")},
        {"key.jump", kMovement, k("key.keyboard.space"), k("key.keyboard.space")},
        {"key.sneak", kMovement, k("key.keyboard.left.shift"), k("key.keyboard.left.shift")},
        {"key.sprint", kMovement, k("key.keyboard.left.control"), k("key.keyboard.left.control")},
        {"key.drop", kInventory, k("key.keyboard.q"), k("key.keyboard.q")},
        {"key.inventory", kInventory, k("key.keyboard.e"), k("key.keyboard.e")},
        {"key.chat", kMulti, k("key.keyboard.t"), k("key.keyboard.t")},
        {"key.playerlist", kMulti, k("key.keyboard.tab"), k("key.keyboard.tab")},
        {"key.pickItem", kGameplay, k("key.mouse.middle"), k("key.mouse.middle")},
        {"key.command", kMulti, k("key.keyboard.slash"), k("key.keyboard.slash")},
        {"key.socialInteractions", kMulti, k("key.keyboard.p"), k("key.keyboard.p")},
        {"key.screenshot", kMisc, k("key.keyboard.f2"), k("key.keyboard.f2")},
        {"key.togglePerspective", kMisc, k("key.keyboard.f5"), k("key.keyboard.f5")},
        {"key.smoothCamera", kMisc, kUnknownKey, kUnknownKey},
        {"key.fullscreen", kMisc, k("key.keyboard.f11"), k("key.keyboard.f11")},
        {"key.spectatorOutlines", kMisc, kUnknownKey, kUnknownKey},
        {"key.swapOffhand", kInventory, k("key.keyboard.f"), k("key.keyboard.f")},
        {"key.saveToolbarActivator", kCreative, k("key.keyboard.c"), k("key.keyboard.c")},
        {"key.loadToolbarActivator", kCreative, k("key.keyboard.x"), k("key.keyboard.x")},
        {"key.advancements", kMisc, k("key.keyboard.l"), k("key.keyboard.l")},
    };
    static constexpr std::array<std::string_view, 9> kHotbar{
        "key.hotbar.1", "key.hotbar.2", "key.hotbar.3", "key.hotbar.4", "key.hotbar.5",
        "key.hotbar.6", "key.hotbar.7", "key.hotbar.8", "key.hotbar.9"};
    for (usize i = 0; i < kHotbar.size(); ++i) {
        const i32 code = kKey0 + 1 + static_cast<i32>(i);
        keys.push_back(KeyBinding{kHotbar[i], kInventory, code, code});
    }
    return keys;
}

// ── GameOptions ─────────────────────────────────────────────────────────────

GameOptions GameOptions::from(const OptionsFile& file, std::vector<std::string>* problems) {
    GameOptions options;
    const auto  refuse = [&](std::string_view key, std::string_view value) {
        if (problems != nullptr) {
            problems->push_back(std::string(key) + ":" + std::string(value) +
                                " does not read; the default is kept");
        }
    };
    const auto integer = [&](std::string_view key, i32& field, i32 low, i32 high) {
        if (const auto value = file.get(key)) {
            if (const auto parsed = parse_number<i32>(*value)) {
                field = std::clamp(*parsed, low, high);
            } else {
                refuse(key, *value);
            }
        }
    };
    const auto real = [&](std::string_view key, f64& field, f64 low, f64 high) {
        if (const auto value = file.get(key)) {
            if (const auto parsed = parse_number<f64>(*value)) {
                field = std::clamp(*parsed, low, high);
            } else {
                refuse(key, *value);
            }
        }
    };
    const auto flag = [&](std::string_view key, bool& field) {
        if (const auto value = file.get(key)) {
            if (const auto parsed = parse_bool(*value)) {
                field = *parsed;
            } else {
                refuse(key, *value);
            }
        }
    };

    if (const auto value = file.get("fov")) {
        if (const auto parsed = parse_number<f64>(*value)) {
            options.fov = std::clamp(static_cast<i32>(std::lround(*parsed * 40.0 + 70.0)), 30, 110);
        } else {
            refuse("fov", *value);
        }
    }
    integer("renderDistance", options.render_distance, 2, 32);
    integer("simulationDistance", options.simulation_distance, 5, 32);
    integer("guiScale", options.gui_scale, 0, 1000);
    real("mouseSensitivity", options.mouse_sensitivity, 0.0, 1.0);
    flag("enableVsync", options.vsync);
    integer("maxFps", options.max_fps, 10, 260);
    real("gamma", options.gamma, 0.0, 1.0);
    flag("pauseOnLostFocus", options.pause_on_lost_focus);
    flag("operatorItemsTab", options.operator_items_tab);  // ── allow-commands ──
    flag("showSubtitles", options.show_subtitles);         // ── sound ──
    real("notificationDisplayTime", options.notification_display_time, 0.5, 10.0);
    if (const auto value = file.get("lang")) {
        options.language = std::string(unquote(*value));
    }
    for (usize i = 0; i < kSoundCategoryNames.size(); ++i) {
        real(std::string("soundCategory_") + std::string(kSoundCategoryNames[i]),
             options.volumes[i], 0.0, 1.0);
    }
    for (KeyBinding& key : options.keys) {
        const std::string option = "key_" + std::string(key.name);
        if (const auto value = file.get(option)) {
            const i32 code = key_code(*value);
            if (code == kUnknownKey && *value != "key.keyboard.unknown") {
                refuse(option, *value);
            } else {
                key.code = code;
            }
        }
    }
    return options;
}

void GameOptions::store(OptionsFile& file) const {
    file.set("fov", java_double((static_cast<f64>(fov) - 70.0) / 40.0));
    file.set("renderDistance", std::to_string(render_distance));
    file.set("simulationDistance", std::to_string(simulation_distance));
    file.set("guiScale", std::to_string(gui_scale));
    file.set("mouseSensitivity", java_double(mouse_sensitivity));
    file.set("enableVsync", vsync ? "true" : "false");
    file.set("maxFps", std::to_string(max_fps));
    file.set("gamma", java_double(gamma));
    file.set("pauseOnLostFocus", pause_on_lost_focus ? "true" : "false");
    file.set("operatorItemsTab", operator_items_tab ? "true" : "false");  // ── allow-commands ──
    file.set("showSubtitles", show_subtitles ? "true" : "false");         // ── sound ──
    file.set("notificationDisplayTime", java_double(notification_display_time));
    file.set("lang", language);
    for (const KeyBinding& key : keys) {
        file.set("key_" + std::string(key.name), key_name(key.code));
    }
    for (usize i = 0; i < kSoundCategoryNames.size(); ++i) {
        file.set(std::string("soundCategory_") + std::string(kSoundCategoryNames[i]),
                 java_double(volumes[i]));
    }
}

const KeyBinding* GameOptions::binding(std::string_view name) const noexcept {
    for (const KeyBinding& key : keys) {
        if (key.name == name) {
            return &key;
        }
    }
    return nullptr;
}

KeyBinding* GameOptions::binding(std::string_view name) noexcept {
    for (KeyBinding& key : keys) {
        if (key.name == name) {
            return &key;
        }
    }
    return nullptr;
}

u32 effective_gui_scale(const GameOptions& options, u32 width, u32 height) noexcept {
    return auto_gui_scale(width, height,
                          options.gui_scale > 0 ? static_cast<u32>(options.gui_scale) : 0U);
}

f32 degrees_per_pixel(f64 sensitivity) noexcept {
    const f64 f = sensitivity * 0.6 + 0.2;
    return static_cast<f32>(f * f * f * 8.0 * 0.15);
}

}  // namespace ov::client
