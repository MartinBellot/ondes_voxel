// options.txt — the player's settings, in vanilla's own file format.
//
// One `key:value` per line, split at the first colon, values written the way
// Java writes them (`0.5`, `1.0`, `true`, `key.keyboard.w`, `"right"`,
// `["vanilla"]`). The format and the key names were read off files the real
// 1.20.1 client wrote — its defaults, and the same file after known values
// were set through its own option objects (scripts/measure_screens.py) — and
// off the options.txt of the user's own PrismLauncher instance. See
// docs/provenance/ecrans.md.
//
// Two layers, on purpose:
//
//   * OptionsFile keeps **every line**, known or not, in order. A file copied
//     from a vanilla game directory is written back with its 120-odd keys
//     intact, the ones this client does not understand included — a settings
//     file that loses half its lines on the first save is how a player's
//     vanilla options get destroyed.
//   * GameOptions is the handful this client acts on, typed. It reads from and
//     writes into an OptionsFile, touching only its own keys.
//
// No GLFW here: a key is named by vanilla's own names (`key.keyboard.w`) and
// by GLFW's numeric codes, which are fixed numbers in GLFW's public header and
// are repeated below as constants.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::client {

/// Java's `Double.toString`, for the values an options file holds: the
/// shortest digits that read back to the same double, at least one decimal
/// (`1.0`), and `1.0E-4` notation below 10⁻³ and from 10⁷ up.
[[nodiscard]] std::string java_double(f64 value);

class OptionsFile {
public:
    /// Every `key:value` line. A line without a colon is kept as a key with
    /// an empty value and no colon, and written back as it was.
    [[nodiscard]] static OptionsFile parse(std::string_view text);

    /// Read a file. A file that does not exist is an empty OptionsFile, not an
    /// error: that is every first start.
    [[nodiscard]] static std::expected<OptionsFile, std::string> load(
        const std::filesystem::path& path);

    [[nodiscard]] std::string serialize() const;

    /// Written whole, through a temporary file and a rename.
    [[nodiscard]] std::expected<void, std::string> save(const std::filesystem::path& path) const;

    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;

    /// Replace the value in place, or append the line if the key is new.
    void set(std::string_view key, std::string value);

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

    struct Entry {
        std::string key;
        std::string value;
        bool        has_colon{true};
    };
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

// ── Keys ────────────────────────────────────────────────────────────────────

/// GLFW's key codes are fixed numbers in its public header. Mouse buttons are
/// not keys there; they are given codes from here up (left, right, middle, 4…).
inline constexpr i32 kMouseCodeBase = 1000;
inline constexpr i32 kUnknownKey    = -1;

/// Vanilla's name of a key or button: `key.keyboard.w`, `key.mouse.left`,
/// `key.keyboard.unknown`.
[[nodiscard]] std::string key_name(i32 code);

/// The inverse; `kUnknownKey` for a name this table does not have.
[[nodiscard]] i32 key_code(std::string_view name) noexcept;

/// One key binding: vanilla's option name (`key.forward`), its category's
/// translation key, and the code bound.
struct KeyBinding {
    std::string_view name;
    std::string_view category;
    i32              code{kUnknownKey};
    i32              default_code{kUnknownKey};
};

/// Every binding vanilla 1.20.1 writes to options.txt, in the order it writes
/// them, with vanilla's defaults.
[[nodiscard]] std::vector<KeyBinding> default_key_bindings();

// ── The typed layer ─────────────────────────────────────────────────────────

/// The sound categories, in options.txt's order — which is also ov_audio's.
inline constexpr std::array<std::string_view, 10> kSoundCategoryNames{
    "master", "music", "record", "weather", "block",
    "hostile", "neutral", "player", "ambient", "voice"};

struct GameOptions {
    /// Degrees, 30 to 110. options.txt stores `(fov − 70) / 40`.
    i32 fov{70};
    /// Chunks, 2 to 32.
    i32 render_distance{12};
    i32 simulation_distance{12};
    /// 0 is automatic.
    i32 gui_scale{0};
    /// 0 to 1; 0.5 is vanilla's "100%".
    f64  mouse_sensitivity{0.5};
    bool vsync{true};
    /// 10 to 260; 260 is "Unlimited".
    i32  max_fps{120};
    f64  gamma{0.5};
    std::string language{"en_us"};
    /// Indexed as kSoundCategoryNames.
    std::array<f64, 10> volumes{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    bool                    pause_on_lost_focus{true};
    std::vector<KeyBinding> keys{default_key_bindings()};

    /// Read the keys this client knows. A value that does not parse keeps the
    /// default and is named in `problems`, never silently turned into zero.
    [[nodiscard]] static GameOptions from(const OptionsFile& file,
                                          std::vector<std::string>* problems = nullptr);

    /// Write them back, each in vanilla's own format. Other lines untouched.
    void store(OptionsFile& file) const;

    [[nodiscard]] const KeyBinding* binding(std::string_view name) const noexcept;
    [[nodiscard]] KeyBinding*       binding(std::string_view name) noexcept;
};

/// The effective GUI scale for a framebuffer: the setting, capped by what the
/// window leaves room for — vanilla's rule, the same as auto_gui_scale's.
[[nodiscard]] u32 effective_gui_scale(const GameOptions& options, u32 width, u32 height) noexcept;

/// Vanilla's mouse-look rate in degrees per pixel of motion: the sensitivity
/// `s` becomes `f = 0.6·s + 0.2`, and the turn is `f³ · 8 · 0.15` degrees a
/// pixel. At the default 0.5 that is 0.15 degrees a pixel — the rate this
/// client's camera always turned at.
[[nodiscard]] f32 degrees_per_pixel(f64 sensitivity) noexcept;

}  // namespace ov::client
