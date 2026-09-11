#include "ov/client/menu_layouts.hpp"

#include <charconv>
#include <cmath>

namespace ov::client {
namespace {

[[nodiscard]] Widget button(std::string id, f32 x, f32 y, f32 w, std::string key, f32 h = 20.0F) {
    Widget widget;
    widget.id   = std::move(id);
    widget.kind = WidgetKind::Button;
    widget.rect = Rect{x, y, w, h};
    widget.text = std::move(key);
    return widget;
}

[[nodiscard]] Widget slider(std::string id, f32 x, f32 y, f32 w, std::string key) {
    Widget widget = button(std::move(id), x, y, w, std::move(key));
    widget.kind   = WidgetKind::Slider;
    return widget;
}

[[nodiscard]] f32 half(f32 value) {
    return std::floor(value / 2.0F);
}

/// An options list, vanilla's: two 150-wide columns 10 apart around the
/// centre, one row every 25 pixels from `top` + 4.
struct OptionRows {
    f32 width;
    f32 top;
    f32 row{0.0F};

    [[nodiscard]] f32 y() const { return top + 4.0F + row * 25.0F; }
    [[nodiscard]] f32 left() const { return half(width) - 155.0F; }
    [[nodiscard]] f32 right() const { return half(width) + 5.0F; }
};

}  // namespace

std::vector<Widget> title_layout(f32 width, f32 height) {
    const f32           top = std::floor(height / 4.0F) + 48.0F;
    const f32           x   = half(width) - 100.0F;
    std::vector<Widget> out;
    out.push_back(button("singleplayer", x, top, 200.0F, "menu.singleplayer"));
    out.push_back(button("multiplayer", x, top + 24.0F, 200.0F, "menu.multiplayer"));
    out.push_back(button("realms", x, top + 48.0F, 200.0F, "menu.online"));
    const f32 row = top + 72.0F + 12.0F;
    out.push_back(button("language", half(width) - 124.0F, row, 20.0F, "narrator.button.language"));
    out.back().kind = WidgetKind::Icon;
    out.push_back(button("options", x, row, 98.0F, "menu.options"));
    out.push_back(button("quit", half(width) + 2.0F, row, 98.0F, "menu.quit"));
    out.push_back(button("accessibility", half(width) + 104.0F, row, 20.0F,
                         "narrator.button.accessibility"));
    out.back().kind = WidgetKind::Icon;
    return out;
}

std::vector<Widget> pause_layout(f32 width, f32 height, bool integrated) {
    // A grid of two 98-wide columns with 4 pixels around each cell and 4
    // above, the wide buttons spanning both; the grid is centred across and
    // placed a quarter of the way down the space it leaves.
    constexpr f32 kRow     = 24.0F;
    constexpr f32 kColumn  = 106.0F;
    const f32     grid_w   = 2.0F * kColumn;
    const f32     grid_h   = 5.0F * kRow;
    const f32     left     = half(width - grid_w);
    const f32     top      = std::floor((height - grid_h) * 0.25F);
    const f32     x0       = left + 4.0F;
    const f32     x1       = left + kColumn + 4.0F;
    const auto    y        = [&](f32 row) { return top + row * kRow + 4.0F; };
    std::vector<Widget> out;
    out.push_back(button("resume", x0, y(0), 204.0F, "menu.returnToGame"));
    out.push_back(button("advancements", x0, y(1), 98.0F, "gui.advancements"));
    out.push_back(button("stats", x1, y(1), 98.0F, "gui.stats"));
    out.push_back(button("feedback", x0, y(2), 98.0F, "menu.sendFeedback"));
    out.push_back(button("bugs", x1, y(2), 98.0F, "menu.reportBugs"));
    out.push_back(button("options", x0, y(3), 98.0F, "menu.options"));
    out.push_back(integrated ? button("lan", x1, y(3), 98.0F, "menu.shareToLan")
                             : button("reporting", x1, y(3), 98.0F, "menu.playerReporting"));
    out.push_back(button("leave", x0, y(4), 204.0F,
                         integrated ? "menu.returnToMenu" : "menu.disconnect"));
    return out;
}

std::vector<Widget> death_layout(f32 width, f32 height) {
    const f32           x   = half(width) - 100.0F;
    const f32           top = std::floor(height / 4.0F);
    std::vector<Widget> out;
    out.push_back(button("respawn", x, top + 72.0F, 200.0F, "deathScreen.respawn"));
    out.push_back(button("title", x, top + 96.0F, 200.0F, "deathScreen.titleScreen"));
    return out;
}

std::vector<Widget> options_layout(f32 width, f32 height, bool in_game) {
    const f32 left  = half(width) - 155.0F;
    const f32 right = half(width) + 5.0F;
    const f32 top   = std::floor(height / 6.0F) - 12.0F;
    std::vector<Widget> out;
    out.push_back(slider("fov", left, top, 150.0F, "options.fov"));
    if (in_game) {
        out.push_back(button("difficulty", right, top, 130.0F, "options.difficulty"));
        out.push_back(button("difficulty_lock", right + 130.0F, top, 20.0F, "difficulty.lock.title"));
    } else {
        out.push_back(button("online", right, top, 150.0F, "options.online"));
    }
    const f32 y = top + 48.0F;
    out.push_back(button("skin", left, y, 150.0F, "options.skinCustomisation"));
    out.push_back(button("sounds", right, y, 150.0F, "options.sounds"));
    out.push_back(button("video", left, y + 24.0F, 150.0F, "options.video"));
    out.push_back(button("controls", right, y + 24.0F, 150.0F, "options.controls"));
    out.push_back(button("language", left, y + 48.0F, 150.0F, "options.language"));
    out.push_back(button("chat", right, y + 48.0F, 150.0F, "options.chat.title"));
    out.push_back(button("packs", left, y + 72.0F, 150.0F, "options.resourcepack"));
    out.push_back(button("accessibility", right, y + 72.0F, 150.0F, "options.accessibility.title"));
    out.push_back(button("telemetry", left, y + 96.0F, 150.0F, "options.telemetry"));
    out.push_back(button("credits", right, y + 96.0F, 150.0F, "options.credits_and_attribution"));
    out.push_back(button("done", half(width) - 100.0F, y + 120.0F + 4.0F, 200.0F, "gui.done"));
    return out;
}

std::vector<Widget> video_layout(f32 width, f32 height) {
    OptionRows          rows{width, 32.0F};
    std::vector<Widget> out;
    const auto pair = [&](std::string a, std::string ka, bool slide_a, std::string b,
                          std::string kb, bool slide_b) {
        out.push_back(slide_a ? slider(a, rows.left(), rows.y(), 150.0F, ka)
                              : button(a, rows.left(), rows.y(), 150.0F, ka));
        if (!b.empty()) {
            out.push_back(slide_b ? slider(b, rows.right(), rows.y(), 150.0F, kb)
                                  : button(b, rows.right(), rows.y(), 150.0F, kb));
        }
        rows.row += 1.0F;
    };
    pair("graphics", "options.graphics", false, "render_distance", "options.renderDistance", true);
    pair("chunk_updates", "options.prioritizeChunkUpdates", false, "simulation_distance",
         "options.simulationDistance", true);
    pair("smooth_lighting", "options.ao", false, "max_fps", "options.framerateLimit", true);
    pair("vsync", "options.vsync", false, "view_bobbing", "options.viewBobbing", false);
    pair("gui_scale", "options.guiScale", false, "attack_indicator", "options.attackIndicator",
         false);
    pair("gamma", "options.gamma", true, "clouds", "options.renderClouds", false);
    pair("fullscreen", "options.fullscreen", false, "particles", "options.particles", false);
    pair("mipmaps", "options.mipmapLevels", true, "biome_blend", "options.biomeBlendRadius", true);
    pair("entity_distance", "options.entityDistanceScaling", true, "entity_shadows",
         "options.entityShadows", false);
    out.push_back(button("done", half(width) - 100.0F, height - 27.0F, 200.0F, "gui.done"));
    return out;
}

std::vector<Widget> sounds_layout(f32 width, f32 height) {
    OptionRows          rows{width, 32.0F};
    std::vector<Widget> out;
    out.push_back(slider("volume_master", rows.left(), rows.y(), 310.0F, "soundCategory.master"));
    rows.row += 1.0F;
    static constexpr std::array<std::string_view, 9> kRest{
        "music", "record", "weather", "block", "hostile", "neutral", "player", "ambient", "voice"};
    for (usize i = 0; i < kRest.size(); ++i) {
        const bool right = (i % 2) == 1;
        out.push_back(slider("volume_" + std::string(kRest[i]), right ? rows.right() : rows.left(),
                             rows.y(), 150.0F, "soundCategory." + std::string(kRest[i])));
        if (right || i + 1 == kRest.size()) {
            rows.row += 1.0F;
        }
    }
    out.push_back(button("device", rows.left(), rows.y(), 310.0F, "options.audioDevice"));
    rows.row += 1.0F;
    out.push_back(button("subtitles", rows.left(), rows.y(), 150.0F, "options.showSubtitles"));
    out.push_back(button("directional", rows.right(), rows.y(), 150.0F, "options.directionalAudio"));
    out.push_back(button("done", half(width) - 100.0F, height - 27.0F, 200.0F, "gui.done"));
    return out;
}

std::vector<Widget> controls_layout(f32 width, f32 height) {
    const f32           left  = half(width) - 155.0F;
    const f32           right = half(width) + 5.0F;
    const f32           top   = std::floor(height / 6.0F) - 12.0F;
    std::vector<Widget> out;
    out.push_back(button("mouse", left, top, 150.0F, "options.mouse_settings"));
    out.push_back(button("keybinds", right, top, 150.0F, "controls.keybinds"));
    out.push_back(button("sneak", left, top + 24.0F, 150.0F, "key.sneak"));
    out.push_back(button("sprint", right, top + 24.0F, 150.0F, "key.sprint"));
    out.push_back(button("auto_jump", left, top + 48.0F, 150.0F, "options.autoJump"));
    out.push_back(button("operator_tab", right, top + 48.0F, 150.0F, "options.operatorItemsTab"));
    out.push_back(button("done", half(width) - 100.0F, top + 72.0F + 4.0F, 200.0F, "gui.done"));
    return out;
}

std::vector<Widget> key_binds_layout(f32 width, f32 height, std::span<const KeyRow> rows,
                                     f32 scroll) {
    std::vector<Widget> out;
    constexpr f32       kTop     = 32.0F;
    constexpr f32       kRowH    = 20.0F;
    const f32           bottom   = height - 32.0F;
    const f32           key_x    = half(width) + 40.0F;
    f32                 y        = kTop + 4.0F - scroll;
    std::string_view    category;
    for (const KeyRow& row : rows) {
        if (row.category != category) {
            category = row.category;
            Widget header;
            header.id     = "category:" + std::string(category);
            header.kind   = WidgetKind::Label;
            header.rect   = Rect{0.0F, y + 6.0F, width, 9.0F};
            header.text   = std::string(category);
            if (y >= kTop && y + kRowH <= bottom) {
                out.push_back(header);
            }
            y += kRowH;
        }
        if (y >= kTop && y + kRowH <= bottom) {
            Widget label;
            label.id   = "label:" + std::string(row.name);
            label.kind = WidgetKind::Label;
            label.rect = Rect{key_x - 90.0F - 200.0F, y + 5.0F, 200.0F, 9.0F};
            label.text = std::string(row.name);
            out.push_back(label);
            out.push_back(button("key:" + std::string(row.name), key_x, y, 75.0F, ""));
            out.push_back(button("reset:" + std::string(row.name), key_x + 80.0F, y, 50.0F,
                                 "controls.reset"));
        }
        y += kRowH;
    }
    out.push_back(button("reset_all", half(width) - 155.0F, height - 29.0F, 150.0F,
                         "controls.resetAll"));
    out.push_back(button("done", half(width) + 5.0F, height - 29.0F, 150.0F, "gui.done"));
    return out;
}

ListBox select_world_list(f32 width, f32 height) {
    (void)width;
    ListBox box;
    box.top        = 48.0F;
    box.bottom     = height - 64.0F;
    box.row_height = 36.0F;
    box.row_width  = 270.0F;
    box.first_row  = box.top + 4.0F;
    return box;
}

std::vector<Widget> select_world_layout(f32 width, f32 height) {
    std::vector<Widget> out;
    Widget              search;
    search.id   = "search";
    search.kind = WidgetKind::EditBox;
    search.rect = Rect{half(width) - 100.0F, 22.0F, 200.0F, 20.0F};
    out.push_back(search);
    out.push_back(button("play", half(width) - 154.0F, height - 52.0F, 150.0F,
                         "selectWorld.select"));
    out.push_back(button("create", half(width) + 4.0F, height - 52.0F, 150.0F,
                         "selectWorld.create"));
    out.push_back(button("edit", half(width) - 154.0F, height - 28.0F, 72.0F, "selectWorld.edit"));
    out.push_back(button("delete", half(width) - 76.0F, height - 28.0F, 72.0F,
                         "selectWorld.delete"));
    out.push_back(button("recreate", half(width) + 4.0F, height - 28.0F, 72.0F,
                         "selectWorld.recreate"));
    out.push_back(button("cancel", half(width) + 82.0F, height - 28.0F, 72.0F, "gui.cancel"));
    return out;
}

// ── allow-commands ──
CreateGameMode next_game_mode(CreateGameMode mode) noexcept {
    switch (mode) {
        case CreateGameMode::Survival: return CreateGameMode::Hardcore;
        case CreateGameMode::Hardcore: return CreateGameMode::Creative;
        case CreateGameMode::Creative: return CreateGameMode::Survival;
    }
    return CreateGameMode::Survival;
}

std::vector<Widget> create_world_layout(f32 width, f32 height, i32 tab) {
    std::vector<Widget> out;
    static constexpr std::array<std::string_view, 3> kTabs{"createWorld.tab.game.title",
                                                           "createWorld.tab.world.title",
                                                           "createWorld.tab.more.title"};
    // Measured (854×480): tabs 124 wide from x = 242 = w/2 − 185, y 0..24.
    for (usize i = 0; i < kTabs.size(); ++i) {
        Widget t;
        t.id       = "tab" + std::to_string(i);
        t.kind     = WidgetKind::Tab;
        t.rect     = Rect{half(width) - 185.0F + 124.0F * static_cast<f32>(i), 0.0F, 124.0F, 24.0F};
        t.text     = std::string(kTabs[i]);
        t.selected = static_cast<i32>(i) == tab;
        out.push_back(t);
    }
    const auto label = [&](std::string id, f32 x, f32 y, std::string key) {
        Widget text;
        text.id     = std::move(id);
        text.kind   = WidgetKind::Label;
        text.rect   = Rect{x, y, 0.0F, 9.0F};
        text.text   = std::move(key);
        text.colour       = 0xFFFFFFFFU;
        text.left_aligned = true;
        out.push_back(text);
    };
    // The Game and More pages: one 210-wide column at w/2 − 105, a step of 28.
    const f32 x = half(width) - 105.0F;
    if (tab == 0) {
        label("name_label", half(width) - 104.0F, 75.0F, "selectWorld.enterName");
        Widget name;
        name.id   = "world_name";
        name.kind = WidgetKind::EditBox;
        name.rect = Rect{half(width) - 104.0F, 89.0F, 208.0F, 20.0F};
        out.push_back(name);
        out.push_back(button("game_mode", x, 118.0F, 210.0F, "selectWorld.gameMode"));
        out.push_back(button("difficulty", x, 146.0F, 210.0F, "options.difficulty"));
        out.push_back(button("cheats", x, 174.0F, 210.0F, "selectWorld.allowCommands"));
    } else if (tab == 1) {
        out.push_back(button("world_type", half(width) - 155.0F, 75.0F, 150.0F,
                             "selectWorld.mapType"));
        out.push_back(button("customize", half(width) + 5.0F, 75.0F, 150.0F,
                             "selectWorld.customizeType"));
        label("seed_label", half(width) - 155.0F, 103.0F, "selectWorld.enterSeed");
        Widget seed;
        seed.id   = "seed";
        seed.kind = WidgetKind::EditBox;
        seed.rect = Rect{half(width) - 154.0F, 117.0F, 308.0F, 20.0F};
        seed.hint = "selectWorld.seedInfo";  // the owner translates it
        out.push_back(seed);
        // Each toggle is a caption on the left and a 44-wide ON/OFF button
        // ending at the right edge of the seed field.
        label("structures_label", half(width) - 154.0F, 156.0F, "selectWorld.mapFeatures");
        out.push_back(button("structures", half(width) + 111.0F, 150.0F, 44.0F, "options.on"));
        label("bonus_label", half(width) - 154.0F, 180.0F, "selectWorld.bonusItems");
        out.push_back(button("bonus_chest", half(width) + 111.0F, 174.0F, 44.0F, "options.off"));
    } else {
        out.push_back(button("game_rules", x, 82.0F, 210.0F, "selectWorld.gameRules"));
        out.push_back(button("experiments", x, 110.0F, 210.0F, "selectWorld.experiments"));
        out.push_back(button("data_packs", x, 138.0F, 210.0F, "selectWorld.dataPacks"));
    }
    out.push_back(button("create", half(width) - 155.0F, height - 28.0F, 150.0F,
                         "selectWorld.create"));
    out.push_back(button("cancel", half(width) + 5.0F, height - 28.0F, 150.0F, "gui.cancel"));
    return out;
}

std::vector<Widget> direct_connect_layout(f32 width, f32 height) {
    std::vector<Widget> out;
    Widget              address;
    address.id   = "address";
    address.kind = WidgetKind::EditBox;
    address.rect = Rect{half(width) - 100.0F, 116.0F, 200.0F, 20.0F};
    out.push_back(address);
    const f32 top = std::floor(height / 4.0F);
    out.push_back(button("join", half(width) - 100.0F, top + 96.0F + 12.0F, 200.0F,
                         "selectServer.select"));
    out.push_back(button("cancel", half(width) - 100.0F, top + 120.0F + 12.0F, 200.0F,
                         "gui.cancel"));
    return out;
}

ListBox language_list(f32 width, f32 height) {
    (void)width;
    ListBox box;
    box.top        = 32.0F;
    box.bottom     = height - 65.0F + 4.0F;
    box.row_height = 18.0F;
    box.row_width  = 220.0F;
    box.first_row  = box.top + 4.0F;
    return box;
}

std::vector<Widget> language_layout(f32 width, f32 height) {
    std::vector<Widget> out;
    out.push_back(button("done", half(width) - 100.0F, height - 38.0F, 200.0F, "gui.done"));
    return out;
}

i32 java_string_hash(std::string_view utf8) {
    u32   hash = 0;
    usize i    = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        u32        codepoint = lead;
        usize      length    = 1;
        if (lead >= 0xF0U && i + 3 < utf8.size() + 0) {
            codepoint = ((lead & 0x07U) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3FU) << 12) |
                        ((static_cast<unsigned char>(utf8[i + 2]) & 0x3FU) << 6) |
                        (static_cast<unsigned char>(utf8[i + 3]) & 0x3FU);
            length = 4;
        } else if (lead >= 0xE0U && i + 2 < utf8.size()) {
            codepoint = ((lead & 0x0FU) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3FU) << 6) |
                        (static_cast<unsigned char>(utf8[i + 2]) & 0x3FU);
            length = 3;
        } else if (lead >= 0xC0U && i + 1 < utf8.size()) {
            codepoint = ((lead & 0x1FU) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3FU);
            length = 2;
        }
        i += length;
        // Java hashes UTF-16 units: a codepoint above the BMP is two.
        if (codepoint >= 0x10000U) {
            const u32 v = codepoint - 0x10000U;
            hash        = hash * 31U + (0xD800U + (v >> 10));
            hash        = hash * 31U + (0xDC00U + (v & 0x3FFU));
        } else {
            hash = hash * 31U + codepoint;
        }
    }
    return static_cast<i32>(hash);
}

std::optional<i64> seed_from_text(std::string_view text) {
    // Vanilla strips the field before deciding.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    i64         value = 0;
    const char* first = text.data();
    if (!text.empty() && text.front() == '+') {
        ++first;
    }
    const auto [end, ec] = std::from_chars(first, text.data() + text.size(), value);
    if (ec == std::errc{} && end == text.data() + text.size() && first != end) {
        return value;
    }
    return static_cast<i64>(java_string_hash(text));
}

}  // namespace ov::client
