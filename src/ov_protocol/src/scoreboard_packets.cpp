#include "ov/protocol/scoreboard_packets.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net::scoreboard {
namespace {

// A chat component on the wire is a string of at most 262144 characters; a
// name, 32767 — the protocol page's defaults for Chat and String.
constexpr u32 kMaxComponent = 262144;
constexpr u32 kMaxName      = 32767;

void write_parameters(io::ByteWriter& writer, const TeamParameters& p) {
    write_string(writer, p.display_json);
    writer.write_u8(p.flags);
    write_string(writer, p.nametag_visibility);
    write_string(writer, p.collision_rule);
    write_varint(writer, p.color);
    write_string(writer, p.prefix_json);
    write_string(writer, p.suffix_json);
}

void write_entities(io::ByteWriter& writer, const std::vector<std::string>& entities) {
    write_varint(writer, static_cast<i32>(entities.size()));
    for (const std::string& entity : entities) {
        write_string(writer, entity);
    }
}

[[nodiscard]] bool read_text(io::ByteReader& reader, std::string& out, u32 max) {
    auto value = read_string(reader, max);
    if (!value) {
        return false;
    }
    out = std::move(*value);
    return true;
}

[[nodiscard]] bool read_int(io::ByteReader& reader, i32& out) {
    const auto value = read_varint(reader);
    if (!value) {
        return false;
    }
    out = *value;
    return true;
}

[[nodiscard]] bool read_byte(io::ByteReader& reader, u8& out) {
    const auto value = reader.read_u8();
    if (!value) {
        return false;
    }
    out = *value;
    return true;
}

}  // namespace

std::vector<u8> encode_display_objective(const DisplayObjective& packet) {
    io::ByteWriter writer;
    writer.write_u8(packet.slot);
    write_string(writer, packet.objective);
    return writer.take();
}

std::vector<u8> encode_update_objectives(const ObjectiveUpdate& packet) {
    io::ByteWriter writer;
    write_string(writer, packet.name);
    writer.write_u8(static_cast<u8>(packet.mode));
    if (packet.mode != ObjectiveMode::Remove) {
        write_string(writer, packet.display_json);
        write_varint(writer, packet.render_type);
    }
    return writer.take();
}

std::vector<u8> encode_update_teams(const TeamUpdate& packet) {
    io::ByteWriter writer;
    write_string(writer, packet.name);
    writer.write_u8(static_cast<u8>(packet.mode));
    switch (packet.mode) {
    case TeamMode::Create:
        write_parameters(writer, packet.parameters);
        write_entities(writer, packet.entities);
        break;
    case TeamMode::Update:
        write_parameters(writer, packet.parameters);
        break;
    case TeamMode::AddEntities:
    case TeamMode::RemoveEntities:
        write_entities(writer, packet.entities);
        break;
    case TeamMode::Remove:
        break;
    }
    return writer.take();
}

std::vector<u8> encode_update_score(const ScoreUpdate& packet) {
    io::ByteWriter writer;
    write_string(writer, packet.holder);
    write_varint(writer, static_cast<i32>(packet.action));
    write_string(writer, packet.objective);
    if (packet.action == ScoreAction::Change) {
        write_varint(writer, packet.value);
    }
    return writer.take();
}

std::optional<DisplayObjective> parse_display_objective(std::span<const u8> payload) {
    io::ByteReader   reader{payload};
    DisplayObjective out;
    if (!read_byte(reader, out.slot) || !read_text(reader, out.objective, kMaxName) ||
        reader.remaining() != 0) {
        return std::nullopt;
    }
    return out;
}

std::optional<ObjectiveUpdate> parse_update_objectives(std::span<const u8> payload) {
    io::ByteReader  reader{payload};
    ObjectiveUpdate out;
    u8              mode = 0;
    if (!read_text(reader, out.name, kMaxName) || !read_byte(reader, mode) || mode > 2) {
        return std::nullopt;
    }
    out.mode = static_cast<ObjectiveMode>(mode);
    if (out.mode != ObjectiveMode::Remove &&
        (!read_text(reader, out.display_json, kMaxComponent) || !read_int(reader, out.render_type))) {
        return std::nullopt;
    }
    if (reader.remaining() != 0) {
        return std::nullopt;
    }
    return out;
}

std::optional<TeamUpdate> parse_update_teams(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    TeamUpdate     out;
    u8             mode = 0;
    if (!read_text(reader, out.name, kMaxName) || !read_byte(reader, mode) || mode > 4) {
        return std::nullopt;
    }
    out.mode = static_cast<TeamMode>(mode);
    if (out.mode == TeamMode::Create || out.mode == TeamMode::Update) {
        TeamParameters& p = out.parameters;
        if (!read_text(reader, p.display_json, kMaxComponent) || !read_byte(reader, p.flags) ||
            !read_text(reader, p.nametag_visibility, 40) || !read_text(reader, p.collision_rule, 40) ||
            !read_int(reader, p.color) || !read_text(reader, p.prefix_json, kMaxComponent) ||
            !read_text(reader, p.suffix_json, kMaxComponent)) {
            return std::nullopt;
        }
    }
    if (out.mode == TeamMode::Create || out.mode == TeamMode::AddEntities ||
        out.mode == TeamMode::RemoveEntities) {
        i32 count = 0;
        // Each entry takes at least one byte: a count beyond what is left is a
        // lie, refused before anything is reserved for it.
        if (!read_int(reader, count) || count < 0 || static_cast<usize>(count) > reader.remaining()) {
            return std::nullopt;
        }
        out.entities.resize(static_cast<usize>(count));
        for (std::string& entity : out.entities) {
            if (!read_text(reader, entity, kMaxName)) {
                return std::nullopt;
            }
        }
    }
    if (reader.remaining() != 0) {
        return std::nullopt;
    }
    return out;
}

std::optional<ScoreUpdate> parse_update_score(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    ScoreUpdate    out;
    i32            action = 0;
    if (!read_text(reader, out.holder, kMaxName) || !read_int(reader, action) || action < 0 ||
        action > 1 || !read_text(reader, out.objective, kMaxName)) {
        return std::nullopt;
    }
    out.action = static_cast<ScoreAction>(action);
    if (out.action == ScoreAction::Change && !read_int(reader, out.value)) {
        return std::nullopt;
    }
    if (reader.remaining() != 0) {
        return std::nullopt;
    }
    return out;
}

}  // namespace ov::net::scoreboard
