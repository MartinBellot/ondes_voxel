#include "ov/protocol/entity_metadata.hpp"

#include "ov/io/byte_writer.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {

namespace {

/// Read one value of `type` into `out`. False when the body ended, or when the
/// type has no width this reader can know.
[[nodiscard]] bool read_value(io::ByteReader& reader, MetadataType type, MetadataValue& out) {
    switch (type) {
        case MetadataType::Byte:
        case MetadataType::Boolean: {
            const auto value = reader.read_i8();
            if (!value) {
                return false;
            }
            out.integer = *value;
            return true;
        }
        case MetadataType::Float: {
            const auto value = reader.read_f32();
            if (!value) {
                return false;
            }
            out.real = *value;
            return true;
        }
        case MetadataType::VarLong: {
            // A real varlong, not a varint: a long past five bytes would shift
            // every field after it by the difference.
            const auto value = read_varlong(reader);
            if (!value) {
                return false;
            }
            out.integer = *value;
            return true;
        }
        case MetadataType::VarInt:
        case MetadataType::Direction:
        case MetadataType::BlockState:
        case MetadataType::OptionalBlockState:
        case MetadataType::Pose:
        case MetadataType::CatVariant:
        case MetadataType::FrogVariant:
        case MetadataType::PaintingVariant:
        case MetadataType::SnifferState: {
            const auto value = read_varint(reader);
            if (!value) {
                return false;
            }
            out.integer = *value;
            if (type == MetadataType::OptionalBlockState) {
                out.present = *value != 0;
            }
            return true;
        }
        case MetadataType::OptionalUnsignedInt: {
            const auto value = read_varint(reader);
            if (!value) {
                return false;
            }
            out.present = *value != 0;
            out.integer = *value - 1;
            return true;
        }
        case MetadataType::String:
        case MetadataType::Component: {
            auto value = read_string(reader);
            if (!value) {
                return false;
            }
            out.text = std::move(*value);
            return true;
        }
        case MetadataType::OptionalComponent: {
            const auto present = reader.read_u8();
            if (!present) {
                return false;
            }
            out.present = *present != 0;
            if (out.present) {
                auto value = read_string(reader);
                if (!value) {
                    return false;
                }
                out.text = std::move(*value);
            }
            return true;
        }
        case MetadataType::ItemStack: {
            auto stack = read_slot(reader);
            if (!stack) {
                return false;
            }
            out.stack = std::move(*stack);
            return true;
        }
        case MetadataType::Rotations:
        case MetadataType::Vector3:
        case MetadataType::Quaternion: {
            const usize count = type == MetadataType::Quaternion ? 4 : 3;
            for (usize component = 0; component < count; ++component) {
                const auto value = reader.read_f32();
                if (!value) {
                    return false;
                }
                out.vector[component] = *value;
            }
            return true;
        }
        case MetadataType::BlockPos: {
            const auto value = reader.read_i64();
            if (!value) {
                return false;
            }
            out.position = *value;
            return true;
        }
        case MetadataType::OptionalBlockPos:
        case MetadataType::OptionalUuid: {
            const auto present = reader.read_u8();
            if (!present) {
                return false;
            }
            out.present = *present != 0;
            if (!out.present) {
                return true;
            }
            if (type == MetadataType::OptionalUuid) {
                return reader.skip(16).has_value();
            }
            const auto value = reader.read_i64();
            if (!value) {
                return false;
            }
            out.position = *value;
            return true;
        }
        case MetadataType::VillagerData:
            for (usize field = 0; field < 3; ++field) {
                const auto value = read_varint(reader);
                if (!value) {
                    return false;
                }
                out.villager[field] = *value;
            }
            return true;
        case MetadataType::OptionalGlobalPos: {
            const auto present = reader.read_u8();
            if (!present) {
                return false;
            }
            out.present = *present != 0;
            if (!out.present) {
                return true;
            }
            auto dimension = read_string(reader);
            if (!dimension) {
                return false;
            }
            out.text = std::move(*dimension);
            const auto value = reader.read_i64();
            if (!value) {
                return false;
            }
            out.position = *value;
            return true;
        }
        case MetadataType::CompoundTag:
        case MetadataType::Particle:
            // Refused by name rather than skipped by a guessed width: a
            // compound tag and a particle each need a parser this layer does
            // not have, and a wrong guess reads the rest as garbage.
            return false;
    }
    return false;
}

}  // namespace

ParsedMetadata parse_entity_metadata(io::ByteReader& reader) {
    ParsedMetadata parsed;
    for (;;) {
        const auto index = reader.read_u8();
        if (!index) {
            parsed.complete = false;
            return parsed;
        }
        if (*index == 0xFF) {
            return parsed;
        }
        const auto type = read_varint(reader);
        if (!type || *type < 0 || *type > static_cast<i32>(MetadataType::Quaternion)) {
            parsed.complete = false;
            return parsed;
        }
        MetadataValue value;
        value.index = *index;
        value.type  = static_cast<MetadataType>(*type);
        if (!read_value(reader, value.type, value)) {
            parsed.complete = false;
            return parsed;
        }
        parsed.values.push_back(std::move(value));
    }
}

std::vector<u8> encode_entity_equipment(i32 entity_id, std::span<const EquipmentEntry> entries) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    for (usize index = 0; index < entries.size(); ++index) {
        // The top bit says "another entry follows". It is on every entry but
        // the last; a list with it set on the last makes the reader consume
        // the next packet's first byte as a slot.
        const bool more = index + 1 < entries.size();
        writer.write_u8(static_cast<u8>(static_cast<u8>(entries[index].slot) | (more ? 0x80U : 0U)));
        write_slot(writer, entries[index].stack);
    }
    return writer.take();
}

std::optional<EntityEquipment> parse_entity_equipment(std::span<const u8> payload) {
    io::ByteReader reader(payload);
    const auto     id = read_varint(reader);
    if (!id) {
        return std::nullopt;
    }
    EntityEquipment equipment;
    equipment.entity_id = *id;
    for (;;) {
        const auto slot = reader.read_u8();
        if (!slot) {
            return std::nullopt;
        }
        const u8 number = static_cast<u8>(*slot & 0x7FU);
        if (number >= kEquipmentSlots) {
            return std::nullopt;
        }
        auto stack = read_slot(reader);
        if (!stack) {
            return std::nullopt;
        }
        equipment.entries.push_back(
            EquipmentEntry{static_cast<EquipmentSlot>(number), std::move(*stack)});
        if ((*slot & 0x80U) == 0) {
            return equipment;
        }
    }
}

std::vector<u8> encode_set_passengers(const Passengers& passengers) {
    io::ByteWriter writer;
    write_varint(writer, passengers.vehicle_id);
    write_varint(writer, static_cast<i32>(passengers.riders.size()));
    for (const i32 rider : passengers.riders) {
        write_varint(writer, rider);
    }
    return writer.take();
}

std::optional<Passengers> parse_set_passengers(std::span<const u8> payload) {
    io::ByteReader reader(payload);
    const auto     vehicle = read_varint(reader);
    const auto     count   = read_varint(reader);
    if (!vehicle || !count || *count < 0 || static_cast<usize>(*count) > reader.remaining()) {
        return std::nullopt;
    }
    Passengers passengers;
    passengers.vehicle_id = *vehicle;
    for (i32 index = 0; index < *count; ++index) {
        const auto rider = read_varint(reader);
        if (!rider) {
            return std::nullopt;
        }
        passengers.riders.push_back(*rider);
    }
    return passengers;
}

std::optional<EntityEventPacket> parse_entity_event(std::span<const u8> payload) {
    io::ByteReader reader(payload);
    const auto     id     = reader.read_i32();
    const auto     status = reader.read_i8();
    if (!id || !status) {
        return std::nullopt;
    }
    return EntityEventPacket{*id, *status};
}

std::optional<HurtAnimationPacket> parse_hurt_animation(std::span<const u8> payload) {
    io::ByteReader reader(payload);
    const auto     id  = read_varint(reader);
    const auto     yaw = reader.read_f32();
    if (!id || !yaw) {
        return std::nullopt;
    }
    return HurtAnimationPacket{*id, *yaw};
}

std::optional<i32> parse_damage_event_victim(std::span<const u8> payload) {
    io::ByteReader reader(payload);
    const auto     id = read_varint(reader);
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

}  // namespace ov::net
