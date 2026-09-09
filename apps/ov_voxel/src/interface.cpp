#define OV_LOG_CATEGORY "voxel"

#include "interface.hpp"

#include "ov/base/log.hpp"
#include "ov/render/font.hpp"
#include "ov/render/texture_image.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::demo {

namespace {

/// Window 0 always has exactly this many slots: 1 crafting result, 4 grid,
/// 4 armour, 27 inventory, 9 hotbar, 1 off hand.
constexpr usize kPlayerSlots = 46;
constexpr usize kHotbarFirst = 36;
constexpr usize kArmourFirst = 5;
constexpr usize kOffHandSlot = 45;

/// How long the held item's name stays up, in seconds. Vanilla's forty ticks.
constexpr f32 kNameFlashSeconds = 2.0F;
/// How long the hearts blink after damage. Vanilla's ten ticks.
constexpr f32 kDamageFlashSeconds = 0.5F;

/// The plain text of a chat component, as far as the interface needs it.
///
/// Only `text`, and only at the top level. That is what the server sends for a
/// window title — `{"text":"Chest"}` — and a component with `translate` or
/// `extra` is left as its JSON rather than half-rendered, so that a title this
/// does not understand looks obviously wrong instead of subtly so.
[[nodiscard]] std::string component_text(std::string_view json) {
    constexpr std::string_view kKey = R"("text":")";
    const auto                 at   = json.find(kKey);
    if (at == std::string_view::npos) {
        return std::string(json);
    }
    std::string out;
    for (usize i = at + kKey.size(); i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            out.push_back(json[++i]);
            continue;
        }
        if (json[i] == '"') {
            break;
        }
        out.push_back(json[i]);
    }
    return out;
}

}  // namespace

Interface::~Interface() = default;

std::expected<std::unique_ptr<Interface>, std::string> Interface::create(
    rhi::Device& device, rhi::Format colour_format, const render::AssetSource& assets,
    const render::ItemModelCache& items, rhi::ImageHandle atlas, u32 atlas_width,
    u32 atlas_height, const registry::Registries* registries, u32 foliage_tint,
    const InterfaceOptions& options) {
    std::unique_ptr<Interface> self(new Interface);
    self->options_    = options;
    self->registries_ = registries;
    if (registries != nullptr) {
        self->item_registry_ = registries->find("minecraft:item");
    }

    auto gui = client::Gui::create(device, colour_format);
    if (!gui) {
        return std::unexpected(std::string(rhi::to_string(gui.error())));
    }
    self->gui_ = std::move(*gui);

    auto font = render::Font::load_default(assets);
    if (!font) {
        return std::unexpected(std::string(render::to_string(font.error())));
    }
    const usize glyphs = font->glyph_count();
    const usize pages  = font->pages().size();
    if (auto set = self->gui_->set_font(std::move(*font)); !set) {
        return std::unexpected(std::string(rhi::to_string(set.error())));
    }
    OV_LOG_INFO("interface: font with {} glyphs across {} page(s)", glyphs, pages);

    const auto sheet = [&](std::string_view location) -> std::expected<client::GuiTexture, std::string> {
        const auto parsed = ResourceLocation::parse(location);
        if (!parsed) {
            return std::unexpected(std::string("bad texture location: ") + std::string(location));
        }
        auto image = render::load_texture(assets, *parsed);
        if (!image) {
            return std::unexpected(std::string(location) + ": " +
                                   std::string(render::to_string(image.error())));
        }
        auto handle = self->gui_->add_texture(*image, location);
        if (!handle) {
            return std::unexpected(std::string(rhi::to_string(handle.error())));
        }
        return *handle;
    };

    auto widgets = sheet("minecraft:gui/widgets");
    if (!widgets) {
        return std::unexpected(widgets.error());
    }
    auto icons = sheet("minecraft:gui/icons");
    if (!icons) {
        return std::unexpected(icons.error());
    }
    self->textures_.widgets = *widgets;
    self->textures_.icons   = *icons;

    // The four backgrounds a window can have, uploaded up front. Four textures
    // of a quarter of a megabyte each is nothing next to the block atlas, and
    // uploading one in the middle of a frame is a stall.
    for (const std::string_view location :
         {"minecraft:gui/container/inventory", "minecraft:gui/container/generic_54",
          "minecraft:gui/container/crafting_table", "minecraft:gui/container/furnace"}) {
        auto handle = sheet(location);
        if (!handle) {
            OV_LOG_WARN("{}", handle.error());
            continue;
        }
        self->backgrounds_.emplace_back(std::string(location), *handle);
    }

    const client::GuiTexture atlas_texture =
        self->gui_->borrow_texture(atlas, atlas_width, atlas_height);
    self->items_.emplace(items, atlas_texture, foliage_tint);

    auto language = render::Language::load(assets, options.language);
    if (language) {
        OV_LOG_INFO("interface: language {} with {} entries", options.language, language->size());
        self->language_ = std::move(*language);
    } else {
        OV_LOG_WARN("language {}: {}; names will be shown as registry ids", options.language,
                    render::to_string(language.error()));
    }

    self->inventory_.assign(kPlayerSlots, net::ItemStack{});
    self->views_.reserve(128);
    return self;
}

std::string_view Interface::item_name(i32 item_id) const noexcept {
    if (registries_ == nullptr || !item_registry_ || item_id <= 0) {
        return {};
    }
    return registries_->entry_of(*item_registry_, static_cast<registry::ProtocolId>(item_id));
}

void Interface::build_views(const std::vector<net::ItemStack>& slots) {
    views_.clear();
    for (const net::ItemStack& stack : slots) {
        views_.push_back(client::ItemStackView{stack.empty() ? std::string_view{}
                                                             : item_name(stack.item_id),
                                               stack.count});
    }
}

void Interface::refresh_hotbar() {
    for (usize i = 0; i < hud_.hotbar.size(); ++i) {
        const net::ItemStack& stack = inventory_[kHotbarFirst + i];
        hud_.hotbar[i] = client::ItemStackView{
            stack.empty() ? std::string_view{} : item_name(stack.item_id), stack.count};
    }
    const net::ItemStack& off = inventory_[kOffHandSlot];
    hud_.off_hand                 = client::ItemStackView{
        off.empty() ? std::string_view{} : item_name(off.item_id), off.count};

    std::array<client::ItemStackView, 4> armour{};
    for (usize i = 0; i < armour.size(); ++i) {
        const net::ItemStack& piece = inventory_[kArmourFirst + i];
        armour[i]                       = client::ItemStackView{
            piece.empty() ? std::string_view{} : item_name(piece.item_id), piece.count};
    }
    hud_.armour = client::armour_points(armour);
}

void Interface::apply(const netclient::ClientEvents& events) {
    if (events.game_mode) {
        // 1 is creative. Creative hides the hearts, the haunches and the
        // experience bar, and it is the only thing that decides it.
        hud_.creative = *events.game_mode == 1;
    }
    if (events.health) {
        if (events.health->health < previous_health_) {
            hud_.damage_flash = kDamageFlashSeconds;
        }
        previous_health_ = events.health->health;
        hud_.health      = events.health->health;
        hud_.food        = events.health->food;
        hud_.saturation  = events.health->saturation;
    }
    if (events.experience) {
        hud_.experience_bar   = events.experience->bar;
        hud_.experience_level = events.experience->level;
    }

    // Open Screen first, and *before* the slot packets. A server sends the
    // window and its contents in one breath, and both arrive in the same poll:
    // applying the contents first means dropping every one of them, because the
    // window they name does not exist yet. That is exactly what happened, and
    // the symptom was a chest that opened empty and filled itself one click
    // later.
    if (events.open_screen) {
        auto screen = client::ContainerScreen::from_menu(
            events.open_screen->type, static_cast<u8>(events.open_screen->window_id),
            component_text(events.open_screen->title_json));
        if (screen) {
            screen_        = std::move(*screen);
            own_inventory_ = false;
            window_slots_.assign(screen_->slot_count(), net::ItemStack{});
            background_ = client::GuiTexture::Invalid;
            for (const auto& [location, handle] : backgrounds_) {
                if (location == screen_->background_texture()) {
                    background_ = handle;
                }
            }
            OV_LOG_INFO("interface: opened {} ({} slots, window {})", screen_->title(),
                        screen_->slot_count(), screen_->window_id());
        } else {
            // Refused and named by ContainerScreen::from_menu. The window is
            // left closed; the caller's next Close Container tells the server.
            screen_.reset();
        }
    }

    for (const net::ContainerContent& content : events.containers) {
        if (content.window_id == 0) {
            inventory_ = content.slots;
            inventory_.resize(kPlayerSlots);
            refresh_hotbar();
        } else if (screen_ && content.window_id == screen_->window_id()) {
            window_slots_ = content.slots;
            state_id_     = content.state_id;
            // The player's own section of an open window is a *view* of window
            // 0, and the server sends it here rather than twice. Copying it
            // back keeps the hotbar under the screen correct.
            const usize container = window_slots_.size() >= 36 ? window_slots_.size() - 36 : 0;
            for (usize i = 0; i + container < window_slots_.size() && i + 9 < kPlayerSlots; ++i) {
                inventory_[9 + i] = window_slots_[container + i];
            }
            refresh_hotbar();
        }
        carried_ = content.carried;
    }

    for (const net::ContainerSlotUpdate& update : events.container_slots) {
        state_id_ = update.state_id;
        if (update.window_id == -1 && update.slot == -1) {
            // The cursor. Not a window: reading the id as unsigned turns this
            // into window 255 and the stack on the cursor never appears.
            carried_ = update.stack;
            continue;
        }
        if (update.window_id == 0) {
            if (update.slot >= 0 && static_cast<usize>(update.slot) < inventory_.size()) {
                inventory_[static_cast<usize>(update.slot)] = update.stack;
                refresh_hotbar();
            }
            continue;
        }
        if (screen_ && update.window_id == static_cast<i8>(screen_->window_id())) {
            if (update.slot >= 0 && static_cast<usize>(update.slot) < window_slots_.size()) {
                window_slots_[static_cast<usize>(update.slot)] = update.stack;
            }
        }
    }

    if (events.close_window) {
        screen_.reset();
        window_slots_.clear();
    }
}

void Interface::toggle_inventory(client::Window& window, netclient::Client& client) {
    if (screen_) {
        close(window, client);
        return;
    }
    screen_        = client::ContainerScreen::player_inventory();
    own_inventory_ = true;
    background_    = client::GuiTexture::Invalid;
    for (const auto& [location, handle] : backgrounds_) {
        if (location == screen_->background_texture()) {
            background_ = handle;
        }
    }
    window.set_cursor_captured(false);
}

void Interface::close(client::Window& window, netclient::Client& client) {
    if (screen_ && !own_inventory_) {
        // Not optional. A server that still believes a container is open
        // refuses the next one, and the symptom is a chest that will not open
        // a second time.
        client.send_close_container(screen_->window_id());
    }
    screen_.reset();
    window_slots_.clear();
    window.set_cursor_captured(true);
}

void Interface::note_creative(i16 slot, i32 item_id, i8 count) {
    if (slot < 0 || static_cast<usize>(slot) >= inventory_.size()) {
        return;
    }
    inventory_[static_cast<usize>(slot)] = net::ItemStack{item_id, count, {}};
    refresh_hotbar();
}

u8 Interface::window_id() const noexcept {
    return screen_ && !own_inventory_ ? screen_->window_id() : u8{0};
}

void Interface::click_slot(netclient::Client& client, i16 slot, i32 button, bool shift,
                           i32 hotbar_key) {
    const client::ClickIntent intent =
        client::ContainerScreen::click(slot, button, shift, hotbar_key);
    client.send_container_click(window_id(), state_id_, intent.slot, intent.button, intent.mode,
                                carried_);
}

bool Interface::update(const client::InputState& input, netclient::Client& client,
                       client::Window& window, f64 delta_seconds) {
    const auto delta = static_cast<f32>(delta_seconds);
    hud_.damage_flash    = std::max(0.0F, hud_.damage_flash - delta);
    hud_.name_flash_life = std::max(0.0F, hud_.name_flash_life - delta / kNameFlashSeconds);

    const f32 scale = static_cast<f32>(gui_->scale());
    mouse_x_        = static_cast<f32>(input.mouse_x) / scale;
    mouse_y_        = static_cast<f32>(input.mouse_y) / scale;

    if (input.just_pressed(client::Key::Inventory)) {
        toggle_inventory(window, client);
        return true;
    }

    if (!screen_) {
        // The hotbar. Number keys and the wheel, and both tell the server:
        // which slot is held decides what a placement places.
        i32 selection = hud_.selected;
        if (input.hotbar_pressed >= 0) {
            selection = input.hotbar_pressed;
        } else if (input.scroll != 0.0) {
            // Away from the player is the *previous* slot, which is vanilla's
            // direction and the opposite of what the sign suggests.
            const auto steps = static_cast<i32>(std::lround(input.scroll));
            selection        = ((selection - steps) % 9 + 9) % 9;
        }
        if (selection != hud_.selected) {
            hud_.selected = selection;
            client.send_held_slot(static_cast<i16>(selection));
            const client::ItemStackView& held = hud_.hotbar[static_cast<usize>(selection)];
            hud_.name_flash =
                held.empty() ? std::string{} : std::string(language_.item_name(held.item));
            hud_.name_flash_life = held.empty() ? 0.0F : 1.0F;
        }
        return false;
    }

    // ── A screen is open ────────────────────────────────────────────────────
    if (input.just_pressed(client::Key::Escape)) {
        close(window, client);
        return true;
    }

    const client::SlotRect* hovered =
        screen_->slot_at(gui_->width(), gui_->height(), mouse_x_, mouse_y_);

    if (input.hotbar_pressed >= 0 && hovered != nullptr) {
        click_slot(client, hovered->index, 0, false, input.hotbar_pressed);
        return true;
    }
    if (input.just_pressed(client::Key::Drop) && hovered != nullptr) {
        client.send_container_click(window_id(), state_id_, hovered->index,
                                    input.shift_held ? 1 : 0, client::click_mode::kThrow,
                                    carried_);
        return true;
    }
    if (input.attack_pressed || input.use_pressed || input.middle_pressed) {
        const i32 button = input.middle_pressed ? 2 : (input.use_pressed ? 1 : 0);
        if (hovered != nullptr) {
            click_slot(client, hovered->index, button, input.shift_held, -1);
        } else if (!carried_.empty()) {
            // Outside every slot with a full cursor: vanilla throws it, as a
            // click on slot −999.
            client.send_container_click(window_id(), state_id_, -999, static_cast<i8>(button),
                                        client::click_mode::kPickup, carried_);
        }
        return true;
    }
    return true;
}

void Interface::draw(rhi::CommandList& cmd, u32 framebuffer_width, u32 framebuffer_height) {
    const u32 scale =
        options_.gui_scale != 0
            ? options_.gui_scale
            : client::auto_gui_scale(framebuffer_width, framebuffer_height, 0);
    gui_->begin(framebuffer_width, framebuffer_height, scale);

    if (options_.hud) {
        client::draw_hud(*gui_, *items_, textures_, hud_);
    }

    if (screen_ && background_ != client::GuiTexture::Invalid) {
        client::draw_screen_dim(*gui_);
        build_views(own_inventory_ ? inventory_ : window_slots_);
        const client::SlotRect* hovered =
            screen_->slot_at(gui_->width(), gui_->height(), mouse_x_, mouse_y_);
        screen_->draw(*gui_, *items_, background_, views_, hovered);

        // The cursor's stack, drawn last and centred on the pointer, exactly
        // eight pixels up and left of it — a cell is sixteen.
        if (!carried_.empty()) {
            const client::ItemStackView held{item_name(carried_.item_id), carried_.count};
            items_->draw(*gui_, mouse_x_ - 8.0F, mouse_y_ - 8.0F, held);
            items_->draw_count(*gui_, mouse_x_ - 8.0F, mouse_y_ - 8.0F, held);
        }
    }

    gui_->flush(cmd);
}

const client::GuiStats& Interface::stats() const noexcept {
    return gui_->stats();
}

std::string Interface::describe_window() const {
    if (!screen_) {
        return "no window open";
    }
    std::string out = fmt::format("window {} \"{}\" ({} slots, state {}):", screen_->window_id(),
                                  screen_->title(), screen_->slot_count(), state_id_);
    for (usize i = 0; i < window_slots_.size(); ++i) {
        if (!window_slots_[i].empty()) {
            out += fmt::format("\n  slot {:2}  {} x{}", i, item_name(window_slots_[i].item_id),
                               window_slots_[i].count);
        }
    }
    return out;
}

std::string Interface::describe_inventory() const {
    std::string out = "window 0:";
    for (usize i = 0; i < inventory_.size(); ++i) {
        if (!inventory_[i].empty()) {
            out += fmt::format("\n  slot {:2}  {} x{}", i, item_name(inventory_[i].item_id),
                               inventory_[i].count);
        }
    }
    return out;
}

}  // namespace ov::demo
