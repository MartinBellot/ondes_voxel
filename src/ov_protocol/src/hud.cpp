#include "ov/protocol/hud.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/varint.hpp"

#include <utility>

namespace ov::net {
namespace {

/// Name Tag Visibility and Collision Rule are String Enum (40).
constexpr u32 kMaxTeamRuleLength = 40;

[[nodiscard]] std::optional<bool> read_bool(io::ByteReader& reader) {
    const auto value = reader.read_u8();
    if (!value || *value > 1) {
        return std::nullopt;
    }
    return *value == 1;
}

void write_bool(io::ByteWriter& writer, bool value) {
    writer.write_u8(value ? 1 : 0);
}

[[nodiscard]] std::optional<std::string> read_text(io::ByteReader& reader,
                                                   u32             max_length = kMaxStringLength) {
    auto text = read_string(reader, max_length);
    if (!text) {
        return std::nullopt;
    }
    return std::move(*text);
}

/// A chat component, as JSON text.
[[nodiscard]] std::optional<std::string> read_component(io::ByteReader& reader) {
    return read_text(reader, kMaxChatComponentLength);
}

/// A VarInt enum in [0, bound). The client reads these as ordinals and refuses
/// any other value, so the server's reader does too.
[[nodiscard]] std::optional<i32> read_enum(io::ByteReader& reader, i32 bound) {
    const auto value = read_varint(reader);
    if (!value || *value < 0 || *value >= bound) {
        return std::nullopt;
    }
    return *value;
}

/// A byte enum in [0, bound).
[[nodiscard]] std::optional<u8> read_byte_enum(io::ByteReader& reader, u8 bound) {
    const auto value = reader.read_u8();
    if (!value || *value >= bound) {
        return std::nullopt;
    }
    return *value;
}

/// An element count, each element at least `min_size` bytes. A count the bytes
/// left cannot hold is a lie, and is refused before anything is reserved for it.
[[nodiscard]] std::optional<usize> read_count(io::ByteReader& reader, usize min_size) {
    const auto count = read_varint(reader);
    if (!count || *count < 0) {
        return std::nullopt;
    }
    const auto wanted = static_cast<usize>(*count);
    if (wanted > reader.remaining() / min_size) {
        return std::nullopt;
    }
    return wanted;
}

/// The value, if nothing follows it. Trailing bytes mean a field was misread.
template<typename T>
[[nodiscard]] std::optional<T> finish(const io::ByteReader& reader, T value) {
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return std::optional<T>{std::move(value)};
}

[[nodiscard]] bool read_style(io::ByteReader& reader, BossBar& bar) {
    const auto color    = read_enum(reader, kBossBarColors);
    const auto division = color ? read_enum(reader, kBossBarDivisions) : std::nullopt;
    if (!color || !division) {
        return false;
    }
    bar.color    = *color;
    bar.division = *division;
    return true;
}

[[nodiscard]] bool read_title(io::ByteReader& reader, BossBar& bar) {
    auto title = read_component(reader);
    if (!title) {
        return false;
    }
    bar.title_json = std::move(*title);
    return true;
}

[[nodiscard]] bool read_health(io::ByteReader& reader, BossBar& bar) {
    const auto health = reader.read_f32();
    if (!health) {
        return false;
    }
    bar.health = *health;
    return true;
}

[[nodiscard]] bool read_flags(io::ByteReader& reader, BossBar& bar) {
    const auto flags = reader.read_u8();
    if (!flags) {
        return false;
    }
    bar.flags = *flags;
    return true;
}

void write_team_info(io::ByteWriter& writer, const TeamInfo& info) {
    write_string(writer, info.display_json);
    writer.write_u8(info.friendly_flags);
    write_string(writer, info.name_tag_visibility);
    write_string(writer, info.collision_rule);
    write_varint(writer, info.color);
    write_string(writer, info.prefix_json);
    write_string(writer, info.suffix_json);
}

[[nodiscard]] bool read_team_info(io::ByteReader& reader, TeamInfo& info) {
    auto display = read_component(reader);
    if (!display) {
        return false;
    }
    const auto friendly = reader.read_u8();
    if (!friendly) {
        return false;
    }
    auto       visibility = read_text(reader, kMaxTeamRuleLength);
    auto       collision  = visibility ? read_text(reader, kMaxTeamRuleLength) : std::nullopt;
    const auto color      = collision ? read_enum(reader, kTeamColors) : std::nullopt;
    auto       prefix     = color ? read_component(reader) : std::nullopt;
    auto       suffix     = prefix ? read_component(reader) : std::nullopt;
    if (!suffix) {
        return false;
    }
    info.display_json        = std::move(*display);
    info.friendly_flags      = *friendly;
    info.name_tag_visibility = std::move(*visibility);
    info.collision_rule      = std::move(*collision);
    info.color               = *color;
    info.prefix_json         = std::move(*prefix);
    info.suffix_json         = std::move(*suffix);
    return true;
}

void write_entities(io::ByteWriter& writer, const std::vector<std::string>& entities) {
    write_varint(writer, static_cast<i32>(entities.size()));
    for (const auto& entity : entities) {
        write_string(writer, entity);
    }
}

[[nodiscard]] bool read_entities(io::ByteReader& reader, std::vector<std::string>& entities) {
    // Each name is at least its one-byte length prefix.
    const auto count = read_count(reader, 1);
    if (!count) {
        return false;
    }
    entities.clear();
    entities.reserve(*count);
    for (usize i = 0; i < *count; ++i) {
        auto entity = read_text(reader);
        if (!entity) {
            return false;
        }
        entities.push_back(std::move(*entity));
    }
    return true;
}

}  // namespace

// ── Boss Bar ────────────────────────────────────────────────────────────────

std::vector<u8> encode_boss_bar(const BossBar& bar) {
    io::ByteWriter writer;
    write_uuid(writer, bar.uuid);
    write_varint(writer, static_cast<i32>(bar.action));
    switch (bar.action) {
        case BossBarAction::Add:
            write_string(writer, bar.title_json);
            writer.write_f32(bar.health);
            write_varint(writer, bar.color);
            write_varint(writer, bar.division);
            writer.write_u8(bar.flags);
            break;
        case BossBarAction::Remove: break;
        case BossBarAction::UpdateHealth: writer.write_f32(bar.health); break;
        case BossBarAction::UpdateTitle: write_string(writer, bar.title_json); break;
        case BossBarAction::UpdateStyle:
            write_varint(writer, bar.color);
            write_varint(writer, bar.division);
            break;
        case BossBarAction::UpdateFlags: writer.write_u8(bar.flags); break;
    }
    return writer.take();
}

std::optional<BossBar> parse_boss_bar(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     uuid   = read_uuid(reader);
    const auto     action = uuid ? read_enum(reader, 6) : std::nullopt;
    if (!action) {
        return std::nullopt;
    }
    BossBar bar;
    bar.uuid   = *uuid;
    bar.action = static_cast<BossBarAction>(*action);

    bool ok = true;
    switch (bar.action) {
        case BossBarAction::Add:
            ok = read_title(reader, bar) && read_health(reader, bar) && read_style(reader, bar) &&
                 read_flags(reader, bar);
            break;
        case BossBarAction::Remove: break;
        case BossBarAction::UpdateHealth: ok = read_health(reader, bar); break;
        case BossBarAction::UpdateTitle: ok = read_title(reader, bar); break;
        case BossBarAction::UpdateStyle: ok = read_style(reader, bar); break;
        case BossBarAction::UpdateFlags: ok = read_flags(reader, bar); break;
    }
    if (!ok) {
        return std::nullopt;
    }
    return finish(reader, std::move(bar));
}

// ── World border ────────────────────────────────────────────────────────────

std::vector<u8> encode_initialize_world_border(const WorldBorderInit& border) {
    io::ByteWriter writer;
    writer.write_f64(border.x);
    writer.write_f64(border.z);
    writer.write_f64(border.old_diameter);
    writer.write_f64(border.new_diameter);
    write_varlong(writer, border.lerp_ms);
    write_varint(writer, border.portal_teleport_boundary);
    write_varint(writer, border.warning_blocks);
    write_varint(writer, border.warning_time);
    return writer.take();
}

std::optional<WorldBorderInit> parse_initialize_world_border(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     x        = reader.read_f64();
    const auto     z        = reader.read_f64();
    const auto     old_size = reader.read_f64();
    const auto     new_size = reader.read_f64();
    if (!x || !z || !old_size || !new_size) {
        return std::nullopt;
    }
    // Read in order; after a failure the later reads fail too, and the packet
    // is refused whichever field ran out.
    const auto lerp     = read_varlong(reader);
    const auto boundary = read_varint(reader);
    const auto blocks   = read_varint(reader);
    const auto time     = read_varint(reader);
    if (!lerp || !boundary || !blocks || !time) {
        return std::nullopt;
    }
    WorldBorderInit border;
    border.x                        = *x;
    border.z                        = *z;
    border.old_diameter             = *old_size;
    border.new_diameter             = *new_size;
    border.lerp_ms                  = *lerp;
    border.portal_teleport_boundary = *boundary;
    border.warning_blocks           = *blocks;
    border.warning_time             = *time;
    return finish(reader, border);
}

std::vector<u8> encode_set_border_center(f64 x, f64 z) {
    io::ByteWriter writer;
    writer.write_f64(x);
    writer.write_f64(z);
    return writer.take();
}

std::optional<BorderCenter> parse_set_border_center(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     x = reader.read_f64();
    const auto     z = reader.read_f64();
    if (!x || !z) {
        return std::nullopt;
    }
    return finish(reader, BorderCenter{*x, *z});
}

std::vector<u8> encode_set_border_lerp_size(const BorderLerp& lerp) {
    io::ByteWriter writer;
    writer.write_f64(lerp.old_diameter);
    writer.write_f64(lerp.new_diameter);
    write_varlong(writer, lerp.lerp_ms);
    return writer.take();
}

std::optional<BorderLerp> parse_set_border_lerp_size(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     old_size = reader.read_f64();
    const auto     new_size = reader.read_f64();
    if (!old_size || !new_size) {
        return std::nullopt;
    }
    const auto lerp = read_varlong(reader);
    if (!lerp) {
        return std::nullopt;
    }
    return finish(reader, BorderLerp{*old_size, *new_size, *lerp});
}

std::vector<u8> encode_set_border_size(f64 diameter) {
    io::ByteWriter writer;
    writer.write_f64(diameter);
    return writer.take();
}

std::optional<f64> parse_set_border_size(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     diameter = reader.read_f64();
    if (!diameter) {
        return std::nullopt;
    }
    return finish(reader, *diameter);
}

std::vector<u8> encode_set_border_warning_delay(i32 seconds) {
    io::ByteWriter writer;
    write_varint(writer, seconds);
    return writer.take();
}

std::optional<i32> parse_set_border_warning_delay(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     seconds = read_varint(reader);
    if (!seconds) {
        return std::nullopt;
    }
    return finish(reader, *seconds);
}

std::vector<u8> encode_set_border_warning_distance(i32 blocks) {
    io::ByteWriter writer;
    write_varint(writer, blocks);
    return writer.take();
}

std::optional<i32> parse_set_border_warning_distance(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     blocks = read_varint(reader);
    if (!blocks) {
        return std::nullopt;
    }
    return finish(reader, *blocks);
}

// ── Scoreboard ──────────────────────────────────────────────────────────────

std::vector<u8> encode_display_objective(const DisplayObjective& display) {
    io::ByteWriter writer;
    writer.write_i8(display.position);
    write_string(writer, display.objective);
    return writer.take();
}

std::optional<DisplayObjective> parse_display_objective(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    // A signed Byte on the wire; reading it unsigned folds the negatives into
    // the out-of-range values.
    const auto position = read_byte_enum(reader, static_cast<u8>(kDisplaySlots));
    auto       name     = position ? read_text(reader) : std::nullopt;
    if (!name) {
        return std::nullopt;
    }
    return finish(reader, DisplayObjective{static_cast<i8>(*position), std::move(*name)});
}

std::vector<u8> encode_update_objectives(const UpdateObjectives& objectives) {
    io::ByteWriter writer;
    write_string(writer, objectives.name);
    writer.write_u8(static_cast<u8>(objectives.mode));
    if (objectives.mode != ObjectiveMode::Remove) {
        write_string(writer, objectives.display_json);
        write_varint(writer, static_cast<i32>(objectives.render));
    }
    return writer.take();
}

std::optional<UpdateObjectives> parse_update_objectives(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           name = read_text(reader);
    const auto     mode = name ? read_byte_enum(reader, 3) : std::nullopt;
    if (!mode) {
        return std::nullopt;
    }
    UpdateObjectives objectives;
    objectives.name = std::move(*name);
    objectives.mode = static_cast<ObjectiveMode>(*mode);
    if (objectives.mode != ObjectiveMode::Remove) {
        auto       display = read_component(reader);
        const auto render  = display ? read_enum(reader, 2) : std::nullopt;
        if (!render) {
            return std::nullopt;
        }
        objectives.display_json = std::move(*display);
        objectives.render       = static_cast<ObjectiveRender>(*render);
    }
    return finish(reader, std::move(objectives));
}

std::vector<u8> encode_update_teams(const UpdateTeams& teams) {
    io::ByteWriter writer;
    write_string(writer, teams.team);
    writer.write_u8(static_cast<u8>(teams.mode));
    if (teams.mode == TeamMode::Create || teams.mode == TeamMode::UpdateInfo) {
        write_team_info(writer, teams.info);
    }
    if (teams.mode == TeamMode::Create || teams.mode == TeamMode::AddEntities ||
        teams.mode == TeamMode::RemoveEntities) {
        write_entities(writer, teams.entities);
    }
    return writer.take();
}

std::optional<UpdateTeams> parse_update_teams(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           name = read_text(reader);
    const auto     mode = name ? read_byte_enum(reader, 5) : std::nullopt;
    if (!mode) {
        return std::nullopt;
    }
    UpdateTeams teams;
    teams.team = std::move(*name);
    teams.mode = static_cast<TeamMode>(*mode);
    if (teams.mode == TeamMode::Create || teams.mode == TeamMode::UpdateInfo) {
        if (!read_team_info(reader, teams.info)) {
            return std::nullopt;
        }
    }
    if (teams.mode == TeamMode::Create || teams.mode == TeamMode::AddEntities ||
        teams.mode == TeamMode::RemoveEntities) {
        if (!read_entities(reader, teams.entities)) {
            return std::nullopt;
        }
    }
    return finish(reader, std::move(teams));
}

std::vector<u8> encode_update_score(const UpdateScore& score) {
    io::ByteWriter writer;
    write_string(writer, score.entity);
    write_varint(writer, static_cast<i32>(score.action));
    write_string(writer, score.objective);
    if (score.action == ScoreAction::Change) {
        write_varint(writer, score.value);
    }
    return writer.take();
}

std::optional<UpdateScore> parse_update_score(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           entity    = read_text(reader);
    const auto     action    = entity ? read_enum(reader, 2) : std::nullopt;
    auto           objective = action ? read_text(reader) : std::nullopt;
    if (!objective) {
        return std::nullopt;
    }
    UpdateScore score;
    score.entity    = std::move(*entity);
    score.action    = static_cast<ScoreAction>(*action);
    score.objective = std::move(*objective);
    if (score.action == ScoreAction::Change) {
        const auto value = read_varint(reader);
        if (!value) {
            return std::nullopt;
        }
        score.value = *value;
    }
    return finish(reader, std::move(score));
}

// ── Statistics ──────────────────────────────────────────────────────────────

std::vector<u8> encode_award_statistics(std::span<const Statistic> statistics) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(statistics.size()));
    for (const auto& stat : statistics) {
        write_varint(writer, stat.category);
        write_varint(writer, stat.statistic);
        write_varint(writer, stat.value);
    }
    return writer.take();
}

std::optional<std::vector<Statistic>> parse_award_statistics(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    // Three VarInts per entry, each at least one byte.
    const auto count = read_count(reader, 3);
    if (!count) {
        return std::nullopt;
    }
    std::vector<Statistic> statistics;
    statistics.reserve(*count);
    for (usize i = 0; i < *count; ++i) {
        const auto category  = read_varint(reader);
        const auto statistic = category ? read_varint(reader) : category;
        const auto value     = statistic ? read_varint(reader) : statistic;
        if (!value) {
            return std::nullopt;
        }
        statistics.push_back(Statistic{*category, *statistic, *value});
    }
    return finish(reader, std::move(statistics));
}

// ── Advancement tabs ────────────────────────────────────────────────────────

std::vector<u8> encode_select_advancements_tab(const SelectAdvancementsTab& select) {
    io::ByteWriter writer;
    write_bool(writer, select.tab.has_value());
    if (select.tab) {
        write_string(writer, *select.tab);
    }
    return writer.take();
}

std::optional<SelectAdvancementsTab> parse_select_advancements_tab(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     present = read_bool(reader);
    if (!present) {
        return std::nullopt;
    }
    SelectAdvancementsTab select;
    if (*present) {
        auto tab = read_text(reader);
        if (!tab) {
            return std::nullopt;
        }
        select.tab = std::move(*tab);
    }
    return finish(reader, std::move(select));
}

std::vector<u8> encode_seen_advancements(const SeenAdvancements& seen) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(seen.action));
    if (seen.action == SeenAdvancementsAction::OpenedTab) {
        write_string(writer, seen.tab);
    }
    return writer.take();
}

std::optional<SeenAdvancements> parse_seen_advancements(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     action = read_enum(reader, 2);
    if (!action) {
        return std::nullopt;
    }
    SeenAdvancements seen;
    seen.action = static_cast<SeenAdvancementsAction>(*action);
    if (seen.action == SeenAdvancementsAction::OpenedTab) {
        auto tab = read_text(reader);
        if (!tab) {
            return std::nullopt;
        }
        seen.tab = std::move(*tab);
    }
    return finish(reader, std::move(seen));
}

}  // namespace ov::net
