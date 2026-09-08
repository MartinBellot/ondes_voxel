#include "ov/protocol/play.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/world/heightmap.hpp"

#include <array>
#include <cmath>

namespace ov::net {
namespace {

/// The dimensions the client is told exist. It needs the list to size its own
/// registry; it does not need them to be reachable yet.
constexpr std::array<std::string_view, 3> kWorldNames{
    "minecraft:overworld",
    "minecraft:the_nether",
    "minecraft:the_end",
};

void write_chat_component(io::ByteWriter& writer, std::string_view text) {
    std::string json = R"({"text":")";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            json.push_back('\\');
        }
        json.push_back(c);
    }
    json += R"("})";
    write_string(writer, json);
}

/// A block position packed into a long: 26 bits x, 26 bits z, 12 bits y.
///
/// Note the order — x, then **z**, then y in the low bits. Swapping y and z
/// puts the spawn point somewhere plausible and wrong, and the packing is
/// signed, so the shifts have to be done on unsigned values to avoid relying on
/// implementation-defined behaviour.
void write_position(io::ByteWriter& writer, i32 x, i32 y, i32 z) {
    const u64 packed = ((static_cast<u64>(x) & 0x3FFFFFF) << 38) |
                       ((static_cast<u64>(z) & 0x3FFFFFF) << 12) | (static_cast<u64>(y) & 0xFFF);
    writer.write_u64(packed);
}

/// One paletted container, in the wire's shape.
///
/// The three forms differ in more than size: the width is what tells the reader
/// which one it is looking at, so a container that pads its width into another
/// form's range is decoded as that other form.
void write_paletted_container(io::ByteWriter& writer, const world::PalettedContainer& container) {
    writer.write_u8(container.bits());

    switch (container.kind()) {
        case world::PaletteKind::SingleValue:
            // One value, and no data at all — not an empty array with a length
            // of zero written after a palette.
            write_varint(
                writer, static_cast<i32>(container.palette().empty() ? 0 : container.palette()[0]));
            break;

        case world::PaletteKind::Indirect:
            write_varint(writer, static_cast<i32>(container.palette().size()));
            for (const u16 entry : container.palette()) {
                write_varint(writer, static_cast<i32>(entry));
            }
            break;

        case world::PaletteKind::Direct:
            // No palette section at all.
            break;
    }

    write_varint(writer, static_cast<i32>(container.data().size()));
    for (const u64 word : container.data()) {
        writer.write_u64(word);
    }
}

/// The heightmaps the client is sent, as an NBT compound.
///
/// Only two of the four travel. Sending the others is harmless and sending
/// neither is not: the client uses MOTION_BLOCKING for rain and particles.
[[nodiscard]] std::vector<u8> encode_heightmaps(const world::Chunk& chunk) {
    nbt::Document document;
    document.name = "";
    document.root = nbt::Tag::make_compound();

    for (const world::HeightmapType type :
         {world::HeightmapType::MotionBlocking, world::HeightmapType::WorldSurface}) {
        const auto data = chunk.heightmap(type).data();

        nbt::Tag::LongArray longs;
        longs.reserve(data.size());
        for (const u64 word : data) {
            longs.push_back(static_cast<i64>(word));
        }
        document.root.compound()->push_back(
            nbt::CompoundEntry{std::string{world::to_string(type)}, nbt::Tag{std::move(longs)}});
    }

    return nbt::write(document);
}

}  // namespace

std::vector<u8> encode_login_play(const LoginPlay& login) {
    io::ByteWriter writer;

    writer.write_i32(login.entity_id);
    writer.write_u8(0);  // is hardcore
    writer.write_u8(login.game_mode);
    writer.write_i8(-1);  // previous game mode: none

    write_varint(writer, static_cast<i32>(kWorldNames.size()));
    for (const std::string_view name : kWorldNames) {
        write_string(writer, name);
    }

    writer.write_bytes(login.registry_codec);
    write_string(writer, login.dimension_type);
    write_string(writer, login.dimension_name);

    writer.write_i64(login.hashed_seed);
    write_varint(writer, login.max_players);
    write_varint(writer, login.view_distance);
    write_varint(writer, login.simulation_distance);

    writer.write_u8(0);  // reduced debug info
    writer.write_u8(1);  // enable respawn screen
    writer.write_u8(0);  // is debug
    writer.write_u8(login.is_flat ? 1 : 0);
    writer.write_u8(0);  // has death location

    // Portal cooldown. Present in 763 — confirmed by decoding what a real
    // 1.20.1 server sends, because the schema this was written from also
    // covers later versions and the field could have belonged to one of them.
    write_varint(writer, 0);

    return writer.take();
}

std::vector<u8> encode_synchronize_position(f64 x, f64 y, f64 z, f32 yaw, f32 pitch,
                                            i32 teleport_id) {
    io::ByteWriter writer;
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_f32(yaw);
    writer.write_f32(pitch);
    writer.write_u8(0);  // all values absolute
    write_varint(writer, teleport_id);
    return writer.take();
}

std::vector<u8> encode_set_center_chunk(i32 chunk_x, i32 chunk_z) {
    io::ByteWriter writer;
    write_varint(writer, chunk_x);
    write_varint(writer, chunk_z);
    return writer.take();
}

std::vector<u8> encode_unload_chunk(i32 chunk_x, i32 chunk_z) {
    io::ByteWriter writer;
    writer.write_i32(chunk_x);
    writer.write_i32(chunk_z);
    return writer.take();
}

namespace {

/// Angles travel as a single byte: 256 steps to a full turn.
///
/// The conversion has to wrap rather than clamp — a yaw of 350 degrees and one
/// of -10 are the same direction, and clamping would pin a player looking
/// slightly west to due south.
[[nodiscard]] i8 angle_byte(f32 degrees) noexcept {
    const auto steps = static_cast<i32>(std::lround(static_cast<f64>(degrees) * 256.0 / 360.0));
    return static_cast<i8>(static_cast<u8>(steps & 0xFF));
}

}  // namespace

std::vector<u8> encode_player_info_add(const Uuid& uuid, std::string_view name, i32 game_mode) {
    io::ByteWriter writer;

    // add_player | update_game_mode | update_listed. Sending add_player alone
    // leaves the entry unlisted, and an unlisted player is not rendered.
    writer.write_u8(0x01 | 0x04 | 0x08);
    write_varint(writer, 1);

    write_uuid(writer, uuid);
    write_string(writer, name);
    write_varint(writer, 0);  // no skin properties: offline mode has no session
    write_varint(writer, game_mode);
    writer.write_u8(1);  // listed
    return writer.take();
}

std::vector<u8> encode_player_info_remove(const Uuid& uuid) {
    io::ByteWriter writer;
    write_varint(writer, 1);
    write_uuid(writer, uuid);
    return writer.take();
}

std::vector<u8> encode_spawn_player(i32 entity_id, const Uuid& uuid, f64 x, f64 y, f64 z, f32 yaw,
                                    f32 pitch) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    write_uuid(writer, uuid);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_i8(angle_byte(yaw));
    writer.write_i8(angle_byte(pitch));
    return writer.take();
}

std::vector<u8> encode_entity_teleport(i32 entity_id, f64 x, f64 y, f64 z, f32 yaw, f32 pitch,
                                       bool on_ground) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_i8(angle_byte(yaw));
    writer.write_i8(angle_byte(pitch));
    writer.write_u8(on_ground ? 1 : 0);
    return writer.take();
}

std::vector<u8> encode_entity_head_rotation(i32 entity_id, f32 yaw) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_i8(angle_byte(yaw));
    return writer.take();
}

std::vector<u8> encode_remove_entity(i32 entity_id) {
    io::ByteWriter writer;
    write_varint(writer, 1);
    write_varint(writer, entity_id);
    return writer.take();
}

std::vector<u8> encode_keep_alive(i64 id) {
    io::ByteWriter writer;
    writer.write_i64(id);
    return writer.take();
}

std::vector<u8> encode_set_default_spawn(i32 x, i32 y, i32 z, f32 angle) {
    io::ByteWriter writer;
    write_position(writer, x, y, z);
    writer.write_f32(angle);
    return writer.take();
}

std::vector<u8> encode_player_abilities(bool invulnerable, bool flying, bool allow_flying,
                                        bool creative, f32 flying_speed, f32 walking_speed) {
    io::ByteWriter writer;

    u8 flags = 0;
    flags |= invulnerable ? 0x01 : 0;
    flags |= flying ? 0x02 : 0;
    flags |= allow_flying ? 0x04 : 0;
    flags |= creative ? 0x08 : 0;
    writer.write_u8(flags);

    writer.write_f32(flying_speed);
    writer.write_f32(walking_speed);
    return writer.take();
}

std::vector<u8> encode_game_event(u8 reason, f32 value) {
    io::ByteWriter writer;
    writer.write_u8(reason);
    writer.write_f32(value);
    return writer.take();
}

std::vector<u8> encode_play_disconnect(std::string_view reason) {
    io::ByteWriter writer;
    write_chat_component(writer, reason);
    return writer.take();
}

std::vector<u8> encode_chunk_data(const world::Chunk& chunk) {
    io::ByteWriter writer;

    writer.write_i32(chunk.position().x);
    writer.write_i32(chunk.position().z);
    writer.write_bytes(encode_heightmaps(chunk));

    // The sections, concatenated with no per-section length. One miscounted
    // byte shifts everything after it, and the client reports a decode error
    // that names no field.
    io::ByteWriter sections;
    for (const world::ChunkSection& section : chunk.sections()) {
        sections.write_i16(static_cast<i16>(section.non_air_count()));
        write_paletted_container(sections, section.blocks());
        write_paletted_container(sections, section.biomes());
    }
    write_varint(writer, static_cast<i32>(sections.size()));
    writer.write_bytes(sections.data());

    write_varint(writer, 0);  // block entities

    // Light. The masks cover section_count + 2 entries: one below the world and
    // one above, because light spills past the build limits in both directions.
    const usize light_sections = chunk.shape().section_count() + 2;

    io::ByteWriter sky_data;
    io::ByteWriter block_data;
    u64            sky_mask         = 0;
    u64            block_mask       = 0;
    u64            empty_sky_mask   = 0;
    u64            empty_block_mask = 0;
    usize          sky_count        = 0;
    usize          block_count      = 0;

    for (usize i = 0; i < light_sections; ++i) {
        // The extra section at each end has no storage of its own.
        const world::ChunkSection* section =
            (i == 0 || i > chunk.sections().size()) ? nullptr : &chunk.sections()[i - 1];

        // "Empty" on the wire means *all zero*, not "not stored". A section
        // that is uniformly lit still has to be written out; only a uniformly
        // dark one can be elided. Reading that the other way round renders the
        // whole world black, and the client reports nothing at all.
        const bool has_sky   = section != nullptr && !section->sky_light().is_absent();
        const bool has_block = section != nullptr && !section->block_light().is_absent();

        if (has_sky) {
            sky_mask |= u64{1} << i;
            ++sky_count;
            const auto bytes = section->sky_light().to_bytes();
            write_varint(sky_data, static_cast<i32>(bytes.size()));
            sky_data.write_bytes(bytes);
        } else {
            empty_sky_mask |= u64{1} << i;
        }

        if (has_block) {
            block_mask |= u64{1} << i;
            ++block_count;
            const auto bytes = section->block_light().to_bytes();
            write_varint(block_data, static_cast<i32>(bytes.size()));
            block_data.write_bytes(bytes);
        } else {
            empty_block_mask |= u64{1} << i;
        }
    }

    // Each mask is a BitSet: a length-prefixed array of longs.
    for (const u64 mask : {sky_mask, block_mask, empty_sky_mask, empty_block_mask}) {
        write_varint(writer, 1);
        writer.write_u64(mask);
    }

    write_varint(writer, static_cast<i32>(sky_count));
    writer.write_bytes(sky_data.data());
    write_varint(writer, static_cast<i32>(block_count));
    writer.write_bytes(block_data.data());

    return writer.take();
}

// ── Serverbound ─────────────────────────────────────────────────────────────

std::optional<ClientInformation> parse_client_information(std::span<const u8> payload) {
    io::ByteReader reader{payload};

    const auto locale = read_string(reader, 16);
    if (!locale) {
        return std::nullopt;
    }
    const auto view_distance = reader.read_u8();
    if (!view_distance) {
        return std::nullopt;
    }

    // The remaining fields exist and the server does not act on any of them
    // yet. Reading them would be honest padding; not reading them costs
    // nothing, because the frame length already told us where the packet ends.
    return ClientInformation{std::string{*locale}, *view_distance};
}

std::optional<PlayerMovement> parse_movement(i32 packet_id, std::span<const u8> payload) {
    io::ByteReader reader{payload};
    PlayerMovement movement;

    const bool has_position = packet_id == serverbound::kSetPlayerPosition ||
                              packet_id == serverbound::kSetPlayerPositionRot;
    const bool has_rotation = packet_id == serverbound::kSetPlayerRotation ||
                              packet_id == serverbound::kSetPlayerPositionRot;

    if (!has_position && !has_rotation && packet_id != serverbound::kSetPlayerOnGround) {
        return std::nullopt;
    }

    if (has_position) {
        const auto x = reader.read_f64();
        const auto y = reader.read_f64();
        const auto z = reader.read_f64();
        if (!x || !y || !z) {
            return std::nullopt;
        }
        movement.x = *x;
        movement.y = *y;
        movement.z = *z;
    }
    if (has_rotation) {
        const auto yaw   = reader.read_f32();
        const auto pitch = reader.read_f32();
        if (!yaw || !pitch) {
            return std::nullopt;
        }
        movement.yaw   = *yaw;
        movement.pitch = *pitch;
    }

    const auto on_ground = reader.read_u8();
    if (!on_ground) {
        return std::nullopt;
    }
    movement.on_ground = *on_ground != 0;
    return movement;
}

std::optional<i32> parse_confirm_teleport(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     id = read_varint(reader);
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

namespace {

/// Unpack the 26/26/12 block position.
///
/// All three fields are signed, so each is shifted left to put its sign bit at
/// the top and then arithmetically right — masking instead would make every
/// negative coordinate a large positive one, and half the world is at negative
/// coordinates.
[[nodiscard]] WirePosition unpack_position(u64 packed) noexcept {
    const auto value = static_cast<i64>(packed);
    return WirePosition{
        static_cast<i32>(value >> 38),
        static_cast<i32>(value << 52 >> 52),
        static_cast<i32>(value << 26 >> 38),
    };
}

}  // namespace

WirePosition offset_by_face(WirePosition position, i32 face) noexcept {
    switch (face) {
        case 0: return {position.x, position.y - 1, position.z};
        case 1: return {position.x, position.y + 1, position.z};
        case 2: return {position.x, position.y, position.z - 1};
        case 3: return {position.x, position.y, position.z + 1};
        case 4: return {position.x - 1, position.y, position.z};
        case 5: return {position.x + 1, position.y, position.z};
        default: return position;
    }
}

std::optional<PlayerAction> parse_player_action(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     status = read_varint(reader);
    const auto     packed = reader.read_u64();
    const auto     face   = reader.read_i8();
    if (!status || !packed || !face) {
        return std::nullopt;
    }
    const auto sequence = read_varint(reader);
    if (!sequence) {
        return std::nullopt;
    }
    return PlayerAction{*status, unpack_position(*packed), *face, *sequence};
}

std::optional<UseItemOn> parse_use_item_on(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     hand   = read_varint(reader);
    const auto     packed = reader.read_u64();
    const auto     face   = read_varint(reader);
    if (!hand || !packed || !face) {
        return std::nullopt;
    }
    const auto cursor_x = reader.read_f32();
    const auto cursor_y = reader.read_f32();
    const auto cursor_z = reader.read_f32();
    // The inside-block flag is read past rather than used: it matters for
    // placing against a block the player is standing in, which needs collision.
    if (!cursor_x || !cursor_y || !cursor_z || !reader.read_u8()) {
        return std::nullopt;
    }
    const auto sequence = read_varint(reader);
    if (!sequence) {
        return std::nullopt;
    }
    return UseItemOn{unpack_position(*packed), *face, *cursor_x, *cursor_y, *cursor_z, *sequence};
}

std::optional<CreativeSlot> parse_set_creative_slot(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     slot    = reader.read_i16();
    const auto     present = reader.read_u8();
    if (!slot || !present) {
        return std::nullopt;
    }
    if (*present == 0) {
        return CreativeSlot{*slot, std::nullopt};
    }
    const auto item_id = read_varint(reader);
    if (!item_id) {
        return std::nullopt;
    }
    // Count and the item's NBT follow. Neither is needed to know which block
    // the player is holding, and the frame length already bounds the packet.
    return CreativeSlot{*slot, *item_id};
}

std::optional<i16> parse_set_held_item(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     slot = reader.read_i16();
    if (!slot) {
        return std::nullopt;
    }
    return *slot;
}

std::vector<u8> encode_block_update(WirePosition position, i32 state) {
    io::ByteWriter writer;
    write_position(writer, position.x, position.y, position.z);
    write_varint(writer, state);
    return writer.take();
}

std::vector<u8> encode_acknowledge_dig(i32 sequence) {
    io::ByteWriter writer;
    write_varint(writer, sequence);
    return writer.take();
}

std::optional<i64> parse_keep_alive(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     id = reader.read_i64();
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

}  // namespace ov::net
