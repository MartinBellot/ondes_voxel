#include "ov/protocol/breaking.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

#include <cmath>

namespace ov::net {

std::vector<u8> encode_block_destroy_stage(const BlockDestroyStage& packet) {
    io::ByteWriter writer;
    write_varint(writer, packet.entity_id);
    write_position(writer, packet.position);
    writer.write_u8(static_cast<u8>(packet.stage));
    return writer.take();
}

std::optional<BlockDestroyStage> parse_block_destroy_stage(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     entity = read_varint(reader);
    const auto     packed = reader.read_i64();
    const auto     stage  = reader.read_i8();
    if (!entity || !packed || !stage || !reader.exhausted()) {
        return std::nullopt;
    }
    BlockDestroyStage out;
    out.entity_id = *entity;
    out.position  = unpack_position(*packed);
    out.stage     = *stage;
    return out;
}

i8 server_destroy_stage(f32 progress) noexcept {
    const f32 scaled = std::floor(progress * 10.0F);
    if (!(scaled >= -128.0F)) {
        return -128;
    }
    if (scaled > 127.0F) {
        return 127;
    }
    return static_cast<i8>(scaled);
}

std::vector<u8> encode_player_action(i32 status, WirePosition position, u8 face, i32 sequence) {
    io::ByteWriter writer;
    write_varint(writer, status);
    write_position(writer, position);
    writer.write_u8(face);
    write_varint(writer, sequence);
    return writer.take();
}

std::vector<u8> encode_swing_arm(Hand hand) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(hand));
    return writer.take();
}

}  // namespace ov::net
