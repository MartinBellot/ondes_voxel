// The HUD packets of protocol 763: boss bars, the world border, the scoreboard,
// statistics and the advancement tabs.
//
// Field layouts from the frozen protocol page (oldid=2773082, "1.20.1, protocol
// 763"), cross-checked field by field against PrismarineJS/minecraft-data
// pc/1.20 (MIT). The two agree everywhere but in one place: the world border's
// lerp time is a VarLong on the page and a VarInt in minecraft-data. The page is
// followed — see docs/provenance/protocole-763.md.
//
// Every decoder is written for a hostile peer: nullopt on a short read, on
// trailing bytes, on an enum value outside its table, and on a count larger than
// the bytes left could possibly hold — refused before anything is allocated.
//
// Encoders return the packet body, without the id, like every other packet.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::net {

namespace clientbound {
inline constexpr i32 kAwardStatistics          = 0x05;
inline constexpr i32 kBossBar                  = 0x0B;
inline constexpr i32 kInitializeWorldBorder    = 0x22;
inline constexpr i32 kSelectAdvancementsTab    = 0x44;
inline constexpr i32 kSetBorderCenter          = 0x47;
inline constexpr i32 kSetBorderLerpSize        = 0x48;
inline constexpr i32 kSetBorderSize            = 0x49;
inline constexpr i32 kSetBorderWarningDelay    = 0x4A;
inline constexpr i32 kSetBorderWarningDistance = 0x4B;
inline constexpr i32 kDisplayObjective         = 0x51;
inline constexpr i32 kUpdateObjectives         = 0x58;
inline constexpr i32 kUpdateTeams              = 0x5A;
inline constexpr i32 kUpdateScore              = 0x5B;
}  // namespace clientbound

namespace serverbound {
inline constexpr i32 kSeenAdvancements = 0x25;
}  // namespace serverbound

// ── Boss Bar ────────────────────────────────────────────────────────────────

/// What a Boss Bar packet does; it decides which fields follow.
enum class BossBarAction : u8 {
    Add          = 0,
    Remove       = 1,
    UpdateHealth = 2,
    UpdateTitle  = 3,
    UpdateStyle  = 4,
    UpdateFlags  = 5,
};

/// Colours 0..6: pink, blue, red, green, yellow, purple, white.
inline constexpr i32 kBossBarColors = 7;
/// Divisions 0..4: none, 6, 10, 12 and 20 notches.
inline constexpr i32 kBossBarDivisions = 5;

inline constexpr u8 kBossBarDarkenSky   = 0x01;
inline constexpr u8 kBossBarDragonMusic = 0x02;
inline constexpr u8 kBossBarFog         = 0x04;

/// One Boss Bar packet. Only the fields its action carries travel; the others
/// are ignored by the encoder and left at their defaults by the decoder.
struct BossBar {
    Uuid          uuid{};
    BossBarAction action{BossBarAction::Add};
    /// Add, UpdateTitle. A chat component, as JSON.
    std::string title_json;
    /// Add, UpdateHealth. 0 to 1.
    f32 health{0.0F};
    /// Add, UpdateStyle.
    i32 color{0};
    i32 division{0};
    /// Add, UpdateFlags. The kBossBar* bits.
    u8 flags{0};
};

[[nodiscard]] std::vector<u8>        encode_boss_bar(const BossBar& bar);
[[nodiscard]] std::optional<BossBar> parse_boss_bar(std::span<const u8> payload);

// ── World border ────────────────────────────────────────────────────────────

/// Initialize World Border: everything at once, sent on join and on a
/// dimension change.
struct WorldBorderInit {
    f64 x{0.0};
    f64 z{0.0};
    f64 old_diameter{0.0};
    f64 new_diameter{0.0};
    /// Real-time milliseconds until new_diameter is reached; 0 when still.
    /// A VarLong on the wire.
    i64 lerp_ms{0};
    /// Portal destinations are clamped to ±this.
    i32 portal_teleport_boundary{0};
    i32 warning_blocks{0};
    /// Seconds.
    i32 warning_time{0};

    friend bool operator==(const WorldBorderInit&, const WorldBorderInit&) = default;
};

struct BorderCenter {
    f64 x{0.0};
    f64 z{0.0};

    friend bool operator==(const BorderCenter&, const BorderCenter&) = default;
};

/// Set Border Lerp Size: a resize in progress.
struct BorderLerp {
    f64 old_diameter{0.0};
    f64 new_diameter{0.0};
    i64 lerp_ms{0};

    friend bool operator==(const BorderLerp&, const BorderLerp&) = default;
};

[[nodiscard]] std::vector<u8> encode_initialize_world_border(const WorldBorderInit& border);
[[nodiscard]] std::optional<WorldBorderInit> parse_initialize_world_border(
    std::span<const u8> payload);

[[nodiscard]] std::vector<u8>             encode_set_border_center(f64 x, f64 z);
[[nodiscard]] std::optional<BorderCenter> parse_set_border_center(std::span<const u8> payload);

[[nodiscard]] std::vector<u8>           encode_set_border_lerp_size(const BorderLerp& lerp);
[[nodiscard]] std::optional<BorderLerp> parse_set_border_lerp_size(std::span<const u8> payload);

[[nodiscard]] std::vector<u8>    encode_set_border_size(f64 diameter);
[[nodiscard]] std::optional<f64> parse_set_border_size(std::span<const u8> payload);

/// Seconds.
[[nodiscard]] std::vector<u8>    encode_set_border_warning_delay(i32 seconds);
[[nodiscard]] std::optional<i32> parse_set_border_warning_delay(std::span<const u8> payload);

/// Blocks.
[[nodiscard]] std::vector<u8>    encode_set_border_warning_distance(i32 blocks);
[[nodiscard]] std::optional<i32> parse_set_border_warning_distance(std::span<const u8> payload);

// ── Scoreboard ──────────────────────────────────────────────────────────────

/// Display slots 0..18: list, sidebar, below name, then one sidebar per team
/// colour (3 + colour).
inline constexpr i8 kDisplaySlots = 19;

struct DisplayObjective {
    i8          position{0};
    std::string objective;

    friend bool operator==(const DisplayObjective&, const DisplayObjective&) = default;
};

[[nodiscard]] std::vector<u8> encode_display_objective(const DisplayObjective& display);
[[nodiscard]] std::optional<DisplayObjective> parse_display_objective(std::span<const u8> payload);

enum class ObjectiveMode : u8 { Create = 0, Remove = 1, UpdateText = 2 };
enum class ObjectiveRender : u8 { Integer = 0, Hearts = 1 };

/// Update Objectives. The display text and render type travel for Create and
/// UpdateText only.
struct UpdateObjectives {
    std::string     name;
    ObjectiveMode   mode{ObjectiveMode::Create};
    std::string     display_json;
    ObjectiveRender render{ObjectiveRender::Integer};

    friend bool operator==(const UpdateObjectives&, const UpdateObjectives&) = default;
};

[[nodiscard]] std::vector<u8> encode_update_objectives(const UpdateObjectives& objectives);
[[nodiscard]] std::optional<UpdateObjectives> parse_update_objectives(std::span<const u8> payload);

enum class TeamMode : u8 {
    Create         = 0,
    Remove         = 1,
    UpdateInfo     = 2,
    AddEntities    = 3,
    RemoveEntities = 4,
};

/// Team colours 0..21: the sixteen chat colours, then obfuscated, bold,
/// strikethrough, underlined, italic and reset.
inline constexpr i32 kTeamColors = 22;

inline constexpr u8 kTeamFriendlyFire = 0x01;
inline constexpr u8 kTeamSeeInvisible = 0x02;

/// What Create and UpdateInfo carry.
struct TeamInfo {
    std::string display_json;
    u8          friendly_flags{0};
    /// always, hideForOtherTeams, hideForOwnTeam, never.
    std::string name_tag_visibility{"always"};
    /// always, pushOtherTeams, pushOwnTeam, never.
    std::string collision_rule{"always"};
    i32         color{0};
    std::string prefix_json;
    std::string suffix_json;

    friend bool operator==(const TeamInfo&, const TeamInfo&) = default;
};

/// Update Teams. `info` travels for Create and UpdateInfo, `entities` for
/// Create, AddEntities and RemoveEntities — player names, or UUIDs as text.
struct UpdateTeams {
    std::string              team;
    TeamMode                 mode{TeamMode::Create};
    TeamInfo                 info;
    std::vector<std::string> entities;

    friend bool operator==(const UpdateTeams&, const UpdateTeams&) = default;
};

[[nodiscard]] std::vector<u8>            encode_update_teams(const UpdateTeams& teams);
[[nodiscard]] std::optional<UpdateTeams> parse_update_teams(std::span<const u8> payload);

enum class ScoreAction : u8 { Change = 0, Remove = 1 };

/// Update Score. The value travels only when the action is Change.
struct UpdateScore {
    std::string entity;
    ScoreAction action{ScoreAction::Change};
    std::string objective;
    i32         value{0};

    friend bool operator==(const UpdateScore&, const UpdateScore&) = default;
};

[[nodiscard]] std::vector<u8>            encode_update_score(const UpdateScore& score);
[[nodiscard]] std::optional<UpdateScore> parse_update_score(std::span<const u8> payload);

// ── Statistics ──────────────────────────────────────────────────────────────

/// One entry of Award Statistics. `category` indexes the nine stat types
/// (mined, crafted, used, broken, picked up, dropped, killed, killed by,
/// custom); `statistic` is an id in that category's registry — Mojang's
/// number, as every registry id on the wire.
struct Statistic {
    i32 category{0};
    i32 statistic{0};
    i32 value{0};

    friend bool operator==(const Statistic&, const Statistic&) = default;
};

[[nodiscard]] std::vector<u8> encode_award_statistics(std::span<const Statistic> statistics);
[[nodiscard]] std::optional<std::vector<Statistic>> parse_award_statistics(
    std::span<const u8> payload);

// ── Advancement tabs ────────────────────────────────────────────────────────

/// Select Advancements Tab, clientbound: switch the open screen to a tab, or
/// to none.
struct SelectAdvancementsTab {
    std::optional<std::string> tab;

    friend bool operator==(const SelectAdvancementsTab&, const SelectAdvancementsTab&) = default;
};

[[nodiscard]] std::vector<u8> encode_select_advancements_tab(const SelectAdvancementsTab& select);
[[nodiscard]] std::optional<SelectAdvancementsTab> parse_select_advancements_tab(
    std::span<const u8> payload);

enum class SeenAdvancementsAction : u8 { OpenedTab = 0, ClosedScreen = 1 };

/// Seen Advancements, serverbound. The tab travels only with OpenedTab, and
/// without a presence flag: the action is the flag.
struct SeenAdvancements {
    SeenAdvancementsAction action{SeenAdvancementsAction::OpenedTab};
    std::string            tab;

    friend bool operator==(const SeenAdvancements&, const SeenAdvancements&) = default;
};

[[nodiscard]] std::vector<u8> encode_seen_advancements(const SeenAdvancements& seen);
[[nodiscard]] std::optional<SeenAdvancements> parse_seen_advancements(std::span<const u8> payload);

}  // namespace ov::net
