#include "ov/protocol/entity.hpp"

#include "ov/protocol/varint.hpp"

#include <algorithm>
#include <cmath>

namespace ov::net {
namespace {

/// Degrees to the protocol's byte of 1/256 turns.
///
/// The wrap is deliberate and the cast chain is what makes it defined: 350
/// degrees and -10 are the same direction, and clamping instead would pin a
/// player looking slightly west to due south. Duplicated from play.cpp on
/// purpose — the two files are compiled separately and a shared private helper
/// would need a header that is not part of the module's surface.
[[nodiscard]] i8 angle_byte(f32 degrees) noexcept {
    const auto steps = static_cast<i32>(std::lround(static_cast<f64>(degrees) * 256.0 / 360.0));
    return static_cast<i8>(static_cast<u8>(steps & 0xFF));
}

/// Blocks per tick to the wire's 1/8000ths, saturating.
///
/// Saturating rather than wrapping: a velocity past four blocks a tick is rare
/// and a wrapped one sends the entity the other way, which looks like a bug in
/// the physics rather than in the encoder.
[[nodiscard]] i16 velocity_units(f64 blocks_per_tick) noexcept {
    const f64 scaled = std::round(blocks_per_tick * 8000.0);
    return static_cast<i16>(std::clamp(scaled, -32768.0, 32767.0));
}

/// One block to the delta packets' 1/4096ths.
///
/// Measured, not assumed: a mob teleported three quarters of a block produced
/// 3072, and one moved -1.125 produced -4608.
inline constexpr f64 kDeltaUnitsPerBlock = 4096.0;

[[nodiscard]] i16 delta_units(f64 blocks) noexcept {
    return static_cast<i16>(
        std::clamp(std::round(blocks * kDeltaUnitsPerBlock), -32768.0, 32767.0));
}

}  // namespace

// ── MetadataWriter ──────────────────────────────────────────────────────────

void MetadataWriter::header(u8 index, MetadataType type) {
    writer_.write_u8(index);
    write_varint(writer_, static_cast<i32>(type));
    ++fields_;
}

MetadataWriter& MetadataWriter::byte_value(u8 index, i8 value) {
    header(index, MetadataType::Byte);
    writer_.write_i8(value);
    return *this;
}

MetadataWriter& MetadataWriter::varint_value(u8 index, i32 value) {
    header(index, MetadataType::VarInt);
    write_varint(writer_, value);
    return *this;
}

MetadataWriter& MetadataWriter::float_value(u8 index, f32 value) {
    header(index, MetadataType::Float);
    writer_.write_f32(value);
    return *this;
}

MetadataWriter& MetadataWriter::boolean_value(u8 index, bool value) {
    header(index, MetadataType::Boolean);
    writer_.write_u8(value ? 1 : 0);
    return *this;
}

MetadataWriter& MetadataWriter::string_value(u8 index, std::string_view value) {
    header(index, MetadataType::String);
    write_string(writer_, value);
    return *this;
}

MetadataWriter& MetadataWriter::component_value(u8 index, std::string_view json) {
    header(index, MetadataType::Component);
    write_string(writer_, json);
    return *this;
}

MetadataWriter& MetadataWriter::optional_component_value(u8                              index,
                                                         std::optional<std::string_view> json) {
    header(index, MetadataType::OptionalComponent);
    writer_.write_u8(json ? 1 : 0);
    if (json) {
        write_string(writer_, *json);
    }
    return *this;
}

MetadataWriter& MetadataWriter::item_value(u8 index, const ItemStack& stack) {
    header(index, MetadataType::ItemStack);
    write_slot(writer_, stack);
    return *this;
}

MetadataWriter& MetadataWriter::rotations_value(u8 index, f32 x, f32 y, f32 z) {
    header(index, MetadataType::Rotations);
    writer_.write_f32(x);
    writer_.write_f32(y);
    writer_.write_f32(z);
    return *this;
}

MetadataWriter& MetadataWriter::block_pos_value(u8 index, WirePosition position) {
    header(index, MetadataType::BlockPos);
    write_position(writer_, position);
    return *this;
}

MetadataWriter& MetadataWriter::optional_block_pos_value(u8                          index,
                                                         std::optional<WirePosition> position) {
    header(index, MetadataType::OptionalBlockPos);
    writer_.write_u8(position ? 1 : 0);
    if (position) {
        write_position(writer_, *position);
    }
    return *this;
}

MetadataWriter& MetadataWriter::direction_value(u8 index, i32 direction) {
    header(index, MetadataType::Direction);
    write_varint(writer_, direction);
    return *this;
}

MetadataWriter& MetadataWriter::optional_uuid_value(u8 index, const std::optional<Uuid>& uuid) {
    header(index, MetadataType::OptionalUuid);
    writer_.write_u8(uuid ? 1 : 0);
    if (uuid) {
        write_uuid(writer_, *uuid);
    }
    return *this;
}

MetadataWriter& MetadataWriter::block_state_value(u8 index, i32 state) {
    header(index, MetadataType::BlockState);
    write_varint(writer_, state);
    return *this;
}

MetadataWriter& MetadataWriter::optional_block_state_value(u8 index, i32 state) {
    header(index, MetadataType::OptionalBlockState);
    write_varint(writer_, state);
    return *this;
}

MetadataWriter& MetadataWriter::villager_data_value(u8 index, i32 type, i32 profession,
                                                    i32 level) {
    header(index, MetadataType::VillagerData);
    write_varint(writer_, type);
    write_varint(writer_, profession);
    write_varint(writer_, level);
    return *this;
}

MetadataWriter& MetadataWriter::optional_unsigned_int_value(u8 index, std::optional<i32> value) {
    header(index, MetadataType::OptionalUnsignedInt);
    // Absent is zero and present is value + 1, so a real zero still travels.
    write_varint(writer_, value ? *value + 1 : 0);
    return *this;
}

MetadataWriter& MetadataWriter::pose_value(u8 index, i32 pose) {
    header(index, MetadataType::Pose);
    write_varint(writer_, pose);
    return *this;
}

MetadataWriter& MetadataWriter::vector3_value(u8 index, f32 x, f32 y, f32 z) {
    header(index, MetadataType::Vector3);
    writer_.write_f32(x);
    writer_.write_f32(y);
    writer_.write_f32(z);
    return *this;
}

MetadataWriter& MetadataWriter::quaternion_value(u8 index, f32 x, f32 y, f32 z, f32 w) {
    header(index, MetadataType::Quaternion);
    writer_.write_f32(x);
    writer_.write_f32(y);
    writer_.write_f32(z);
    writer_.write_f32(w);
    return *this;
}

std::vector<u8> MetadataWriter::take() {
    // 0xFF and not 0xFE or a count: the format has no length, so the terminator
    // is the only thing that tells the client the list ended. Omitting it makes
    // the client read the next packet's bytes as another field.
    writer_.write_u8(0xFF);
    fields_ = 0;
    return writer_.take();
}

std::vector<u8> encode_entity_metadata(i32 entity_id, std::span<const u8> fields) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_bytes(fields);
    return writer.take();
}

// ── Appearance and movement ─────────────────────────────────────────────────

std::vector<u8> encode_spawn_entity(const SpawnEntity& spawn) {
    io::ByteWriter writer;
    write_varint(writer, spawn.entity_id);
    write_uuid(writer, spawn.uuid);
    write_varint(writer, spawn.type);
    writer.write_f64(spawn.x);
    writer.write_f64(spawn.y);
    writer.write_f64(spawn.z);
    // Pitch before yaw, then head yaw. Captured in that order from a real
    // server; the opposite order is the single most common way to make every
    // spawned mob face somewhere plausible and wrong.
    writer.write_i8(angle_byte(spawn.pitch));
    writer.write_i8(angle_byte(spawn.yaw));
    writer.write_i8(angle_byte(spawn.head_yaw));
    write_varint(writer, spawn.data);
    writer.write_i16(velocity_units(spawn.velocity_x));
    writer.write_i16(velocity_units(spawn.velocity_y));
    writer.write_i16(velocity_units(spawn.velocity_z));
    return writer.take();
}

bool fits_in_delta(f64 dx, f64 dy, f64 dz) noexcept {
    // Strictly less than 32768 after scaling, on every axis. A delta of exactly
    // eight blocks scales to 32768, one past an i16, and would wrap the entity
    // eight blocks the other way.
    const auto fits = [](f64 value) {
        return std::abs(std::round(value * kDeltaUnitsPerBlock)) <= 32767.0;
    };
    return fits(dx) && fits(dy) && fits(dz);
}

f64 quantised_delta(f64 blocks) noexcept {
    return static_cast<f64>(delta_units(blocks)) / kDeltaUnitsPerBlock;
}

std::vector<u8> encode_entity_position(i32 entity_id, f64 dx, f64 dy, f64 dz, bool on_ground) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_i16(delta_units(dx));
    writer.write_i16(delta_units(dy));
    writer.write_i16(delta_units(dz));
    writer.write_u8(on_ground ? 1 : 0);
    return writer.take();
}

std::vector<u8> encode_entity_position_rotation(i32 entity_id, f64 dx, f64 dy, f64 dz, f32 yaw,
                                                f32 pitch, bool on_ground) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_i16(delta_units(dx));
    writer.write_i16(delta_units(dy));
    writer.write_i16(delta_units(dz));
    // Yaw before pitch here, unlike Spawn Entity. That is not a mistake in one
    // of the two: the packets genuinely disagree, and both orders were taken
    // from captures rather than from a summary.
    writer.write_i8(angle_byte(yaw));
    writer.write_i8(angle_byte(pitch));
    writer.write_u8(on_ground ? 1 : 0);
    return writer.take();
}

std::vector<u8> encode_entity_rotation(i32 entity_id, f32 yaw, f32 pitch, bool on_ground) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_i8(angle_byte(yaw));
    writer.write_i8(angle_byte(pitch));
    writer.write_u8(on_ground ? 1 : 0);
    return writer.take();
}

std::vector<u8> encode_entity_velocity(i32 entity_id, f64 x, f64 y, f64 z) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_i16(velocity_units(x));
    writer.write_i16(velocity_units(y));
    writer.write_i16(velocity_units(z));
    return writer.take();
}

std::vector<u8> encode_remove_entities(std::span<const i32> entity_ids) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(entity_ids.size()));
    for (const i32 id : entity_ids) {
        write_varint(writer, id);
    }
    return writer.take();
}

// ── Being hit ───────────────────────────────────────────────────────────────

std::vector<u8> encode_entity_animation(i32 entity_id, u8 animation) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_u8(animation);
    return writer.take();
}

std::vector<u8> encode_entity_event(i32 entity_id, i8 status) {
    io::ByteWriter writer;
    // A fixed i32, not a varint. The only entity packet in the protocol that
    // does this; writing a varint here shortens the packet by three bytes for
    // small ids and the client reads the status out of the wrong place.
    writer.write_i32(entity_id);
    writer.write_i8(status);
    return writer.take();
}

std::vector<u8> encode_damage_event(i32 entity_id, i32 damage_type, std::optional<i32> cause,
                                    std::optional<i32> direct) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    write_varint(writer, damage_type);
    // Both are id + 1, with zero for absent. Captured: unattributed damage
    // arrives with both fields zero.
    write_varint(writer, cause ? *cause + 1 : 0);
    write_varint(writer, direct ? *direct + 1 : 0);
    writer.write_u8(0);  // no source position
    return writer.take();
}

std::vector<u8> encode_hurt_animation(i32 entity_id, f32 yaw) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    // A full float of degrees, not the byte angle the movement packets use.
    writer.write_f32(yaw);
    return writer.take();
}

// ── Attributes ──────────────────────────────────────────────────────────────

std::vector<u8> encode_update_attributes(i32                             entity_id,
                                         std::span<const AttributeValue> attributes) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    write_varint(writer, static_cast<i32>(attributes.size()));
    for (const AttributeValue& attribute : attributes) {
        write_string(writer, attribute.name);
        writer.write_f64(attribute.value);
        write_varint(writer, 0);  // no modifiers
    }
    return writer.take();
}

}  // namespace ov::net
