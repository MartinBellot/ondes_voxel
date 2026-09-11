// Where vanilla 1.20.1 puts every button of its menus, as functions of the
// GUI size.
//
// Each layout was read off the running client, widget by widget, with the
// window at 1280×720 and the GUI scale at 3 (854×480 GUI pixels) —
// scripts/measure_screens.py, docs/provenance/ecrans.md — and is written here
// as the rule that produces those numbers, not as the numbers: vanilla
// centres on `width / 2` and stacks from `height / 4` or `height / 6`, so the
// same function gives the right answer at any window size. The tests check
// the rule against the measured numbers.
//
// A layout is the widgets' ids, kinds, rectangles and *translation keys*;
// the owner translates and fills in values (a slider's position, a toggle's
// state). Nothing here draws.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/menu_widgets.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::client {

/// The title screen: Singleplayer, Multiplayer, Minecraft Realms, Options…,
/// Quit Game, and the two 20×20 icon buttons beside Options and Quit.
[[nodiscard]] std::vector<Widget> title_layout(f32 width, f32 height);

/// The pause menu. `integrated`: a singleplayer game, whose last button is
/// "Save and Quit to Title" and whose second-to-last is "Open to LAN";
/// otherwise "Disconnect" and "Player Reporting".
[[nodiscard]] std::vector<Widget> pause_layout(f32 width, f32 height, bool integrated);

/// The death screen: Respawn and Title Screen.
[[nodiscard]] std::vector<Widget> death_layout(f32 width, f32 height);

/// The Options screen. `in_game`: the difficulty button appears.
[[nodiscard]] std::vector<Widget> options_layout(f32 width, f32 height, bool in_game);

/// Video Settings, Music & Sounds, Controls: two columns of 150-wide options
/// in a list, and Done at the bottom.
[[nodiscard]] std::vector<Widget> video_layout(f32 width, f32 height);
[[nodiscard]] std::vector<Widget> sounds_layout(f32 width, f32 height);
[[nodiscard]] std::vector<Widget> controls_layout(f32 width, f32 height);

/// Key Binds: one row per binding, scrolled by `scroll` pixels; the key
/// button and Reset at the right of each row, category headers between.
struct KeyRow {
    std::string_view name;
    std::string_view category;
};
[[nodiscard]] std::vector<Widget> key_binds_layout(f32 width, f32 height,
                                                   std::span<const KeyRow> rows, f32 scroll);

/// Select World: the list's box, then the buttons under it.
[[nodiscard]] std::vector<Widget> select_world_layout(f32 width, f32 height);

/// Create World: the three tabs, the page of the current tab, and the two
/// buttons at the bottom. `tab` is 0 Game, 1 World, 2 More.
[[nodiscard]] std::vector<Widget> create_world_layout(f32 width, f32 height, i32 tab);

/// Direct Connection: the address box, Join Server and Cancel.
[[nodiscard]] std::vector<Widget> direct_connect_layout(f32 width, f32 height);

/// Language: the list and Done.
[[nodiscard]] std::vector<Widget> language_layout(f32 width, f32 height);

/// Where the world list's rows start and how tall they are, for both drawing
/// and clicking.
struct ListBox {
    f32 top{0.0F};
    f32 bottom{0.0F};
    f32 row_height{36.0F};
    f32 row_width{220.0F};
    /// The first row's top, before scrolling.
    f32 first_row{0.0F};
};
[[nodiscard]] ListBox select_world_list(f32 width, f32 height);
[[nodiscard]] ListBox language_list(f32 width, f32 height);

/// Vanilla's seed rule: blank is a random seed (the caller's), a number that
/// fits in a long is that number, anything else is Java's `String.hashCode`
/// of the text. Returns nullopt for blank.
[[nodiscard]] std::optional<i64> seed_from_text(std::string_view text);

/// Java's `String.hashCode` over the UTF-16 units of `utf8`.
[[nodiscard]] i32 java_string_hash(std::string_view utf8);

}  // namespace ov::client
