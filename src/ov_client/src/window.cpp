#define OV_LOG_CATEGORY "client"

#include "ov/client/window.hpp"

#include "ov/base/log.hpp"
#include "ov/client/options_file.hpp"  // ── screens ── key names and codes

#include <GLFW/glfw3.h>

#include <array>
#include <string>
#include <vector>

namespace ov::client {

namespace {

/// Default bindings. A table rather than a switch so that rebinding is a data
/// change, which is what the roadmap's key-bindings item will need.
struct Binding {
    Key key;
    int glfw_code;
};

constexpr std::array<Binding, 15> kBindings{{
    {Key::Forward, GLFW_KEY_W},
    {Key::Back, GLFW_KEY_S},
    {Key::Left, GLFW_KEY_A},
    {Key::Right, GLFW_KEY_D},
    {Key::Up, GLFW_KEY_SPACE},
    {Key::Down, GLFW_KEY_LEFT_SHIFT},
    {Key::Sprint, GLFW_KEY_LEFT_CONTROL},
    {Key::Escape, GLFW_KEY_ESCAPE},
    {Key::Reload, GLFW_KEY_F3},
    {Key::Inventory, GLFW_KEY_E},
    {Key::Drop, GLFW_KEY_Q},
    {Key::Backspace, GLFW_KEY_BACKSPACE},
    {Key::SaveToolbar, GLFW_KEY_C},
    {Key::LoadToolbar, GLFW_KEY_X},
    {Key::Chat, GLFW_KEY_T},
}};

/// A key's label as the layout prints it, upper-cased, or `fallback`.
[[nodiscard]] std::string label_of(int glfw_key, std::string_view fallback) {
    const char* name = glfwGetKeyName(glfw_key, 0);
    if (name == nullptr || name[0] == '\0') {
        return std::string(fallback);
    }
    std::string out(name);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    // Non-ASCII labels (É on AZERTY) come as UTF-8 lower case; vanilla
    // upper-cases them with the root locale. The two a French layout has on
    // its number row are folded here; anything else is shown as GLFW gives it.
    for (const auto& [lower, upper] : {std::pair<std::string_view, std::string_view>{"\xC3\xA9", "\xC3\x89"},
                                       {"\xC3\xA8", "\xC3\x88"}, {"\xC3\xA7", "\xC3\x87"},
                                       {"\xC3\xA0", "\xC3\x80"}}) {
        for (usize at = out.find(lower); at != std::string::npos; at = out.find(lower, at)) {
            out.replace(at, lower.size(), upper);
        }
    }
    return out;
}

/// The number row, in hotbar order.
constexpr std::array<int, 9> kHotbarKeys{GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3,
                                         GLFW_KEY_4, GLFW_KEY_5, GLFW_KEY_6,
                                         GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9};

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
    bool                                             previous_attack{false};
    bool                                             previous_use{false};
    bool                                             previous_middle{false};
    std::array<bool, 9>                              previous_hotbar{};
    /// Accumulated by the scroll callback and drained by poll(). A wheel notch
    /// arrives as an event, not as a state, so polling it would lose it.
    f64 scroll{0.0};
    /// The same for typed characters, which are events for the same reason.
    std::string typed;
    // ── chat ──
    /// Key presses and repeats since the last poll, drained into the input.
    std::vector<KeyEvent> key_events;
    // ── end chat ──
    // ── screens ──
    /// The bound GLFW code of each Key, from kBindings until rebound.
    std::array<int, static_cast<usize>(Key::Count)> codes{};
    std::vector<i32>                                codes_pressed;
    // ── end screens ──
};

// ── chat ──
/// GLFW's key callback, for the keys a text field reads. A callback and not
/// a poll because a text field needs the *repeats* — holding Backspace — and
/// polling sees a held key, not how many times the system repeated it.
void key_callback(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    auto* impl = static_cast<Window::Impl*>(glfwGetWindowUserPointer(window));
    if (impl == nullptr || (action != GLFW_PRESS && action != GLFW_REPEAT)) {
        return;
    }
    if (action == GLFW_PRESS && key != GLFW_KEY_UNKNOWN) {  // ── screens ──
        impl->codes_pressed.push_back(key);
    }
    EditKey edit{};
    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: edit = EditKey::Enter; break;
        case GLFW_KEY_ESCAPE: edit = EditKey::Escape; break;
        case GLFW_KEY_TAB: edit = EditKey::Tab; break;
        case GLFW_KEY_BACKSPACE: edit = EditKey::Backspace; break;
        case GLFW_KEY_DELETE: edit = EditKey::Delete; break;
        case GLFW_KEY_LEFT: edit = EditKey::Left; break;
        case GLFW_KEY_RIGHT: edit = EditKey::Right; break;
        case GLFW_KEY_UP: edit = EditKey::Up; break;
        case GLFW_KEY_DOWN: edit = EditKey::Down; break;
        case GLFW_KEY_HOME: edit = EditKey::Home; break;
        case GLFW_KEY_END: edit = EditKey::End; break;
        case GLFW_KEY_PAGE_UP: edit = EditKey::PageUp; break;
        case GLFW_KEY_PAGE_DOWN: edit = EditKey::PageDown; break;
        case GLFW_KEY_A: edit = EditKey::A; break;
        case GLFW_KEY_C: edit = EditKey::C; break;
        case GLFW_KEY_V: edit = EditKey::V; break;
        case GLFW_KEY_X: edit = EditKey::X; break;
        case GLFW_KEY_SLASH: edit = EditKey::Slash; break;
        default: return;
    }
#if defined(__APPLE__)
    const bool control = (mods & GLFW_MOD_SUPER) != 0;
#else
    const bool control = (mods & GLFW_MOD_CONTROL) != 0;
#endif
    impl->key_events.push_back(KeyEvent{edit, action == GLFW_REPEAT, (mods & GLFW_MOD_SHIFT) != 0,
                                        control, (mods & GLFW_MOD_ALT) != 0});
}
// ── end chat ──

void scroll_callback(GLFWwindow* window, double /*x*/, double y) {
    auto* impl = static_cast<Window::Impl*>(glfwGetWindowUserPointer(window));
    if (impl != nullptr) {
        impl->scroll += y;
    }
}

/// GLFW hands over a Unicode codepoint, already through the layout and any
/// dead keys. Encoding it as UTF-8 here rather than passing the number on is
/// what lets a search field hold a string the font can measure directly.
void character_callback(GLFWwindow* window, unsigned int codepoint) {
    auto* impl = static_cast<Window::Impl*>(glfwGetWindowUserPointer(window));
    if (impl == nullptr) {
        return;
    }
    std::string& out = impl->typed;
    if (codepoint < 0x80U) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

Window::Window() : impl_(std::make_unique<Impl>()) {
    for (const auto& binding : kBindings) {  // ── screens ──
        impl_->codes[static_cast<usize>(binding.key)] = binding.glfw_code;
    }
}

// ── screens ──
void Window::bind(Key key, i32 glfw_code) {
    if (glfw_code >= kMouseCodeBase) {
        OV_LOG_WARN("a mouse button cannot be bound to a key in this client; {} is kept",
                    key_name(impl_->codes[static_cast<usize>(key)]));
        return;
    }
    impl_->codes[static_cast<usize>(key)] = glfw_code;
    // The edge detector must not see the new key as newly pressed because the
    // old one was up.
    impl_->previous[static_cast<usize>(key)] = true;
}

i32 Window::binding(Key key) const noexcept {
    return impl_->codes[static_cast<usize>(key)];
}

std::string Window::code_label(i32 glfw_code) const {
    if (glfw_code < 0 || glfw_code >= kMouseCodeBase) {
        return {};
    }
    return label_of(glfw_code, "");
}
// ── end screens ──

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
    glfwSetScrollCallback(self->impl_->window, scroll_callback);
    glfwSetCharCallback(self->impl_->window, character_callback);
    glfwSetKeyCallback(self->impl_->window, key_callback);  // ── chat ──
    return self;
}

// ── chat ──
std::string Window::clipboard() const {
    const char* text = glfwGetClipboardString(impl_->window);
    return text == nullptr ? std::string{} : std::string(text);
}

void Window::set_clipboard(std::string_view utf8) {
    const std::string text(utf8);
    glfwSetClipboardString(impl_->window, text.c_str());
}
// ── end chat ──

void* Window::native_handle() const noexcept {
    return impl_->window;
}

std::string Window::key_label(Key key) const {
    for (const auto& binding : kBindings) {
        if (binding.key == key) {
            const char fallback[2] = {binding.glfw_code >= 32 && binding.glfw_code < 127
                                          ? static_cast<char>(binding.glfw_code)
                                          : '?',
                                      '\0'};
            return label_of(binding.glfw_code, fallback);
        }
    }
    return "?";
}

std::string Window::hotbar_key_label(i32 index) const {
    if (index < 0 || index >= static_cast<i32>(kHotbarKeys.size())) {
        return "?";
    }
    const char fallback[2] = {static_cast<char>('1' + index), '\0'};
    return label_of(kHotbarKeys[static_cast<usize>(index)], fallback);
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
        const int  code        = impl_->codes[index];  // ── screens ── rebindable
        const bool down = code >= 0 && glfwGetKey(impl_->window, code) == GLFW_PRESS;
        input.keys[index]      = down;
        input.pressed[index]   = down && !impl_->previous[index];
        impl_->previous[index] = down;
    }

    // Polled rather than taken from a callback, like the keys: one place that
    // reads the whole input state, and no edge that can arrive between frames
    // and be lost.
    input.typed = std::move(impl_->typed);
    impl_->typed.clear();
    // ── chat ──  (swapped, so both vectors keep their capacity)
    input.key_events.clear();
    input.key_events.swap(impl_->key_events);
    // ── end chat ──

    input.hotbar_pressed = -1;
    for (usize i = 0; i < kHotbarKeys.size(); ++i) {
        const bool down = glfwGetKey(impl_->window, kHotbarKeys[i]) == GLFW_PRESS;
        if (down && !impl_->previous_hotbar[i]) {
            input.hotbar_pressed = static_cast<i32>(i);
        }
        impl_->previous_hotbar[i] = down;
    }
    input.shift_held =
        glfwGetKey(impl_->window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
        glfwGetKey(impl_->window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
#if defined(__APPLE__)
    input.control_held = glfwGetKey(impl_->window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
                         glfwGetKey(impl_->window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
#else
    input.control_held = glfwGetKey(impl_->window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                         glfwGetKey(impl_->window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
#endif

    const bool attack     = glfwGetMouseButton(impl_->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool use        = glfwGetMouseButton(impl_->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    input.attack_held     = attack;
    input.use_held        = use;
    input.attack_pressed  = attack && !impl_->previous_attack;
    input.use_pressed     = use && !impl_->previous_use;
    // ── screens ──
    input.attack_released = !attack && impl_->previous_attack;
    input.codes_pressed.clear();
    input.codes_pressed.swap(impl_->codes_pressed);
    if (input.attack_pressed) {
        input.codes_pressed.push_back(kMouseCodeBase + 0);
    }
    if (input.use_pressed) {
        input.codes_pressed.push_back(kMouseCodeBase + 1);
    }
    // ── end screens ──
    impl_->previous_attack = attack;
    impl_->previous_use    = use;

    const bool middle = glfwGetMouseButton(impl_->window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    input.middle_pressed    = middle && !impl_->previous_middle;
    impl_->previous_middle  = middle;

    input.scroll  = impl_->scroll;
    impl_->scroll = 0.0;

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
    // In *framebuffer* pixels, not window ones. They differ by the display
    // scale on a Retina screen, and hit-testing a slot in window pixels on a
    // 2x display misses every slot by half the window.
    int window_width  = 0;
    int window_height = 0;
    glfwGetWindowSize(impl_->window, &window_width, &window_height);
    const f64 scale_x = window_width > 0 ? static_cast<f64>(framebuffer_width()) /
                                               static_cast<f64>(window_width)
                                         : 1.0;
    const f64 scale_y = window_height > 0 ? static_cast<f64>(framebuffer_height()) /
                                                static_cast<f64>(window_height)
                                          : 1.0;
    input.mouse_x = x * scale_x;
    input.mouse_y = y * scale_y;
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
