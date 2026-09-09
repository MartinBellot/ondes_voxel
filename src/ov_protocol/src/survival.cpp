#include "ov/protocol/survival.hpp"

#include "ov/io/byte_writer.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

#include <string>

namespace ov::net {
namespace {

/// A JSON string literal with the characters JSON cannot carry escaped.
///
/// Player names cannot contain any of these, but death messages are built from
/// entity custom names too, and a mob called `"` would otherwise produce a chat
/// component the client refuses to parse — which disconnects it at the moment
/// it was about to show a death screen.
void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                // A control character. Dropped rather than passed through:
                // the component would parse and then render as a hole.
                break;
            }
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

}  // namespace

std::vector<u8> encode_set_health(f32 health, i32 food, f32 saturation) {
    io::ByteWriter writer;
    writer.write_f32(health);
    write_varint(writer, food);
    writer.write_f32(saturation);
    return writer.take();
}

std::vector<u8> encode_set_experience(f32 bar, i32 level, i32 total) {
    io::ByteWriter writer;
    writer.write_f32(bar);
    write_varint(writer, level);
    write_varint(writer, total);
    return writer.take();
}

std::vector<u8> encode_combat_death(i32 player_entity_id, std::string_view message_json) {
    io::ByteWriter writer;
    write_varint(writer, player_entity_id);
    // And then the message, immediately. No killer entity id — see the note at
    // the top of survival.hpp; this was decoded out of a real server's own
    // bytes, which left no room for one.
    write_string(writer, message_json);
    return writer.take();
}

std::string death_message_json(std::string_view translation_key, std::string_view victim,
                               std::string_view killer) {
    std::string out;
    out.reserve(translation_key.size() + victim.size() + killer.size() + 64);
    out += R"({"translate":)";
    append_json_string(out, translation_key);
    out += R"(,"with":[{"text":)";
    append_json_string(out, victim);
    out += "}";
    if (!killer.empty()) {
        out += R"(,{"text":)";
        append_json_string(out, killer);
        out += "}";
    }
    out += "]}";
    return out;
}

std::vector<u8> encode_respawn(const Respawn& respawn) {
    io::ByteWriter writer;
    write_string(writer, respawn.dimension_type);
    write_string(writer, respawn.dimension_name);
    writer.write_i64(respawn.hashed_seed);
    writer.write_u8(respawn.game_mode);
    writer.write_i8(respawn.previous_game_mode);
    writer.write_u8(respawn.is_debug ? 1 : 0);
    writer.write_u8(respawn.is_flat ? 1 : 0);
    writer.write_u8(respawn.data_kept);
    writer.write_u8(respawn.death_dimension ? 1 : 0);
    if (respawn.death_dimension) {
        write_string(writer, *respawn.death_dimension);
        write_position(writer, respawn.death_position);
    }
    // Portal cooldown, last. The same field Login (play) grew in this version,
    // and the same reason it is easy to leave out: nothing visible depends on
    // it, and the packet after it decodes as garbage without it.
    write_varint(writer, respawn.portal_cooldown);
    return writer.take();
}

std::vector<u8> encode_spawn_experience_orb(i32 entity_id, f64 x, f64 y, f64 z, i16 count) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_i16(count);
    return writer.take();
}

std::optional<ClientCommand> parse_client_command(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     action = read_varint(reader);
    if (!action) {
        return std::nullopt;
    }
    switch (*action) {
    case 0:
        return ClientCommand::PerformRespawn;
    case 1:
        return ClientCommand::RequestStats;
    default:
        // A third action would be a protocol we do not know. Named as unknown
        // rather than treated as a respawn, which would put a live player back
        // at spawn for pressing a button we misread.
        return std::nullopt;
    }
}

std::optional<PlayerCommand> parse_player_command(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     entity = read_varint(reader);
    const auto     action = read_varint(reader);
    if (!entity || !action) {
        return std::nullopt;
    }
    if (*action > static_cast<i32>(PlayerCommandAction::StopSprinting)) {
        // Horse jumping, elytra and the horse inventory live above this. Named
        // as unhandled rather than folded into the nearest action: reading
        // "open horse inventory" as "stop sprinting" would silently stop
        // charging hunger and nothing would ever point at this packet.
        return std::nullopt;
    }
    return PlayerCommand{*entity, static_cast<PlayerCommandAction>(*action)};
}

}  // namespace ov::net
