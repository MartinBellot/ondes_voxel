// The window and the keyboard, and nothing else.
//
// ov_client is the only module that knows a window exists. ov_render is
// forbidden from knowing (a renderer that reads the keyboard cannot be tested
// headless), and ov_rhi only ever receives the window as an opaque pointer.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

enum class WindowError {
    /// GLFW would not start. On Linux this is usually a missing display.
    NoWindowSystem,
    /// The window itself could not be created.
    CreationFailed,
};

[[nodiscard]] std::string_view to_string(WindowError error) noexcept;

/// The keys the client cares about, named rather than numbered so that a
/// binding table can be written without including GLFW anywhere else.
enum class Key : u8 {
    Forward,
    Back,
    Left,
    Right,
    Up,
    Down,
    Sprint,
    Escape,
    Reload,
    /// E. Opens and closes the player's own inventory.
    Inventory,
    /// Q. Throws what is held.
    Drop,
    /// Backspace. Only a text field reads it.
    Backspace,
    /// C: with a number key, saves the hotbar into that saved-hotbar row.
    SaveToolbar,
    /// X: with a number key, loads that saved-hotbar row into the hotbar.
    LoadToolbar,
    /// T: opens the chat — and, on a creative category page, jumps to the
    /// search tab, which is the one thing it does in this client so far.
    Chat,
    /// ── hud ── Tab: the player list, while held.
    PlayerList,
    Count,
};

// ── chat ──
/// The keys a text field and the chat screen give a meaning to, named so no
/// header outside window.cpp has to see GLFW.
enum class EditKey : u8 {
    Enter,
    Escape,
    Tab,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    /// With control: select all, copy, cut, paste.
    A,
    C,
    V,
    X,
    /// The key that opens the command line. GLFW_KEY_SLASH, the key vanilla
    /// binds (`key.keyboard.slash`) — a key *position*, as in vanilla.
    Slash,
};

/// One press or auto-repeat of an EditKey, with the modifiers held at that
/// moment. `control` is vanilla's control: Command on macOS, Control elsewhere.
struct KeyEvent {
    EditKey key{EditKey::Enter};
    bool    repeat{false};
    bool    shift{false};
    bool    control{false};
    bool    alt{false};
};
// ── end chat ──

/// Mouse movement and key state for one frame.
struct InputState {
    f64 mouse_delta_x{0.0};
    f64 mouse_delta_y{0.0};
    /// Held this frame.
    bool keys[static_cast<usize>(Key::Count)]{};
    /// Went down between the previous poll and this one.
    bool pressed[static_cast<usize>(Key::Count)]{};

    /// Left and right mouse, held and newly pressed. Attack and use: the two
    /// verbs the game is played with, so they are not keys.
    bool attack_held{false};
    bool use_held{false};
    bool attack_pressed{false};
    bool use_pressed{false};
    /// Middle mouse: pick block in the world, clone a stack in a screen.
    bool middle_pressed{false};

    /// Where the pointer is, in framebuffer pixels from the top-left.
    ///
    /// Meaningless while the cursor is captured — GLFW keeps it in the middle
    /// and reports movement as a delta — which is exactly right: the pointer
    /// only matters when a screen is open, and a screen releases the cursor.
    f64 mouse_x{0.0};
    f64 mouse_y{0.0};

    /// Wheel movement since the last poll, in notches. Positive is away from
    /// the player, which vanilla maps to the *previous* hotbar slot.
    f64 scroll{0.0};

    /// 0..8 when a number key went down this poll, −1 otherwise. One value
    /// rather than nine booleans: two number keys in one frame is not a thing
    /// the game has a meaning for, and the last one wins.
    i32 hotbar_pressed{-1};

    /// The characters typed since the last poll, as UTF-8.
    ///
    /// GLFW's character callback rather than its key callback: a key is a
    /// physical position and a character is what the layout made of it, and
    /// only the second is what a search field wants. Cleared every poll, so a
    /// frame that drops a keystroke loses it — which is the right trade for a
    /// field nothing is typed into at speed.
    std::string typed;

    /// Either shift, held. Not a Key: it is a modifier on other input rather
    /// than an action, and Key::Down is already bound to left shift for
    /// sneaking.
    bool shift_held{false};

    /// Vanilla's "control" modifier, the one Ctrl+Q throws a whole stack
    /// with: either Control key — and on macOS either *Command* key, which is
    /// what the game reads there instead.
    bool control_held{false};

    // ── chat ──
    /// Every key that went down *or repeated* since the last poll, in order.
    /// A text field reads these rather than `pressed`: holding Backspace must
    /// delete a character per repeat, and two keys in one frame must not
    /// collapse into one. See KeyEvent.
    std::vector<KeyEvent> key_events;
    // ── end chat ──

    // ── screens ──
    /// Every key that went down this poll as its GLFW code, and every mouse
    /// button as kMouseCodeBase + button (options_file.hpp), in order. What
    /// the key binds screen listens to while it waits for "> ??? <".
    std::vector<i32> codes_pressed;
    /// The left button went up this poll: a slider's drag ends here.
    bool attack_released{false};
    // ── end screens ──

    [[nodiscard]] bool held(Key key) const noexcept { return keys[static_cast<usize>(key)]; }

    [[nodiscard]] bool just_pressed(Key key) const noexcept {
        return pressed[static_cast<usize>(key)];
    }
};

class Window {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Window>, WindowError> create(
        u32 width, u32 height, std::string_view title);

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    /// The GLFWwindow*, for DeviceDesc::native_window. Opaque on purpose: this
    /// is the only value that crosses from windowing into the RHI.
    [[nodiscard]] void* native_handle() const noexcept;

    [[nodiscard]] bool should_close() const;
    void               request_close();

    /// Pump events and return this frame's input.
    [[nodiscard]] const InputState& poll();

    [[nodiscard]] u32 framebuffer_width() const;
    [[nodiscard]] u32 framebuffer_height() const;

    /// Capture the pointer for mouse-look, or release it so the window can be
    /// closed. Escape releases; clicking captures again.
    void               set_cursor_captured(bool captured);
    [[nodiscard]] bool cursor_captured() const noexcept;

    /// True while the window has no area — minimised, or being dragged between
    /// displays. Rendering has to be skipped rather than producing a zero-sized
    /// swapchain.
    [[nodiscard]] bool minimised() const;

    /// What the keyboard layout prints on a key, as vanilla shows it in a
    /// tooltip: GLFW's key name, so the "1" of the hotbar is "&" on a French
    /// AZERTY keyboard — which is exactly what the real 1.20.1 client showed
    /// on this machine. Upper-cased like vanilla; the US label when the layout
    /// has none.
    [[nodiscard]] std::string key_label(Key key) const;

    /// The same for the number key of hotbar slot `index` (0..8).
    [[nodiscard]] std::string hotbar_key_label(i32 index) const;

    // ── chat ──
    /// The system clipboard, as UTF-8. Empty when it holds no text.
    [[nodiscard]] std::string clipboard() const;
    void                      set_clipboard(std::string_view utf8);
    // ── end chat ──

    // ── screens ──
    /// Rebind a key to a GLFW key code, as the key binds screen and
    /// options.txt say. A mouse code (≥ kMouseCodeBase) is not a key here:
    /// attack and use stay on their buttons, and such a binding is refused.
    void               bind(Key key, i32 glfw_code);
    [[nodiscard]] i32  binding(Key key) const noexcept;
    /// What the layout prints on a GLFW key code — `key_label` for any code.
    /// Empty when GLFW has no printable name for it (Space, F5, Shift…).
    [[nodiscard]] std::string code_label(i32 glfw_code) const;
    // ── end screens ──

    /// Forward-declared here and defined in the .cpp.
    ///
    /// Public only because GLFW's callbacks are C function pointers: a
    /// captureless free function has to name this type to reach the window's
    /// state, and a free function cannot be a friend of a private nested type.
    /// Nothing outside window.cpp can do anything with an incomplete type.
    struct Impl;

private:
    Window();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::client
