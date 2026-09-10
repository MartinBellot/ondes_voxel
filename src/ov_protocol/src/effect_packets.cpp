#include "ov/protocol/effect_packets.hpp"

#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {
namespace {

// The factor compound's keys, **in the order the real server wrote them**:
// previous frame, start, ticks active, target, current, had-effect, padding.
// Not alphabetical and not declaration order — it is the iteration order of
// the game's own compound. Written in this order so the bytes match the
// capture exactly; a client reads by name and would not care, a byte-for-byte
// test would.
[[nodiscard]] nbt::Tag factor_compound(const EffectFactorData& factor) {
    nbt::Tag compound = nbt::Tag::make_compound();
    compound.put("factor_previous_frame", nbt::Tag{factor.factor_previous_frame});
    compound.put("factor_start", nbt::Tag{factor.factor_start});
    compound.put("ticks_active", nbt::Tag{factor.ticks_active});
    compound.put("factor_target", nbt::Tag{factor.factor_target});
    compound.put("factor_current", nbt::Tag{factor.factor_current});
    compound.put("had_effect_last_tick", nbt::Tag::make_bool(factor.had_effect_last_tick));
    compound.put("padding_duration", nbt::Tag{factor.padding_duration});
    return compound;
}

[[nodiscard]] std::optional<EffectFactorData> factor_from(const nbt::Tag& compound) {
    if (compound.compound() == nullptr) {
        return std::nullopt;
    }
    EffectFactorData out;
    const auto f32_of = [&](std::string_view name, f32& into) {
        if (const nbt::Tag* tag = compound.find(name)) {
            into = static_cast<f32>(tag->as_f64(static_cast<f64>(into)));
        }
    };
    const auto i32_of = [&](std::string_view name, i32& into) {
        if (const nbt::Tag* tag = compound.find(name)) {
            into = static_cast<i32>(tag->as_i64(into));
        }
    };
    i32_of("padding_duration", out.padding_duration);
    f32_of("factor_start", out.factor_start);
    f32_of("factor_target", out.factor_target);
    f32_of("factor_current", out.factor_current);
    i32_of("ticks_active", out.ticks_active);
    f32_of("factor_previous_frame", out.factor_previous_frame);
    if (const nbt::Tag* tag = compound.find("had_effect_last_tick")) {
        out.had_effect_last_tick = tag->as_bool();
    }
    return out;
}

}  // namespace

std::vector<u8> encode_entity_effect(const EntityEffect& packet) {
    io::ByteWriter writer;
    write_varint(writer, packet.entity_id);
    write_varint(writer, packet.effect_id);
    writer.write_u8(packet.amplifier);
    write_varint(writer, packet.duration);
    writer.write_u8(packet.flags);
    writer.write_u8(packet.factor ? 1 : 0);
    if (packet.factor) {
        // A named root with an empty name: 0x0A, then a zero-length name. The
        // capture has it; 1.20.2 would not.
        nbt::write(nbt::Document{"", factor_compound(*packet.factor)}, writer);
    }
    return writer.take();
}

std::expected<EntityEffect, EffectPacketError> decode_entity_effect(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    EntityEffect   out;
    const auto     entity = read_varint(reader);
    const auto     effect = entity ? read_varint(reader) : entity;
    if (!entity || !effect) {
        return std::unexpected(EffectPacketError::BadVarInt);
    }
    out.entity_id        = *entity;
    out.effect_id        = *effect;
    const auto amplifier = reader.read_u8();
    if (!amplifier) {
        return std::unexpected(EffectPacketError::Truncated);
    }
    out.amplifier       = *amplifier;
    const auto duration = read_varint(reader);
    if (!duration) {
        return std::unexpected(EffectPacketError::BadVarInt);
    }
    out.duration     = *duration;
    const auto flags = reader.read_u8();
    const auto has   = flags ? reader.read_u8() : flags;
    if (!flags || !has) {
        return std::unexpected(EffectPacketError::Truncated);
    }
    out.flags = *flags;
    if (*has != 0) {
        auto document = nbt::read(reader);
        if (!document) {
            return std::unexpected(EffectPacketError::BadNbt);
        }
        out.factor = factor_from(document->root);
        if (!out.factor) {
            return std::unexpected(EffectPacketError::BadNbt);
        }
    }
    if (!reader.exhausted()) {
        return std::unexpected(EffectPacketError::TrailingBytes);
    }
    return out;
}

std::vector<u8> encode_remove_entity_effect(i32 entity_id, i32 effect_id) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    write_varint(writer, effect_id);
    return writer.take();
}

std::expected<RemoveEntityEffect, EffectPacketError> decode_remove_entity_effect(
    std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     entity = read_varint(reader);
    const auto     effect = entity ? read_varint(reader) : entity;
    if (!entity || !effect) {
        return std::unexpected(EffectPacketError::BadVarInt);
    }
    if (!reader.exhausted()) {
        return std::unexpected(EffectPacketError::TrailingBytes);
    }
    return RemoveEntityEffect{*entity, *effect};
}

std::vector<u8> encode_update_attributes_full(i32                                entity_id,
                                              std::span<const AttributeProperty> properties) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    write_varint(writer, static_cast<i32>(properties.size()));
    for (const AttributeProperty& property : properties) {
        write_string(writer, property.name);
        writer.write_f64(property.base);
        write_varint(writer, static_cast<i32>(property.modifiers.size()));
        for (const WireModifier& modifier : property.modifiers) {
            write_uuid(writer, modifier.uuid);
            writer.write_f64(modifier.amount);
            writer.write_u8(modifier.operation);
        }
    }
    return writer.take();
}

std::expected<DecodedUpdateAttributes, EffectPacketError> decode_update_attributes(
    std::span<const u8> payload) {
    io::ByteReader          reader{payload};
    DecodedUpdateAttributes out;
    const auto              entity = read_varint(reader);
    const auto              count  = entity ? read_varint(reader) : entity;
    if (!entity || !count || *count < 0) {
        return std::unexpected(EffectPacketError::BadVarInt);
    }
    out.entity_id = *entity;
    for (i32 i = 0; i < *count; ++i) {
        DecodedProperty property;
        auto            name = read_string(reader, 32767);
        if (!name) {
            return std::unexpected(EffectPacketError::Truncated);
        }
        property.name   = std::move(*name);
        const auto base = reader.read_f64();
        const auto mods = base ? read_varint(reader) : std::unexpected(VarIntError{});
        if (!base || !mods || *mods < 0) {
            return std::unexpected(EffectPacketError::Truncated);
        }
        property.base = *base;
        for (i32 m = 0; m < *mods; ++m) {
            const auto uuid      = read_uuid(reader);
            const auto amount    = uuid ? reader.read_f64() : std::unexpected(io::ReadError{});
            const auto operation = amount ? reader.read_u8() : std::unexpected(io::ReadError{});
            if (!uuid || !amount || !operation) {
                return std::unexpected(EffectPacketError::Truncated);
            }
            property.modifiers.push_back(WireModifier{*uuid, *amount, *operation});
        }
        out.properties.push_back(std::move(property));
    }
    if (!reader.exhausted()) {
        return std::unexpected(EffectPacketError::TrailingBytes);
    }
    return out;
}

}  // namespace ov::net
