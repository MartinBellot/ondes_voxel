// The scoreboard as our client holds it, and the sidebar it draws.
//
// The four packets of ov/protocol/scoreboard_packets.hpp, applied in arrival
// order, build the client's copy: objectives and their display names, the
// nineteen display slots, the scores, the teams and who is on them.
//
// The sidebar shows the objective of `sidebar.team.<colour>` when the player
// is on a team of that colour and that slot is set, else the `sidebar` slot:
// at most fifteen scores, holders that start with `#` left out, highest
// first and ties by name; each name dressed by its team (colour, prefix,
// suffix); the score in red at the right edge. That much is the Minecraft
// Wiki's description of the sidebar. The pixel layout below — right edge,
// centred a third below the middle, a darker title row — is **not measured
// against the vanilla client**: the render-parity rig belongs to another
// wave, and it is named in docs/provenance/scoreboard.md.
//
// `below_name_score` is the belowName slot's value for a holder, for whatever
// draws name tags; nothing draws it yet.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/scoreboard_packets.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::render {
class Language;
}

namespace ov::client {

class Gui;

class ScoreboardView {
public:
    void apply(const net::scoreboard::DisplayObjective& packet);
    void apply(const net::scoreboard::ObjectiveUpdate& packet);
    void apply(const net::scoreboard::TeamUpdate& packet);
    void apply(const net::scoreboard::ScoreUpdate& packet);

    /// The local player's name, for `sidebar.team.<colour>`.
    void set_own_name(std::string name) { own_name_ = std::move(name); }

    struct Line {
        std::string holder;
        i32         score{0};

        friend bool operator==(const Line&, const Line&) = default;
    };

    /// The objective the sidebar shows; empty for none.
    [[nodiscard]] std::string sidebar_objective() const;
    /// Its lines, top down.
    [[nodiscard]] std::vector<Line> sidebar_lines() const;
    /// A holder's name dressed by its team, `§`-coded for the GUI's text.
    [[nodiscard]] std::string dressed_name(std::string_view holder,
                                           const render::Language& language) const;
    [[nodiscard]] std::optional<i32> below_name_score(std::string_view holder) const;

    void draw(Gui& gui, const render::Language& language) const;

    // ── hud ── what the tab list reads: the team a holder is on (by name, ""
    // for none), the list slot's objective, whether it shows hearts, a score.
    [[nodiscard]] std::string team_name(std::string_view holder) const {
        const auto it = team_of_.find(std::string{holder});
        return it == team_of_.end() ? std::string{} : it->second;
    }
    [[nodiscard]] std::string list_objective() const { return display_[net::scoreboard::kSlotList]; }
    [[nodiscard]] bool objective_is_hearts(std::string_view objective) const {
        const auto it = objectives_.find(std::string{objective});
        return it != objectives_.end() && it->second.render_type == 1;
    }
    [[nodiscard]] std::optional<i32> score(std::string_view objective, std::string_view holder) const {
        const auto it = scores_.find(std::string{objective});
        if (it == scores_.end()) {
            return std::nullopt;
        }
        const auto score = it->second.find(std::string{holder});
        return score == it->second.end() ? std::nullopt : std::optional<i32>{score->second};
    }
    // ── end hud ──

private:
    struct Objective {
        std::string display_json;
        i32         render_type{0};
    };

    std::unordered_map<std::string, Objective>                              objectives_;
    std::array<std::string, net::scoreboard::kSlotCount>                    display_{};
    std::unordered_map<std::string, std::unordered_map<std::string, i32>>   scores_;
    std::unordered_map<std::string, net::scoreboard::TeamParameters>        teams_;
    std::unordered_map<std::string, std::string>                            team_of_;
    std::string                                                             own_name_;
};

}  // namespace ov::client
