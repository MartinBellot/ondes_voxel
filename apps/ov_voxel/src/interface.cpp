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

/// The search field's cursor: on for 300 ms, off for 300 ms, from the moment
/// the screen opened — vanilla's edit box blink.
constexpr f32 kBlinkSeconds = 0.3F;

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

    // ── loading ──
    // The dirt behind every loading screen. Missing is not fatal: the screen
    // falls back to a plain dark fill and says the rest.
    if (auto dirt = sheet("minecraft:gui/options_background"); dirt) {
        self->loading_background_ = *dirt;
    } else {
        OV_LOG_WARN("{}", dirt.error());
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

    // ── The creative inventory ──────────────────────────────────────────────
    //
    // Refused and named when the catalogue is missing rather than replaced by
    // a guess — and named *on screen* as well as here, since the log is where
    // the original "there is no creative inventory" hid.
    auto tabs = render::CreativeTabs::load(options.creative_tabs);
    if (tabs) {
        self->creative_tabs_ = std::move(*tabs);

        auto described = render::CreativeItems::load(options.creative_items, self->language_);
        if (described) {
            self->creative_items_ = std::move(*described);
        } else {
            OV_LOG_WARN("creative items {}: {} — tooltips show names only, and nothing is "
                        "tinted. scripts/measure_creative_screen.py asks the vanilla client.",
                        options.creative_items, render::to_string(described.error()));
        }
        self->item_tags_ = render::load_item_tags(options.item_tags);
        if (self->item_tags_.empty()) {
            OV_LOG_WARN("no item tags under {}: a '#' search finds nothing", options.item_tags);
        }

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
            self->creative_screen_.emplace(
                *self->creative_tabs_, self->language_,
                self->creative_items_ ? &*self->creative_items_ : nullptr,
                client::CreativeScreenOptions{options.operator_tab});
            self->creative_screen_->set_item_tags(&self->item_tags_);
            usize cells = 0;
            for (const render::CreativeTab* tab : self->creative_screen_->tabs()) {
                cells += tab->stacks.size();
            }
            OV_LOG_INFO("interface: creative catalogue {} — {} tabs shown, {} cells, {} described",
                        options.creative_tabs, self->creative_screen_->tab_count(), cells,
                        self->creative_items_ ? self->creative_items_->size() : 0);
        }
    } else {
        OV_LOG_WARN("creative catalogue {}: {}. The creative inventory is refused; "
                    "generate it with scripts/measure_creative_tabs.py (or "
                    "scripts/setup_vanilla.sh).",
                    options.creative_tabs, render::to_string(tabs.error()));
    }

    auto hotbars = client::SavedHotbars::load(options.hotbar_file);
    if (hotbars) {
        self->saved_hotbars_ = std::move(*hotbars);
    } else {
        OV_LOG_WARN("{}: {}; saved hotbars start empty", options.hotbar_file,
                    client::to_string(hotbars.error()));
    }
    self->creative_textures_.tabs       = self->creative_sheet_;
    self->creative_textures_.slot_icons = options.slot_icons;

    // The gesture model's two rules that depend on data: the stack limit from
    // the registry, the armour slot from what the real client said.
    Interface* raw = self.get();
    self->creative_model_.set_rules(
        [raw](i32 item) -> i32 {
            if (raw->registries_ == nullptr || item <= 0) {
                return 64;
            }
            return raw->registries_->max_stack_size(static_cast<registry::ProtocolId>(item));
        },
        [raw](const client::SlotStack& stack) -> i32 {
            if (!raw->creative_items_) {
                // No data: the armour slots refuse nothing rather than
                // everything, and that is named in the provenance document.
                return -2;
            }
            const render::CreativeItemInfo* info = raw->creative_items_->first(raw->item_name(stack.item));
            return info != nullptr ? info->armour_slot : -1;
        });

    self->inventory_.assign(kPlayerSlots, net::ItemStack{});
    self->views_.reserve(128);
    self->tooltip_lines_.reserve(16);
    return self;
}

std::string_view Interface::item_name(i32 item_id) const noexcept {
    if (registries_ == nullptr || !item_registry_ || item_id <= 0) {
        return {};
    }
    return registries_->entry_of(*item_registry_, static_cast<registry::ProtocolId>(item_id));
}

client::ItemStackView Interface::view_of(const net::ItemStack& stack) const {
    if (stack.empty()) {
        return {};
    }
    client::ItemStackView view{item_name(stack.item_id), stack.count};
    if (creative_items_) {
        if (const render::CreativeItemInfo* info = creative_items_->first(view.item)) {
            view.tints      = info->tints;
            view.has_tints  = true;
            view.max_damage = info->max_damage;
            view.damage     = client::nbt_damage(stack.nbt);
        }
    }
    return view;
}

void Interface::build_views(const std::vector<net::ItemStack>& slots) {
    views_.clear();
    for (const net::ItemStack& stack : slots) {
        views_.push_back(view_of(stack));
    }
}

void Interface::refresh_hotbar() {
    for (usize i = 0; i < hud_.hotbar.size(); ++i) {
        hud_.hotbar[i] = view_of(inventory_[kHotbarFirst + i]);
    }
    hud_.off_hand = view_of(inventory_[kOffHandSlot]);

    std::array<client::ItemStackView, 4> armour{};
    for (usize i = 0; i < armour.size(); ++i) {
        armour[i] = view_of(inventory_[kArmourFirst + i]);
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

    // Open Screen first, and *before* the slot packets: both arrive in the
    // same poll, and contents applied before their window are dropped.
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

// ── The creative model ──────────────────────────────────────────────────────

client::SlotStack Interface::slot_of(const net::ItemStack& stack) const {
    if (stack.empty()) {
        return {};
    }
    return client::SlotStack{stack.item_id, stack.count, stack.nbt};
}

net::ItemStack Interface::stack_of(const client::SlotStack& stack) const {
    if (stack.empty()) {
        return {};
    }
    return net::ItemStack{stack.item, static_cast<i8>(std::clamp(stack.count, 1, 127)), stack.nbt};
}

client::SlotStack Interface::cell_stack(const client::CreativeCell& cell) const {
    if (cell.hint || registries_ == nullptr || !item_registry_) {
        return {};
    }
    const auto id = registries_->protocol_id(*item_registry_, cell.item);
    if (!id) {
        // Refused and named: an item the registry does not know has no wire
        // id, and sending zero would hand the player air.
        OV_LOG_WARN("creative: {} is not in minecraft:item; refused", cell.item);
        return {};
    }
    return client::SlotStack{static_cast<i32>(*id), cell.count, {cell.nbt.begin(), cell.nbt.end()}};
}

void Interface::load_model() {
    for (usize i = 0; i < kPlayerSlots; ++i) {
        creative_model_.slots()[i] = slot_of(inventory_[i]);
    }
    creative_model_.carried() = slot_of(carried_);
    effects_.clear();
}

void Interface::store_model(netclient::Client& client) {
    for (const auto& [slot, stack] : effects_.slots) {
        const net::ItemStack wire = stack_of(stack);
        client.send_creative_slot(slot, wire.item_id, wire.count, wire.nbt);
    }
    // A throw is the same packet with slot −1: vanilla's own creative drop.
    for (const client::SlotStack& drop : effects_.drops) {
        const net::ItemStack wire = stack_of(drop);
        client.send_creative_slot(-1, wire.item_id, wire.count, wire.nbt);
    }
    for (usize i = 0; i < kPlayerSlots; ++i) {
        inventory_[i] = stack_of(creative_model_.slots()[i]);
    }
    carried_ = stack_of(creative_model_.carried());
    refresh_hotbar();
    effects_.clear();
}

void Interface::save_hotbar(usize row) {
    client::SavedHotbars::Row saved{};
    for (usize column = 0; column < saved.size(); ++column) {
        const net::ItemStack& stack = inventory_[kHotbarFirst + column];
        if (!stack.empty()) {
            saved[column] = client::SavedStack{std::string(item_name(stack.item_id)), stack.count,
                                               stack.nbt};
        }
    }
    saved_hotbars_.set_row(row, saved);
    if (auto written = saved_hotbars_.save(options_.hotbar_file); !written) {
        OV_LOG_WARN("{}: {}", options_.hotbar_file, client::to_string(written.error()));
    }
    if (creative_screen_) {
        creative_screen_->refresh_saved_hotbars();
    }
    // Vanilla says so in the chat, which this client does not draw yet; the
    // line over the hotbar is where this client says things.
    hud_.name_flash      = fmt::format("Saved toolbar {}", row + 1);
    hud_.name_flash_life = 1.0F;
    OV_LOG_INFO("saved hotbar {} to {}", row + 1, options_.hotbar_file);
}

void Interface::load_hotbar(netclient::Client& client, usize row) {
    if (registries_ == nullptr || !item_registry_) {
        return;
    }
    const client::SavedHotbars::Row& saved = saved_hotbars_.row(row);
    for (usize column = 0; column < saved.size(); ++column) {
        net::ItemStack stack;
        if (!saved[column].empty()) {
            if (const auto id = registries_->protocol_id(*item_registry_, saved[column].item)) {
                stack = net::ItemStack{static_cast<i32>(*id),
                                       static_cast<i8>(std::clamp(saved[column].count, 1, 127)),
                                       saved[column].nbt};
            } else {
                OV_LOG_WARN("saved hotbar: {} is not an item here; slot left empty",
                            saved[column].item);
            }
        }
        client.send_creative_slot(static_cast<i16>(kHotbarFirst + column), stack.item_id,
                                  stack.count, stack.nbt);
        inventory_[kHotbarFirst + column] = std::move(stack);
    }
    refresh_hotbar();
}

// ── The creative inventory ──────────────────────────────────────────────────

void Interface::toggle_creative(client::Window& window, netclient::Client& client) {
    if (creative_visible_) {
        creative_visible_   = false;
        creative_scrolling_ = false;
        window.set_cursor_captured(true);
        return;
    }
    if (!creative_screen_) {
        refusal_ = fmt::format("No creative inventory: {} is missing (scripts/setup_vanilla.sh)",
                               options_.creative_tabs);
        hud_.name_flash      = refusal_;
        hud_.name_flash_life = 1.0F;
        OV_LOG_WARN("no creative catalogue loaded; the screen is refused");
        return;
    }
    if (!hud_.creative) {
        OV_LOG_INFO("the creative inventory needs creative mode; the server is in survival");
        return;
    }
    if (!hint_labels_set_) {
        std::array<std::string, 9> keys;
        for (i32 i = 0; i < 9; ++i) {
            keys[static_cast<usize>(i)] = window.hotbar_key_label(i);
        }
        creative_screen_->set_saved_hotbars(&saved_hotbars_,
                                            window.key_label(client::Key::SaveToolbar), keys);
        hint_labels_set_ = true;
    }
    if (screen_) {
        close(window, client);
    }
    creative_visible_   = true;
    creative_opened_at_ = clock_;
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
    const client::CreativeCell* stack = creative_screen_->cell(cell);
    if (stack == nullptr) {
        return false;
    }
    const client::SlotStack taken = cell_stack(*stack);
    if (taken.empty()) {
        return false;
    }
    load_model();
    creative_model_.click_cell(taken, 2, false, effects_);
    carried_ = stack_of(creative_model_.carried());
    effects_.clear();
    return true;
}

void Interface::creative_put(netclient::Client& client, i16 slot) {
    load_model();
    creative_model_.click_slot(slot, 0, false,
                               creative_screen_ && creative_screen_->tab().type ==
                                                           render::CreativeTabType::Inventory
                                   ? client::CreativePage::Survival
                                   : client::CreativePage::Items,
                               effects_);
    store_model(client);
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
        const client::CreativeCell* stack = screen.cell(i);
        if (stack != nullptr) {
            out += fmt::format("\n  cell {:2}  {}{}", i, stack->item, stack->hint ? " (hint)" : "");
        }
    }
    return out;
}

client::GuiPoint Interface::creative_pointer() const {
    if (pointer_override_ && creative_screen_) {
        return client::GuiPoint{creative_screen_->origin_x(gui_->width()) + pointer_override_->x,
                                creative_screen_->origin_y(gui_->height()) + pointer_override_->y};
    }
    return client::GuiPoint{mouse_x_, mouse_y_};
}

u8 Interface::window_id() const noexcept {
    return screen_ && !own_inventory_ ? screen_->window_id() : u8{0};
}

void Interface::drag_over(netclient::Client& client, std::span<const i16> slots, bool right) {
    if (slots.empty()) {
        return;
    }
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

    // E on the search page is a letter, not a key: measured, the running
    // client keeps the screen open and the letter goes into the box.
    const bool typing = creative_visible_ && creative_screen_ && creative_screen_->searching();
    if (input.just_pressed(client::Key::Inventory) && !typing) {
        // E opens the creative inventory in creative and the player's own
        // inventory otherwise — and says so on screen when the catalogue that
        // the creative one needs is missing, instead of silently opening the
        // survival inventory in creative. That silence was the whole of the
        // "our client has no creative inventory" report.
        if (hud_.creative && !screen_) {
            toggle_creative(window, client);
            if (!creative_visible_ && !creative_screen_) {
                toggle_inventory(window, client);
            }
        } else if (creative_visible_) {
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
        // C or X held with a number key: save or load a hotbar row, in
        // creative, instead of selecting the slot.
        if (hud_.creative && input.hotbar_pressed >= 0 &&
            (input.held(client::Key::SaveToolbar) || input.held(client::Key::LoadToolbar))) {
            const auto row = static_cast<usize>(input.hotbar_pressed);
            if (input.held(client::Key::LoadToolbar)) {
                load_hotbar(client, row);
            } else {
                save_hotbar(row);
            }
            return false;
        }

        i32 selection = hud_.selected;
        if (input.hotbar_pressed >= 0) {
            selection = input.hotbar_pressed;
        } else if (input.scroll != 0.0) {
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
            if (hovered != nullptr && hovered->index != press_slot_) {
                dragging_ = true;
                drag_slots_.clear();
                drag_slots_.push_back(press_slot_);
                drag_slots_.push_back(hovered->index);
            }
            return true;
        }
        click_slot(client, press_slot_, press_right_ ? 1 : 0, input.shift_held, -1);
        press_pending_ = false;
        return true;
    }

    if (input.attack_pressed || input.use_pressed || input.middle_pressed) {
        const i32 button = input.middle_pressed ? 2 : (input.use_pressed ? 1 : 0);
        if (hovered == nullptr) {
            if (!carried_.empty()) {
                client.send_container_click(window_id(), state_id_, -999,
                                            static_cast<i8>(button),
                                            client::click_mode::kPickup, carried_);
            }
            return true;
        }
        if (button == 0 && !carried_.empty() && hovered->index == last_click_slot_ &&
            clock_ - last_click_time_ < kDoubleClickSeconds) {
            client.send_container_click(window_id(), state_id_, hovered->index, 0,
                                        client::click_mode::kPickupAll, carried_);
            last_click_slot_ = -1;
            return true;
        }
        last_click_slot_ = hovered->index;
        last_click_time_ = clock_;

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

    const client::GuiPoint       pointer = creative_pointer();
    const client::CreativeTarget target =
        screen.hit_test(gui_->width(), gui_->height(), pointer.x, pointer.y);
    const client::CreativePage page = screen.tab().type == render::CreativeTabType::Inventory
                                          ? client::CreativePage::Survival
                                          : client::CreativePage::Items;

    // Keys first. A number key over a stack acts on it and is then *not*
    // typed; T on a category page jumps to the search tab and is not typed
    // either — both measured on the running client.
    bool swallow_text = false;
    if (input.hotbar_pressed >= 0) {
        if (target.kind == client::CreativeHit::Cell) {
            if (const client::CreativeCell* cell = screen.cell(target.index)) {
                load_model();
                creative_model_.hotbar_key_on_cell(cell_stack(*cell), input.hotbar_pressed, effects_);
                store_model(client);
                swallow_text = true;
            }
        } else if (target.kind == client::CreativeHit::PlayerSlot) {
            load_model();
            creative_model_.hotbar_key_on_slot(static_cast<i16>(target.index), input.hotbar_pressed,
                                               effects_);
            store_model(client);
            swallow_text = true;
        }
    }
    if (!screen.searching()) {
        if (input.just_pressed(client::Key::Chat)) {
            (void)screen.select("minecraft:search");
            swallow_text = true;
        } else if (input.just_pressed(client::Key::Drop)) {
            // Q throws one, Ctrl+Q a stack — a copy of a cell, or out of a
            // slot. On the search page Q is a letter.
            load_model();
            if (target.kind == client::CreativeHit::Cell) {
                if (const client::CreativeCell* cell = screen.cell(target.index)) {
                    creative_model_.throw_cell(cell_stack(*cell), input.control_held, effects_);
                }
            } else if (target.kind == client::CreativeHit::PlayerSlot) {
                creative_model_.throw_slot(static_cast<i16>(target.index), input.control_held,
                                           effects_);
            }
            store_model(client);
        }
    } else if (!swallow_text && !input.typed.empty()) {
        screen.type(input.typed);
    }
    if (input.just_pressed(client::Key::Backspace)) {
        screen.backspace();
    }
    if (input.scroll != 0.0) {
        screen.scroll_by(static_cast<f32>(input.scroll));
    }

    if (creative_scrolling_) {
        screen.drag_scroll(gui_->height(), pointer.y);
        creative_scrolling_ = input.attack_held;
        return true;
    }

    if (!input.attack_pressed && !input.use_pressed && !input.middle_pressed) {
        return true;
    }
    const i32 button = input.middle_pressed ? 2 : (input.use_pressed ? 1 : 0);

    switch (target.kind) {
        case client::CreativeHit::Tab:
            screen.select(static_cast<usize>(target.index));
            return true;

        case client::CreativeHit::Scrollbar:
            creative_scrolling_ = true;
            screen.drag_scroll(gui_->height(), pointer.y);
            return true;

        case client::CreativeHit::Cell: {
            const client::CreativeCell* cell = screen.cell(target.index);
            if (cell == nullptr || cell->hint) {
                return true;
            }
            load_model();
            creative_model_.click_cell(cell_stack(*cell), button, input.shift_held, effects_);
            store_model(client);
            return true;
        }

        case client::CreativeHit::PlayerSlot:
            load_model();
            creative_model_.click_slot(static_cast<i16>(target.index), button, input.shift_held,
                                       page, effects_);
            store_model(client);
            return true;

        case client::CreativeHit::Destroy:
            load_model();
            creative_model_.click_destroy(input.shift_held, effects_);
            store_model(client);
            return true;

        case client::CreativeHit::SearchField:
            return true;

        case client::CreativeHit::None:
            break;
    }

    // Outside the panel (and off every button): the stack in hand is thrown.
    // Inside it, over nothing, nothing happens.
    const f32  ox      = screen.origin_x(gui_->width());
    const f32  oy      = screen.origin_y(gui_->height());
    const bool outside = pointer.x < ox || pointer.y < oy ||
                         pointer.x >= ox + client::creative_layout::kPanelWidth ||
                         pointer.y >= oy + client::creative_layout::kPanelHeight;
    if (outside && button != 2) {
        load_model();
        creative_model_.click_outside(button, effects_);
        store_model(client);
    }
    return true;
}

void Interface::draw(rhi::CommandList& cmd, u32 framebuffer_width, u32 framebuffer_height) {
    const u32 scale =
        options_.gui_scale != 0
            ? options_.gui_scale
            : client::auto_gui_scale(framebuffer_width, framebuffer_height, 0);
    gui_->begin(framebuffer_width, framebuffer_height, scale);

    // ── loading ──
    // Vanilla's loading screens — "Preparing spawn area", "Loading terrain" —
    // are the options background tiled every 32 GUI pixels at a quarter
    // brightness, with one white line centred 50 GUI pixels above the middle.
    // Opaque, so nothing of a world that is not there yet shows through.
    if (!loading_line_.empty()) {
        const f32 w = gui_->width();
        const f32 h = gui_->height();
        if (loading_background_ != client::GuiTexture::Invalid) {
            constexpr f32 kTile = 32.0F;
            for (f32 y = 0.0F; y < h; y += kTile) {
                for (f32 x = 0.0F; x < w; x += kTile) {
                    const f32 tw = std::min(kTile, w - x);
                    const f32 th = std::min(kTile, h - y);
                    gui_->quad(loading_background_, x, y, tw, th, 0.0F, 0.0F, tw / kTile,
                               th / kTile, 0xFF404040U);
                }
            }
        } else {
            gui_->fill(0.0F, 0.0F, w, h, 0xFF202020U);
        }
        gui_->text_centred(w * 0.5F, h * 0.5F - 50.0F, loading_line_, 0xFFFFFFFFU);
        gui_->flush(cmd);
        return;
    }
    // ── end loading ──

    if (options_.hud) {
        client::draw_hud(*gui_, *items_, textures_, hud_);
    }

    if (creative_visible_ && creative_screen_) {
        client::draw_screen_dim(*gui_);
        build_views(inventory_);
        const client::GuiPoint       pointer = creative_pointer();
        const client::CreativeTarget hovered =
            creative_screen_->hit_test(gui_->width(), gui_->height(), pointer.x, pointer.y);
        creative_textures_.background = client::GuiTexture::Invalid;
        for (const auto& [location, handle] : backgrounds_) {
            if (location == creative_screen_->background_texture()) {
                creative_textures_.background = handle;
            }
        }
        if (creative_textures_.background != client::GuiTexture::Invalid) {
            const bool cursor_on =
                static_cast<i64>((clock_ - creative_opened_at_) / kBlinkSeconds) % 2 == 0;
            creative_screen_->draw(*gui_, *items_, creative_textures_, views_, hovered, cursor_on);
        }
        if (!carried_.empty()) {
            const client::ItemStackView held = view_of(carried_);
            items_->draw(*gui_, pointer.x - 8.0F, pointer.y - 8.0F, held);
            items_->draw_count(*gui_, pointer.x - 8.0F, pointer.y - 8.0F, held);
        }
        // A stack's tooltip only with nothing in hand, as vanilla; a tab's
        // name always.
        if (carried_.empty() || hovered.kind == client::CreativeHit::Tab) {
            creative_screen_->tooltip(hovered, views_, tooltip_lines_);
            client::draw_tooltip(*gui_, tooltip_lines_, pointer.x, pointer.y);
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
        if (!carried_.empty()) {
            const client::ItemStackView held = view_of(carried_);
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
