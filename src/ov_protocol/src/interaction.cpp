#include "ov/protocol/interaction.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {

std::optional<Interact> parse_interact(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     entity_id = read_varint(reader);
    const auto     kind      = read_varint(reader);
    if (!entity_id || !kind || *kind < 0 || *kind > 2) {
        return std::nullopt;
    }

    Interact out;
    out.entity_id = *entity_id;
    out.kind      = static_cast<InteractKind>(*kind);

    // The three fields are only on the wire for InteractAt, so reading them
    // unconditionally would eat the sneak flag of every other interaction and
    // leave the parser one byte short.
    if (out.kind == InteractKind::InteractAt) {
        const auto x = reader.read_f32();
        const auto y = reader.read_f32();
        const auto z = reader.read_f32();
        if (!x || !y || !z) {
            return std::nullopt;
        }
        out.target_x = *x;
        out.target_y = *y;
        out.target_z = *z;
    }

    if (out.kind != InteractKind::Attack) {
        const auto hand = read_varint(reader);
        if (!hand || (*hand != 0 && *hand != 1)) {
            return std::nullopt;
        }
        out.hand = static_cast<Hand>(*hand);
    }

    const auto sneaking = reader.read_u8();
    if (!sneaking) {
        return std::nullopt;
    }
    out.sneaking = *sneaking != 0;
    return out;
}

std::optional<UseItem> parse_use_item(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     hand     = read_varint(reader);
    const auto     sequence = read_varint(reader);
    if (!hand || !sequence || (*hand != 0 && *hand != 1)) {
        return std::nullopt;
    }
    return UseItem{static_cast<Hand>(*hand), *sequence};
}

std::optional<Hand> parse_swing_arm(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     hand = read_varint(reader);
    if (!hand || (*hand != 0 && *hand != 1)) {
        return std::nullopt;
    }
    return static_cast<Hand>(*hand);
}

std::vector<u8> encode_set_cooldown(i32 item_id, i32 ticks) {
    io::ByteWriter writer;
    write_varint(writer, item_id);
    write_varint(writer, ticks);
    return writer.take();
}

std::vector<u8> encode_block_action(WirePosition position, u8 action_id, u8 action_parameter,
                                    i32 block_type) {
    io::ByteWriter writer;
    writer.write_u64(static_cast<u64>(pack_position(position)));
    writer.write_u8(action_id);
    writer.write_u8(action_parameter);
    write_varint(writer, block_type);
    return writer.take();
}

}  // namespace ov::net
