// The four packets the scoreboard travels in: Display Objective (0x51),
// Update Objectives (0x58), Update Teams (0x5A) and Update Score (0x5B).
//
// Layouts from the frozen protocol page (`…/Protocol?oldid=2773082`, sections
// of the same names) and **captured from a real 1.20.1 server** by
// scripts/capture_scoreboard.py; the captured bytes are frozen in
// tests/test_scoreboard_packets.cpp. What the capture settled that the page
// leaves open:
//
//   * a score reset for *every* objective (`scoreboard players reset x`)
//     travels as Update Score action 1 with an **empty** objective name;
//   * a cleared display slot is Display Objective with an empty name;
//   * the team colour is the ChatFormatting index, 21 for `reset`.
//
// Everything lives in `ov::net::scoreboard` rather than `ov::net::clientbound`
// on purpose: another wave may declare the same four ids, and two headers
// defining `clientbound::kUpdateScore` in one translation unit would not
// compile. A merge keeps one of the two.
#pragma once

#include "ov/base/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net::scoreboard {

inline constexpr i32 kDisplayObjective = 0x51;
inline constexpr i32 kUpdateObjectives = 0x58;
inline constexpr i32 kUpdateTeams      = 0x5A;
inline constexpr i32 kUpdateScore      = 0x5B;

/// Display slots: 0 list, 1 sidebar, 2 belowName, 3 + colour for
/// `sidebar.team.<colour>` (black 3 … white 18).
inline constexpr u8 kSlotList      = 0;
inline constexpr u8 kSlotSidebar   = 1;
inline constexpr u8 kSlotBelowName = 2;
inline constexpr u8 kSlotTeamBase  = 3;
inline constexpr u8 kSlotCount     = 19;

/// ChatFormatting's index for "no colour", what a new team has.
inline constexpr i32 kColorReset = 21;

struct DisplayObjective {
    u8          slot{0};
    /// Empty: the slot is cleared.
    std::string objective;

    friend bool operator==(const DisplayObjective&, const DisplayObjective&) = default;
};

enum class ObjectiveMode : u8 { Create = 0, Remove = 1, Update = 2 };

struct ObjectiveUpdate {
    std::string   name;
    ObjectiveMode mode{ObjectiveMode::Create};
    /// Create and Update only: the display name as a JSON component, and 0
    /// integer / 1 hearts.
    std::string   display_json;
    i32           render_type{0};

    friend bool operator==(const ObjectiveUpdate&, const ObjectiveUpdate&) = default;
};

enum class TeamMode : u8 { Create = 0, Remove = 1, Update = 2, AddEntities = 3, RemoveEntities = 4 };

/// Friendly flags: 0x01 friendly fire, 0x02 see invisible teammates.
inline constexpr u8 kTeamFriendlyFire         = 0x01;
inline constexpr u8 kTeamSeeFriendlyInvisible = 0x02;

struct TeamParameters {
    std::string display_json;
    u8          flags{kTeamFriendlyFire | kTeamSeeFriendlyInvisible};
    /// "always", "never", "hideForOtherTeams", "hideForOwnTeam".
    std::string nametag_visibility{"always"};
    /// "always", "never", "pushOtherTeams", "pushOwnTeam".
    std::string collision_rule{"always"};
    i32         color{kColorReset};
    std::string prefix_json;
    std::string suffix_json;

    friend bool operator==(const TeamParameters&, const TeamParameters&) = default;
};

struct TeamUpdate {
    std::string              name;
    TeamMode                 mode{TeamMode::Create};
    /// Create and Update.
    TeamParameters           parameters;
    /// Create, AddEntities and RemoveEntities.
    std::vector<std::string> entities;

    friend bool operator==(const TeamUpdate&, const TeamUpdate&) = default;
};

enum class ScoreAction : u8 { Change = 0, Remove = 1 };

struct ScoreUpdate {
    std::string holder;
    ScoreAction action{ScoreAction::Change};
    /// Empty with Remove: every objective of the holder.
    std::string objective;
    /// Change only.
    i32         value{0};

    friend bool operator==(const ScoreUpdate&, const ScoreUpdate&) = default;
};

[[nodiscard]] std::vector<u8> encode_display_objective(const DisplayObjective& packet);
[[nodiscard]] std::vector<u8> encode_update_objectives(const ObjectiveUpdate& packet);
[[nodiscard]] std::vector<u8> encode_update_teams(const TeamUpdate& packet);
[[nodiscard]] std::vector<u8> encode_update_score(const ScoreUpdate& packet);

/// The inverse, for the client and the tests. Nullopt for a truncated
/// payload, an unknown mode, or bytes left over.
[[nodiscard]] std::optional<DisplayObjective> parse_display_objective(std::span<const u8> payload);
[[nodiscard]] std::optional<ObjectiveUpdate>  parse_update_objectives(std::span<const u8> payload);
[[nodiscard]] std::optional<TeamUpdate>       parse_update_teams(std::span<const u8> payload);
[[nodiscard]] std::optional<ScoreUpdate>      parse_update_score(std::span<const u8> payload);

}  // namespace ov::net::scoreboard
