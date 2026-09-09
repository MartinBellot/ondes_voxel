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
    Count,
};

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

    /// Either shift, held. Not a Key: it is a modifier on other input rather
    /// than an action, and Key::Down is already bound to left shift for
    /// sneaking.
    bool shift_held{false};

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
