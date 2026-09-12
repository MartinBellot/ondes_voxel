// The player list, held on Tab.
//
// Player Info Update, Player Info Remove and Set Tab List Header And Footer,
// applied in arrival order. Laid out as measured on the real 1.20.1 client
// (docs/provenance/hud.md § tab): from y 10, one row of 9 pixels a player on a
// cell of white at an eighth, black at half behind the whole block, the name
// dressed by its team, the list objective's score in yellow after the widest
// name, the latency's bars at the cell's right end — five under 150 ms — and
// at most twenty rows a column. Spectators are listed last, in italics, at
// 0x90 alpha.
//
// Heads are not drawn: the real client draws them only for an integrated
// server or an encrypted connection, and offline-mode servers are neither.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/netclient/client.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ov::render {
class Language;
}

namespace ov::client {

class ScoreboardView;

class TabListView {
public:
    struct Entry {
        net::Uuid                  uuid{};
        std::string                name;
        i32                        game_mode{0};
        i32                        latency{0};
        bool                       listed{false};
        std::optional<std::string> display_json;
    };

    void apply(const netclient::ClientEvents::TabListEvent& event);
    void clear();

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    /// Listed players in drawing order — spectators last, then by team name,
    /// then by name ignoring case — at most eighty.
    [[nodiscard]] std::vector<const Entry*> listed(const ScoreboardView& scoreboard) const;

    /// Vanilla shows the list while the key is held, except on an integrated
    /// server with one player and no list objective.
    [[nodiscard]] bool should_show(bool key_down, bool integrated,
                                   const ScoreboardView& scoreboard) const;

    [[nodiscard]] const std::string& header() const noexcept { return header_json_; }
    [[nodiscard]] const std::string& footer() const noexcept { return footer_json_; }

    /// The latency's sprite row, 0 (five bars) to 5 (no connection).
    [[nodiscard]] static i32 ping_row(i32 latency) noexcept;

    void draw(Gui& gui, GuiTexture icons, const ScoreboardView& scoreboard,
              const render::Language& language) const;

private:
    std::vector<Entry> entries_;
    std::string        header_json_;
    std::string        footer_json_;
};

}  // namespace ov::client
