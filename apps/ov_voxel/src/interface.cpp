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

/// Two clicks on one slot within this are one double-click. Vanilla's window is
/// 250 ms; this is the same, in the unit the frame loop counts in.
constexpr f32 kDoubleClickSeconds = 0.25F;

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

    // ── The creative catalogue ──────────────────────────────────────────────
    //
    // Refused and named when it is missing rather than replaced by a guess.
    // The order of the tabs is Mojang's and there is no way to state it here:
    // see docs/provenance/inventaire-creatif.md.
    auto tabs = render::CreativeTabs::load(options.creative_tabs);
    if (tabs) {
        usize cells = 0;
        for (const render::CreativeTab& tab : tabs->tabs()) {
            cells += tab.stacks.size();
        }
        OV_LOG_INFO("interface: creative catalogue {} — {} tabs, {} cells",
                    options.creative_tabs, tabs->tabs().size(), cells);
        self->creative_tabs_ = std::move(*tabs);

        for (const std::string_view location :
             {"minecraft:gui/container/creative_inventory/tabs",
              "minecraft:gui/container/creative_inventory/tab_items",
              "minecraft:gui/container/creative_inventory/tab_item_search",
              "minecraft:gui/container/creative_inventory/tab_inventory"}) {
            auto handle = sheet(location);
            if (!handle) {
                OV_LOG_WARN("{}", handle.error());
                continue;
            }
            if (location.ends_with("/tabs")) {
                self->creative_sheet_ = *handle;
            } else {
                self->backgrounds_.emplace_back(std::string(location), *handle);
            }
        }
        if (self->creative_sheet_ != client::GuiTexture::Invalid) {
            self->creative_screen_.emplace(*self->creative_tabs_, self->language_);
        }
    } else {
        OV_LOG_WARN("creative catalogue {}: {}. The creative inventory is refused; "
                    "generate it with scripts/measure_creative_tabs.py.",
                    options.creative_tabs, render::to_string(tabs.error()));
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

// ── The creative inventory ──────────────────────────────────────────────────

void Interface::toggle_creative(client::Window& window, netclient::Client& client) {
    if (creative_visible_) {
        creative_visible_ = false;
        creative_scrolling_ = false;
        window.set_cursor_captured(true);
        return;
    }
    if (!creative_screen_) {
        OV_LOG_WARN("no creative catalogue loaded; the screen is refused");
        return;
    }
    if (!hud_.creative) {
        // Not a cosmetic guard. A survival server *ignores* Set Creative Slot
        // — vanilla's own behaviour, measured — so the screen would be a
        // catalogue that hands out nothing and looks broken.
        OV_LOG_INFO("the creative inventory needs creative mode; the server is in survival");
        return;
    }
    if (screen_) {
        close(window, client);
    }
    creative_visible_ = true;
    window.set_cursor_captured(false);
}

bool Interface::select_creative_tab(std::string_view id) {
    return creative_screen_ && creative_screen_->select(id);
}

void Interface::creative_search(std::string_view text) {
    if (!creative_screen_) {
        return;
    }
    (void)creative_screen_->select("minecraft:search");
    creative_screen_->type(text);
}

bool Interface::creative_take(i32 cell) {
    if (!creative_screen_) {
        return false;
    }
    const render::CreativeStack* stack = creative_screen_->cell(cell);
    if (stack == nullptr) {
        return false;
    }
    if (registries_ == nullptr || !item_registry_) {
        OV_LOG_WARN("no item registry; {} cannot be named to the server", stack->item);
        return false;
    }
    const auto id = registries_->protocol_id(*item_registry_, stack->item);
    if (!id) {
        // Refused and named. An item the registry does not know has no wire id
        // and sending zero would hand the player air.
        OV_LOG_WARN("creative: {} is not in minecraft:item; refused", stack->item);
        return false;
    }
    // A click takes a *full* stack, which is the item's own limit and not
    // always 64: a bucket is 16 and a sword is 1.
    carried_.item_id = static_cast<i32>(*id);
    carried_.count   = registries_->max_stack_size(*id);
    carried_.nbt.assign(stack->nbt.begin(), stack->nbt.end());
    return true;
}

void Interface::creative_put(netclient::Client& client, i16 slot) {
    if (slot < 0 || static_cast<usize>(slot) >= inventory_.size()) {
        return;
    }
    client.send_creative_slot(slot, carried_.item_id, carried_.count, carried_.nbt);
    inventory_[static_cast<usize>(slot)] = carried_;
    refresh_hotbar();
    carried_ = net::ItemStack{};
}

std::string Interface::describe_creative() const {
    if (!creative_screen_) {
        return "no creative catalogue";
    }
    const client::CreativeScreen& screen = *creative_screen_;
    std::string out = fmt::format("creative tab {} \"{}\" ({} cells, row {}/{})", screen.tab().id,
                                  language_.translate(screen.tab().translation_key),
                                  screen.page().size(), screen.scroll_row(),
                                  screen.scroll_range());
    for (i32 i = 0; i < client::creative_layout::kPageCells; ++i) {
        const render::CreativeStack* stack = screen.cell(i);
        if (stack != nullptr) {
            out += fmt::format("\n  cell {:2}  {}", i, stack->item);
        }
    }
    return out;
}

u8 Interface::window_id() const noexcept {
    return screen_ && !own_inventory_ ? screen_->window_id() : u8{0};
}

void Interface::drag_over(netclient::Client& client, std::span<const i16> slots, bool right) {
    if (slots.empty()) {
        return;
    }
    // The three phases, and the button values that name them: 0/1/2 for a left
    // drag, 4/5/6 for a right one, 8/9/10 for the middle. Start and end carry
    // slot −999 — they are not clicks on anything — and only the middle phase
    // names slots.
    const i8 base = right ? i8{4} : i8{0};
    client.send_container_click(window_id(), state_id_, -999, base,
                                client::click_mode::kQuickCraft, carried_);
    for (const i16 slot : slots) {
        client.send_container_click(window_id(), state_id_, slot, static_cast<i8>(base + 1),
                                    client::click_mode::kQuickCraft, carried_);
    }
    client.send_container_click(window_id(), state_id_, -999, static_cast<i8>(base + 2),
                                client::click_mode::kQuickCraft, carried_);
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
    clock_ += delta;
    hud_.damage_flash    = std::max(0.0F, hud_.damage_flash - delta);
    hud_.name_flash_life = std::max(0.0F, hud_.name_flash_life - delta / kNameFlashSeconds);

    const f32 scale = static_cast<f32>(gui_->scale());
    mouse_x_        = static_cast<f32>(input.mouse_x) / scale;
    mouse_y_        = static_cast<f32>(input.mouse_y) / scale;

    if (input.just_pressed(client::Key::Inventory)) {
        // E opens the creative inventory in creative and the player's own
        // inventory otherwise, which is what vanilla binds it to. The creative
        // screen already refuses itself in survival, so this is one branch and
        // not two rules that can disagree.
        if (hud_.creative && creative_screen_ && !screen_) {
            toggle_creative(window, client);
        } else {
            toggle_inventory(window, client);
        }
        return true;
    }

    if (creative_visible_) {
        return update_creative(input, client, window);
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
    // ── The drag, in three phases ───────────────────────────────────────────
    const bool button_held = input.attack_held || input.use_held;

    if (dragging_) {
        if (hovered != nullptr &&
            std::ranges::find(drag_slots_, hovered->index) == drag_slots_.end()) {
            drag_slots_.push_back(hovered->index);
        }
        if (!button_held) {
            drag_over(client, drag_slots_, press_right_);
            dragging_ = false;
            drag_slots_.clear();
            press_pending_ = false;
        }
        return true;
    }

    if (press_pending_) {
        if (button_held) {
            // The pointer left the slot it was pressed on: this is a drag, not
            // a click.
            if (hovered != nullptr && hovered->index != press_slot_) {
                dragging_ = true;
                drag_slots_.clear();
                drag_slots_.push_back(press_slot_);
                drag_slots_.push_back(hovered->index);
            }
            return true;
        }
        // Released without moving: an ordinary click after all.
        click_slot(client, press_slot_, press_right_ ? 1 : 0, input.shift_held, -1);
        press_pending_ = false;
        return true;
    }

    if (input.attack_pressed || input.use_pressed || input.middle_pressed) {
        const i32 button = input.middle_pressed ? 2 : (input.use_pressed ? 1 : 0);
        if (hovered == nullptr) {
            if (!carried_.empty()) {
                // Outside every slot with a full cursor: vanilla throws it, as
                // a click on slot −999.
                client.send_container_click(window_id(), state_id_, -999,
                                            static_cast<i8>(button),
                                            client::click_mode::kPickup, carried_);
            }
            return true;
        }
        // Two left clicks on the same slot in a quarter of a second, with
        // something in hand: mode 6, which gathers every matching stack in the
        // window onto the cursor. It has to be checked before the drag, or the
        // second click of a double-click starts one.
        if (button == 0 && !carried_.empty() && hovered->index == last_click_slot_ &&
            clock_ - last_click_time_ < kDoubleClickSeconds) {
            client.send_container_click(window_id(), state_id_, hovered->index, 0,
                                        client::click_mode::kPickupAll, carried_);
            last_click_slot_ = -1;
            return true;
        }
        last_click_slot_ = hovered->index;
        last_click_time_ = clock_;

        // A press with something in hand waits to see whether the pointer
        // moves; a press with an empty hand is a pickup and acts at once.
        // Shift and the middle button are never drags.
        if (!carried_.empty() && button != 2 && !input.shift_held) {
            press_pending_ = true;
            press_right_   = button == 1;
            press_slot_    = hovered->index;
            return true;
        }
        click_slot(client, hovered->index, button, input.shift_held, -1);
        return true;
    }
    return true;
}

bool Interface::update_creative(const client::InputState& input, netclient::Client& client,
                                client::Window& window) {
    client::CreativeScreen& screen = *creative_screen_;

    if (input.just_pressed(client::Key::Escape)) {
        toggle_creative(window, client);
        return true;
    }
    if (!input.typed.empty()) {
        screen.type(input.typed);
    }
    if (input.just_pressed(client::Key::Backspace)) {
        screen.backspace();
    }
    if (input.scroll != 0.0) {
        screen.scroll_by(static_cast<f32>(input.scroll));
    }

    const client::CreativeTarget target =
        screen.hit_test(gui_->width(), gui_->height(), mouse_x_, mouse_y_);

    // The scrollbar drag holds across frames, so it is answered before the
    // press: a pointer that leaves the groove mid-drag still scrolls, which is
    // what every scrollbar in the game does.
    if (creative_scrolling_) {
        screen.drag_scroll(gui_->height(), mouse_y_);
        creative_scrolling_ = input.attack_held;
        return true;
    }

    if (!input.attack_pressed && !input.use_pressed) {
        return true;
    }

    switch (target.kind) {
        case client::CreativeHit::Tab:
            screen.select(static_cast<usize>(target.index));
            return true;

        case client::CreativeHit::Scrollbar:
            creative_scrolling_ = true;
            screen.drag_scroll(gui_->height(), mouse_y_);
            return true;

        case client::CreativeHit::Cell: {
            if (!creative_take(target.index)) {
                return true;
            }
            if (input.shift_held) {
                // Shift-click puts the stack straight into the inventory: the
                // first empty slot of the hotbar, then of the three rows. That
                // is where vanilla's own quick-move ends up, and it is done
                // here rather than asked of the server because Set Creative
                // Slot names a slot and has no "anywhere" form.
                for (usize slot = kHotbarFirst; slot < kOffHandSlot; ++slot) {
                    const usize index = slot < inventory_.size() ? slot : 0;
                    if (inventory_[index].empty()) {
                        creative_put(client, static_cast<i16>(index));
                        return true;
                    }
                }
                for (usize slot = 9; slot < kHotbarFirst; ++slot) {
                    if (inventory_[slot].empty()) {
                        creative_put(client, static_cast<i16>(slot));
                        return true;
                    }
                }
                // Full. Named rather than silently dropped: the stack stays on
                // the cursor, which is what vanilla does too.
                OV_LOG_INFO("creative: the inventory is full; the stack stays on the cursor");
            }
            return true;
        }

        case client::CreativeHit::PlayerSlot:
            if (carried_.empty()) {
                // Picking a slot's contents back up. Its own Set Creative Slot
                // with an empty stack, or the server keeps the copy.
                const auto index = static_cast<usize>(target.index);
                if (index < inventory_.size() && !inventory_[index].empty()) {
                    carried_ = inventory_[index];
                    client.send_creative_slot(static_cast<i16>(index), 0, 0, {});
                    inventory_[index] = net::ItemStack{};
                    refresh_hotbar();
                }
            } else {
                creative_put(client, static_cast<i16>(target.index));
            }
            return true;

        case client::CreativeHit::Destroy:
            carried_ = net::ItemStack{};
            return true;

        case client::CreativeHit::SearchField:
        case client::CreativeHit::None:
            break;
    }

    // Outside every cell with a full cursor: the stack is dropped, which in
    // creative means it simply stops existing. No packet: the server never had
    // it, because a cell of the catalogue is not a slot.
    if (target.kind == client::CreativeHit::None && !carried_.empty()) {
        carried_ = net::ItemStack{};
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

    if (creative_visible_ && creative_screen_) {
        client::draw_screen_dim(*gui_);
        build_views(inventory_);
        const client::CreativeTarget hovered =
            creative_screen_->hit_test(gui_->width(), gui_->height(), mouse_x_, mouse_y_);
        client::GuiTexture background = client::GuiTexture::Invalid;
        for (const auto& [location, handle] : backgrounds_) {
            if (location == creative_screen_->background_texture()) {
                background = handle;
            }
        }
        if (background != client::GuiTexture::Invalid) {
            creative_screen_->draw(*gui_, *items_, background, creative_sheet_, views_, hovered);
            const std::string_view name = creative_screen_->hovered_name(hovered, views_);
            if (!name.empty()) {
                (void)gui_->text(mouse_x_ + 8.0F, mouse_y_ - 12.0F, name);
            }
        }
        if (!carried_.empty()) {
            const client::ItemStackView held{item_name(carried_.item_id), carried_.count};
            items_->draw(*gui_, mouse_x_ - 8.0F, mouse_y_ - 8.0F, held);
            items_->draw_count(*gui_, mouse_x_ - 8.0F, mouse_y_ - 8.0F, held);
        }
        gui_->flush(cmd);
        return;
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
