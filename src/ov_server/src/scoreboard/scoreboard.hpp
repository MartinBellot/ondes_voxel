// The scoreboard: objectives, scores, teams, display slots — and the file and
// the packets they live in.
//
// ── What it is, in vanilla's terms ─────────────────────────────────────────
//
// A *holder* is a name: a player's name, a mob's UUID written out, or any
// string at all (`#hidden`, `a,b`). An *objective* has a name, a criterion that
// may feed it (dummy, trigger, deathCount, health… or a statistic), a display
// name and a render type. A *score* is a holder's value on an objective, and a
// lock that only `/trigger` cares about. A *team* is a named set of holders
// with display options that the client applies to their names.
//
// ── What the clients are sent ──────────────────────────────────────────────
//
// Every change is queued as a packet (`take_packets`), in the order the real
// server sends them — the capture of scripts/capture_scoreboard.py pins it:
//
//   * teams always travel: Create on `add` (followed by an Update, because
//     `team add` sets the display name after creating), Update on any option,
//     AddEntities / RemoveEntities on join and leave, Remove on remove;
//   * an objective only travels while it is **displayed** in some slot. The
//     first slot that shows it sends Update Objectives Create, a Display
//     Objective per slot, and every score sorted by value then by holder
//     (reverse, ignoring case); the last slot to let it go sends Remove;
//   * a score on a displayed objective travels on every change — and on its
//     creation, at 0, before whatever value it is then given;
//   * a holder whose last score goes is sent as a removal with an empty
//     objective name: `reset fake d` on fake's only score says "everything".
//
// ── The API other systems use ──────────────────────────────────────────────
//
// `score` / `set_score` / `get_or_create` / `reset` are the reads and writes
// `/execute if score` and `/execute store … score` need. `on_death`,
// `on_kill`, `update_player` feed the built-in criteria; `on_stat` is the hook
// a statistics system calls with a stat's new total — the stat criteria
// (`minecraft.mined:minecraft.stone`) take that total as their score, as
// vanilla's do.
//
// Single writer: the tick thread. No lock, like every other piece of world
// state. Not a public header — the server and the tests include it by path.
#pragma once

#include "java_hash_order.hpp"

#include "ov/base/types.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/scoreboard_packets.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ov::registry {
class Registries;
}

namespace ov::server {

/// scoreboard.dat's DataVersion, 1.20.1's.
inline constexpr i32 kScoreboardDataVersion = 3465;

/// ChatFormatting's index for "no colour". The sixteen colours are 0…15.
inline constexpr u8    kColorReset = 21;
inline constexpr usize kColorCount = 16;

/// What an empty prefix, suffix or display name is, as vanilla writes it.
inline constexpr std::string_view kEmptyComponent = R"({"text":""})";

/// "black" … "white" for 0…15, "reset" for 21.
[[nodiscard]] std::string_view color_name(u8 color) noexcept;
/// The inverse, "reset" included; nullopt for a formatting code or a typo.
[[nodiscard]] std::optional<u8> color_from_name(std::string_view name) noexcept;

/// "list", "sidebar", "belowName", "sidebar.team.<colour>" ↔ 0…18.
[[nodiscard]] std::optional<u8> display_slot_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string       display_slot_name(u8 slot);

enum class RenderType : u8 { Integer = 0, Hearts = 1 };
[[nodiscard]] std::string_view          render_type_name(RenderType type) noexcept;
[[nodiscard]] std::optional<RenderType> render_type_from_name(std::string_view name) noexcept;

enum class CriterionKind : u8 {
    Dummy,
    Trigger,
    DeathCount,
    PlayerKillCount,
    TotalKillCount,
    Health,
    Food,
    Air,
    Armor,
    Xp,
    Level,
    TeamKill,
    KilledByTeam,
    Stat,
};

struct Criterion {
    CriterionKind kind{CriterionKind::Dummy};
    /// TeamKill and KilledByTeam: the colour.
    u8            color{0};
    /// The name the file and the commands use: "teamkill.red",
    /// "minecraft.mined:minecraft.stone".
    std::string   name{"dummy"};
    /// Stat: "minecraft:mined" and "minecraft:stone".
    std::string   stat_type;
    std::string   stat_value;

    /// health, food, air, armor, xp and level are the game's, not a command's.
    [[nodiscard]] bool       read_only() const noexcept;
    /// Hearts for health, integer for everything else.
    [[nodiscard]] RenderType default_render() const noexcept;
};

/// A criterion by name, as `scoreboard objectives add` reads it. With the
/// registries, a statistic must name a stat type and a member of its registry
/// (`minecraft.mined:minecraft.nosuch` is refused, as the capture shows);
/// without them, the shape is enough. A statistic comes back under its
/// canonical name, namespaces written out.
[[nodiscard]] std::optional<Criterion> parse_criterion(std::string_view               name,
                                                       const registry::Registries* registries);

enum class Visibility : u8 { Always, Never, HideForOtherTeams, HideForOwnTeam };
enum class CollisionRule : u8 { Always, Never, PushOtherTeams, PushOwnTeam };

/// "always", "never", "hideForOtherTeams", "hideForOwnTeam".
[[nodiscard]] std::string_view          visibility_name(Visibility visibility) noexcept;
[[nodiscard]] std::optional<Visibility> visibility_from_name(std::string_view name) noexcept;
/// "always", "never", "pushOtherTeams", "pushOwnTeam".
[[nodiscard]] std::string_view             collision_rule_name(CollisionRule rule) noexcept;
[[nodiscard]] std::optional<CollisionRule> collision_rule_from_name(std::string_view name) noexcept;

struct Objective {
    std::string name;
    Criterion   criterion;
    /// A JSON component.
    std::string display_json;
    RenderType  render{RenderType::Integer};
};

struct Score {
    i32  value{0};
    /// A new score is locked: `/trigger` needs `scoreboard players enable`.
    bool locked{true};

    friend bool operator==(const Score&, const Score&) = default;
};

class Team {
public:
    std::string   name;
    std::string   display_json;
    u8            color{kColorReset};
    std::string   prefix_json{kEmptyComponent};
    std::string   suffix_json{kEmptyComponent};
    bool          friendly_fire{true};
    bool          see_friendly_invisibles{true};
    Visibility    name_tag{Visibility::Always};
    Visibility    death_message{Visibility::Always};
    CollisionRule collision{CollisionRule::Always};

    /// In vanilla's HashSet order.
    [[nodiscard]] std::vector<std::string> members() const;
    [[nodiscard]] usize member_count() const noexcept { return members_.size(); }
    [[nodiscard]] bool  has_member(std::string_view holder) const noexcept;

    /// The options as Update Teams carries them.
    [[nodiscard]] net::scoreboard::TeamParameters parameters() const;

private:
    friend class Scoreboard;
    struct Member {
        std::string name;
        u64         sequence{0};
    };
    std::vector<Member> members_;
    JavaHashOrder       order_;
};

/// What a player's own state feeds: health (with absorption, rounded up),
/// food, air, armour points, total experience points, level.
struct PlayerCriteria {
    i32 health{0};
    i32 food{0};
    i32 air{0};
    i32 armor{0};
    i32 xp{0};
    i32 level{0};

    friend bool operator==(const PlayerCriteria&, const PlayerCriteria&) = default;
};

struct ScoreboardPacket {
    i32             id{0};
    std::vector<u8> payload;
};

class Scoreboard {
public:
    Scoreboard() = default;
    Scoreboard(Scoreboard&&) noexcept            = default;
    Scoreboard& operator=(Scoreboard&&) noexcept = default;
    Scoreboard(const Scoreboard&)                = delete;
    Scoreboard& operator=(const Scoreboard&)     = delete;
    ~Scoreboard()                                = default;

    // ── Objectives ─────────────────────────────────────────────────────────

    [[nodiscard]] const Objective* objective(std::string_view name) const noexcept;
    /// Null when the name is taken.
    const Objective* add_objective(std::string name, Criterion criterion, std::string display_json,
                                   RenderType render);
    bool remove_objective(std::string_view name);
    /// In vanilla's HashMap order — `scoreboard objectives list`'s.
    [[nodiscard]] std::vector<const Objective*> objectives() const;
    void set_display_name(std::string_view objective, std::string display_json);
    void set_render_type(std::string_view objective, RenderType render);

    [[nodiscard]] const Objective* displayed(u8 slot) const noexcept;
    /// An empty name clears the slot.
    void set_displayed(u8 slot, std::string_view objective);
    /// Shown in at least one slot, hence known to the clients.
    [[nodiscard]] bool tracked(const Objective& objective) const noexcept;

    // ── Scores ─────────────────────────────────────────────────────────────

    [[nodiscard]] std::optional<Score> score(std::string_view holder,
                                             std::string_view objective) const;
    /// The value, the score created at 0 first if it did not exist — what
    /// `operation` does to a source that has none. Nullopt: no such objective.
    std::optional<i32> get_or_create(std::string_view holder, std::string_view objective);
    /// False: no such objective.
    bool set_score(std::string_view holder, std::string_view objective, i32 value);
    bool set_locked(std::string_view holder, std::string_view objective, bool locked);
    /// An empty objective resets every score of the holder. True when
    /// something was removed.
    bool reset(std::string_view holder, std::string_view objective = {});
    /// Every holder with a score, in vanilla's HashMap order — what `*` names.
    [[nodiscard]] std::vector<std::string> holders() const;
    /// A holder's scores. Vanilla walks an identity-hashed map here, whose
    /// order no other process can reproduce; this is the order they were
    /// set in.
    [[nodiscard]] std::vector<std::pair<const Objective*, Score>> scores_of(
        std::string_view holder) const;

    // ── Criteria ───────────────────────────────────────────────────────────

    void on_death(std::string_view holder);
    /// totalKillCount, playerKillCount for a player victim, teamkill.<colour>
    /// of the victim's team for the killer, killedByTeam.<colour> of the
    /// killer's team for the victim. Nothing for a self-kill.
    void on_kill(std::string_view killer, std::string_view victim, bool victim_is_player);
    /// Once per tick per online player. The first call after a player joins
    /// sets every criterion; after that, only what changed.
    void update_player(std::string_view holder, const PlayerCriteria& now);
    void forget_player(std::string_view holder);
    /// The statistics hook: a stat's new total for a holder.
    void on_stat(std::string_view holder, std::string_view stat_type, std::string_view stat_value,
                 i32 total);

    // ── Teams ──────────────────────────────────────────────────────────────

    [[nodiscard]] const Team* team(std::string_view name) const noexcept;
    /// For editing an option; call `team_changed` after.
    [[nodiscard]] Team* edit_team(std::string_view name) noexcept;
    /// Null when the name is taken.
    const Team* add_team(std::string name, std::string display_json);
    bool        remove_team(std::string_view name);
    /// In vanilla's HashMap order.
    [[nodiscard]] std::vector<const Team*> teams() const;
    [[nodiscard]] const Team*              team_of(std::string_view holder) const noexcept;
    /// Leaves any team first — the same one included, as vanilla does.
    void join(std::string_view team, std::string_view holder);
    /// True when the holder was on a team.
    bool leave(std::string_view holder);
    /// Removes every member; the count.
    usize empty_team(std::string_view team);
    void  team_changed(std::string_view team);

    /// Friendly fire: may `attacker` hurt `victim`?
    [[nodiscard]] static bool can_hurt(const Team* attacker, const Team* victim) noexcept;
    /// The collision rule of both sides: may the two push each other?
    [[nodiscard]] static bool can_push(const Team* a, const Team* b) noexcept;

    // ── Packets ────────────────────────────────────────────────────────────

    [[nodiscard]] bool has_packets() const noexcept { return !packets_.empty(); }
    [[nodiscard]] std::vector<ScoreboardPacket> take_packets() noexcept { return std::exchange(packets_, {}); }
    /// What a player who arrives is sent: every team, then every displayed
    /// objective with its slots and scores.
    void arrival_packets(std::vector<ScoreboardPacket>& out) const;

    // ── Persistence ────────────────────────────────────────────────────────

    /// The file's root: {data: {Objectives, PlayerScores, Teams,
    /// DisplaySlots}, DataVersion}. Keys this server does not model — at the
    /// root, in `data`, or an objective whose criterion it does not know,
    /// with its scores — go back out as they came in.
    [[nodiscard]] nbt::Tag save() const;
    [[nodiscard]] static std::expected<Scoreboard, std::string> load(
        const nbt::Tag& root, const registry::Registries* registries);

    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void               mark_clean() noexcept { dirty_ = false; }

private:
    struct ObjectiveEntry {
        Objective objective;
        u64       sequence{0};
        /// Creation order, which is the order a criterion feeds its
        /// objectives in (vanilla keeps a list per criterion).
        u64       created{0};
    };
    struct Holder {
        std::string                                  name;
        u64                                          sequence{0};
        std::vector<std::pair<const Objective*, Score>> scores;
    };
    struct TeamEntry {
        Team team;
        u64  sequence{0};
    };

    [[nodiscard]] ObjectiveEntry*       find_objective(std::string_view name) noexcept;
    [[nodiscard]] const ObjectiveEntry* find_objective(std::string_view name) const noexcept;
    [[nodiscard]] Holder*               find_holder(std::string_view name) noexcept;
    [[nodiscard]] const Holder*         find_holder(std::string_view name) const noexcept;
    [[nodiscard]] TeamEntry*            find_team(std::string_view name) noexcept;
    [[nodiscard]] const TeamEntry*      find_team(std::string_view name) const noexcept;

    Holder& holder(std::string_view name);
    Score&  score_or_create(Holder& holder, const Objective& objective);
    void    change(Holder& holder, const Objective& objective, Score& score, i32 value);
    void    remove_holder(std::string_view name);
    void    increment(CriterionKind kind, u8 color, std::string_view holder);
    void    set_all(CriterionKind kind, std::string_view holder, i32 value);

    void emit(i32 id, std::vector<u8> payload);
    void start_tracking(const Objective& objective, std::vector<ScoreboardPacket>& out) const;
    void send_score(const Holder& holder, const Objective& objective, const Score& score);

    std::vector<std::unique_ptr<ObjectiveEntry>> objectives_;
    JavaHashOrder                                objective_order_;
    u64                                          objectives_created_{0};
    std::unordered_map<std::string, std::unique_ptr<Holder>> holders_;
    JavaHashOrder                                holder_order_;
    std::vector<std::unique_ptr<TeamEntry>>      teams_;
    JavaHashOrder                                team_order_;
    std::unordered_map<std::string, std::string> team_by_holder_;
    std::array<const Objective*, net::scoreboard::kSlotCount> display_{};
    std::unordered_map<std::string, PlayerCriteria> last_seen_;

    // Kept as read, written back as read: see `save`.
    std::vector<nbt::CompoundEntry> extra_root_;
    std::vector<nbt::CompoundEntry> extra_data_;
    std::vector<nbt::Tag>           unknown_objectives_;
    std::vector<nbt::Tag>           unknown_scores_;

    std::vector<ScoreboardPacket> packets_;
    bool                          dirty_{false};
};

/// world/data/scoreboard.dat, gzip. A missing file is an empty scoreboard; a
/// file that is not gzip or not NBT, or from a newer DataVersion, is refused
/// with its path and reason — and must then not be overwritten.
[[nodiscard]] std::expected<Scoreboard, std::string> read_scoreboard_file(
    const std::filesystem::path& path, const registry::Registries* registries);
[[nodiscard]] bool write_scoreboard_file(const std::filesystem::path& path,
                                         const Scoreboard&            scoreboard);

}  // namespace ov::server
