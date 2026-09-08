#define OV_LOG_CATEGORY "client"

#include "ov/client/window.hpp"

#include "ov/base/log.hpp"

#include <GLFW/glfw3.h>

#include <array>
#include <string>

namespace ov::client {

namespace {

/// Default bindings. A table rather than a switch so that rebinding is a data
/// change, which is what the roadmap's key-bindings item will need.
struct Binding {
    Key key;
    int glfw_code;
};

constexpr std::array<Binding, 9> kBindings{{
    {Key::Forward, GLFW_KEY_W},
    {Key::Back, GLFW_KEY_S},
    {Key::Left, GLFW_KEY_A},
    {Key::Right, GLFW_KEY_D},
    {Key::Up, GLFW_KEY_SPACE},
    {Key::Down, GLFW_KEY_LEFT_SHIFT},
    {Key::Sprint, GLFW_KEY_LEFT_CONTROL},
    {Key::Escape, GLFW_KEY_ESCAPE},
    {Key::Reload, GLFW_KEY_F3},
}};

/// GLFW is a process-wide library with a global init count. It is initialised
/// once and terminated when the last window goes, which keeps the "no mutable
/// global singleton" rule honest — nothing reads this but the window itself.
int g_window_count = 0;

}  // namespace

std::string_view to_string(WindowError error) noexcept {
    switch (error) {
        case WindowError::NoWindowSystem: return "no window system available";
        case WindowError::CreationFailed: return "window creation failed";
    }
    return "unknown window error";
}

struct Window::Impl {
    GLFWwindow* window{nullptr};
    InputState  input;
    f64         last_mouse_x{0.0};
    f64         last_mouse_y{0.0};
    bool        first_mouse{true};
    bool        captured{false};
    /// Key state from the previous poll, for the edge detection.
    std::array<bool, static_cast<usize>(Key::Count)> previous{};
};

Window::Window() : impl_(std::make_unique<Impl>()) {}

Window::~Window() {
    if (impl_->window != nullptr) {
        glfwDestroyWindow(impl_->window);
        if (--g_window_count == 0) {
            glfwTerminate();
        }
    }
}

std::expected<std::unique_ptr<Window>, WindowError> Window::create(u32 width, u32 height,
                                                                   std::string_view title) {
    if (g_window_count == 0 && glfwInit() != GLFW_TRUE) {
        OV_LOG_ERROR("glfwInit failed — no window system?");
        return std::unexpected(WindowError::NoWindowSystem);
    }

    // GLFW defaults to an OpenGL context. Saying so explicitly is what stops it
    // creating one we would then have to ignore.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    std::unique_ptr<Window> self(new Window());
    const std::string       window_title(title);
    self->impl_->window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                                           window_title.c_str(), nullptr, nullptr);
    if (self->impl_->window == nullptr) {
        if (g_window_count == 0) {
            glfwTerminate();
        }
        return std::unexpected(WindowError::CreationFailed);
    }
    ++g_window_count;

    glfwSetWindowUserPointer(self->impl_->window, self->impl_.get());
    return self;
}

void* Window::native_handle() const noexcept {
    return impl_->window;
}

bool Window::should_close() const {
    return glfwWindowShouldClose(impl_->window) == GLFW_TRUE;
}

void Window::request_close() {
    glfwSetWindowShouldClose(impl_->window, GLFW_TRUE);
}

const InputState& Window::poll() {
    glfwPollEvents();

    InputState& input = impl_->input;

    for (const auto& binding : kBindings) {
        const auto index       = static_cast<usize>(binding.key);
        const bool down        = glfwGetKey(impl_->window, binding.glfw_code) == GLFW_PRESS;
        input.keys[index]      = down;
        input.pressed[index]   = down && !impl_->previous[index];
        impl_->previous[index] = down;
    }

    f64 x = 0.0;
    f64 y = 0.0;
    glfwGetCursorPos(impl_->window, &x, &y);
    if (impl_->first_mouse) {
        impl_->last_mouse_x = x;
        impl_->last_mouse_y = y;
        impl_->first_mouse  = false;
    }
    // Only while captured. Otherwise the first click after releasing the cursor
    // teleports the view by however far the pointer travelled across the desk.
    input.mouse_delta_x = impl_->captured ? x - impl_->last_mouse_x : 0.0;
    input.mouse_delta_y = impl_->captured ? y - impl_->last_mouse_y : 0.0;
    impl_->last_mouse_x = x;
    impl_->last_mouse_y = y;

    return input;
}

u32 Window::framebuffer_width() const {
    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(impl_->window, &width, &height);
    return static_cast<u32>(width);
}

u32 Window::framebuffer_height() const {
    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(impl_->window, &width, &height);
    return static_cast<u32>(height);
}

void Window::set_cursor_captured(bool captured) {
    impl_->captured = captured;
    glfwSetInputMode(impl_->window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Re-anchor, so the frame after capturing does not see the whole distance
    // between where the pointer was and where the window put it.
    impl_->first_mouse = true;
}

bool Window::cursor_captured() const noexcept {
    return impl_->captured;
}

bool Window::minimised() const {
    return framebuffer_width() == 0 || framebuffer_height() == 0;
}

}  // namespace ov::client
