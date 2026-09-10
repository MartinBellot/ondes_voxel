// The entity half of the Play state: what a client is told about anything that
// is not a block.
//
// None of this was written from a summary. The packet ids come from
// PrismarineJS/minecraft-data (MIT, the source CLAUDE.md names for this) and
// were then **confirmed against a real 1.20.1 server**: a probe client written
// from the spec joined the vanilla jar, mobs were summoned next to it, and what
// arrived was written down byte for byte
// (scripts/capture_entity_packets.py). The archived wiki page has disagreed on
// every id this project has checked against it.
//
// The metadata index table below is measured the same way and for a stronger
// reason: a wrong index is not an error. It is a mob that renders with someone
// else's property, silently. So each index here was derived by setting exactly
// one NBT field on an otherwise identical zombie and reading which index moved.
// Indices that nothing measured are **absent** rather than guessed.
//
// Every encoder returns the packet body, without the id: framing, compression
// and length prefixing have exactly one implementation and it is not here.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/types.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::net {

/// Clientbound entity packet ids, protocol 763.
///
/// The ones already used elsewhere — Spawn Entity 0x01, Set Entity Metadata
/// 0x52, Remove Entities 0x3E, Head Rotation 0x42, Teleport 0x68 — live in
/// play.hpp and are not repeated.
namespace clientbound {
/// Swing an arm, or take critical-hit particles.
inline constexpr i32 kEntityAnimation = 0x04;
/// Damage Event: who hurt whom, and with what kind of damage.
inline constexpr i32 kDamageEvent = 0x18;
/// Entity Event: a one-byte status code with about eighty meanings.
inline constexpr i32 kEntityEvent = 0x1C;
/// The red flash and the direction it came from.
inline constexpr i32 kHurtAnimation = 0x21;
/// Update Entity Position — a delta, in sixteenths of a pixel.
inline constexpr i32 kEntityPosition = 0x2B;
inline constexpr i32 kEntityPositionRotation = 0x2C;
inline constexpr i32 kEntityRotation         = 0x2D;
inline constexpr i32 kEntityVelocity         = 0x54;
inline constexpr i32 kEntityEquipment        = 0x55;
inline constexpr i32 kUpdateAttributes       = 0x6A;
inline constexpr i32 kEntityEffect           = 0x6C;
inline constexpr i32 kRemoveEntityEffect     = 0x3F;
}  // namespace clientbound

// ── Entity metadata ─────────────────────────────────────────────────────────

/// The twenty-eight value types a metadata field can carry in protocol 763.
///
/// The list and its numbering come from minecraft-data's 1.20 protocol
/// description (MIT). Type 7 is confirmed independently: a real server sends a
/// dropped stack as index 8, type 7.
enum class MetadataType : i32 {
    Byte                = 0,
    VarInt              = 1,
    VarLong             = 2,
    Float               = 3,
    String              = 4,
    Component           = 5,
    OptionalComponent   = 6,
    ItemStack           = 7,
    Boolean             = 8,
    Rotations           = 9,
    BlockPos            = 10,
    OptionalBlockPos    = 11,
    Direction           = 12,
    OptionalUuid        = 13,
    BlockState          = 14,
    OptionalBlockState  = 15,
    CompoundTag         = 16,
    Particle            = 17,
    VillagerData        = 18,
    OptionalUnsignedInt = 19,
    Pose                = 20,
    CatVariant          = 21,
    FrogVariant         = 22,
    OptionalGlobalPos   = 23,
    PaintingVariant     = 24,
    SnifferState        = 25,
    Vector3             = 26,
    Quaternion          = 27,
};

/// The metadata indices this project has actually measured.
///
/// Each was found by summoning a zombie with one extra NBT field and reading
/// which index changed against a baseline zombie. The ones vanilla never sent
/// for any probe are deliberately missing: index 6 (pose), index 8 on a living
/// entity, and indices 10 to 14 have no entry here, because writing a plausible
/// number would produce a mob that looks right and behaves as something else.
namespace metadata {

/// Byte of flags on every entity. Bit 0x40 is glowing — measured: a zombie
/// summoned with `Glowing:1b` sends index 0 as the byte 64.
inline constexpr u8 kSharedFlags = 0;
/// Air, in ticks. Measured with `Air:123s`.
inline constexpr u8 kAir = 1;
/// The custom name, as an optional chat component. Measured.
inline constexpr u8 kCustomName = 2;
inline constexpr u8 kCustomNameVisible = 3;
/// Measured: `Silent:1b` moves index 4, and it is the index every summoned mob
/// in the capture carries.
inline constexpr u8 kSilent = 4;
inline constexpr u8 kNoGravity = 5;
/// Measured with `TicksFrozen:123`.
inline constexpr u8 kTicksFrozen = 7;

/// Health, a float. On every living entity, and the one index vanilla sends
/// even at its default value.
inline constexpr u8 kHealth = 9;

/// Mob flags. Measured: bit 0x01 is "no AI" and bit 0x02 is left-handed.
inline constexpr u8 kMobFlags = 15;

/// Bit 0x01 of kMobFlags.
inline constexpr i8 kMobFlagNoAi = 0x01;
/// Bit 0x02 of kMobFlags.
inline constexpr i8 kMobFlagLeftHanded = 0x02;
/// Bit 0x40 of kSharedFlags.
inline constexpr i8 kSharedFlagGlowing = 0x40;

/// The stack a dropped item carries. Captured from a real server before any of
/// the above, and the reason this whole file exists.
inline constexpr u8 kItemStack = 8;

// ── husbandry ── Measured the same way, on farm animals: one NBT field against
// a baseline of the same species (scripts/measure_husbandry.py `meta`).
/// Boolean on every ageable mob: a baby. `Age:-24000` moves it on cow, sheep,
/// pig and chicken alike.
inline constexpr u8 kAgeableBaby = 16;
/// Sheep, a byte: the wool colour in the low four bits, 0x10 sheared.
/// Measured: `Color:14b` sends 14, `Sheared:1b` sends 16.
inline constexpr u8 kSheepFleece = 17;
inline constexpr i8 kSheepSheared = 0x10;
/// Pig, a boolean: saddled. Measured with `Saddle:1b`.
inline constexpr u8 kPigSaddle = 17;

}  // namespace metadata

/// Builds the body of Set Entity Metadata: (index, type, value)*, then 0xFF.
///
/// Deliberately one method per value type rather than a variant. The metadata
/// format has no length prefix per field, so a value written with the wrong
/// type shifts everything after it and the client decodes garbage from a packet
/// that named no field. Making the type part of the call is what stops that
/// being a runtime decision.
class MetadataWriter {
public:
    MetadataWriter& byte_value(u8 index, i8 value);
    MetadataWriter& varint_value(u8 index, i32 value);
    MetadataWriter& float_value(u8 index, f32 value);
    MetadataWriter& boolean_value(u8 index, bool value);
    MetadataWriter& string_value(u8 index, std::string_view value);
    /// A chat component, as the JSON the client parses.
    MetadataWriter& component_value(u8 index, std::string_view json);
    MetadataWriter& optional_component_value(u8 index, std::optional<std::string_view> json);
    MetadataWriter& item_value(u8 index, const ItemStack& stack);
    MetadataWriter& rotations_value(u8 index, f32 x, f32 y, f32 z);
    MetadataWriter& block_pos_value(u8 index, WirePosition position);
    MetadataWriter& optional_block_pos_value(u8 index, std::optional<WirePosition> position);
    MetadataWriter& direction_value(u8 index, i32 direction);
    MetadataWriter& optional_uuid_value(u8 index, const std::optional<Uuid>& uuid);
    MetadataWriter& block_state_value(u8 index, i32 state);
    /// Zero means absent for this type, so a real state of 0 cannot be sent —
    /// which is fine, because state 0 is air and nothing carries it.
    MetadataWriter& optional_block_state_value(u8 index, i32 state);
    MetadataWriter& villager_data_value(u8 index, i32 type, i32 profession, i32 level);
    /// Absent is 0; a present value is written as value + 1.
    MetadataWriter& optional_unsigned_int_value(u8 index, std::optional<i32> value);
    MetadataWriter& pose_value(u8 index, i32 pose);
    MetadataWriter& vector3_value(u8 index, f32 x, f32 y, f32 z);
    MetadataWriter& quaternion_value(u8 index, f32 x, f32 y, f32 z, f32 w);

    [[nodiscard]] bool  empty() const noexcept { return fields_ == 0; }
    [[nodiscard]] usize field_count() const noexcept { return fields_; }

    /// The finished body, terminated by 0xFF.
    [[nodiscard]] std::vector<u8> take();

private:
    void header(u8 index, MetadataType type);

    io::ByteWriter writer_;
    usize          fields_{0};
};

/// Set Entity Metadata. `fields` is what MetadataWriter::take() produced.
[[nodiscard]] std::vector<u8> encode_entity_metadata(i32 entity_id, std::span<const u8> fields);

// ── Appearance and movement ─────────────────────────────────────────────────

/// Everything Spawn Entity carries.
///
/// The struct exists because the packet has eleven fields and four of them are
/// angles: a positional call would put pitch where yaw goes about half the time,
/// and the result is a mob facing the wrong way rather than an error.
struct SpawnEntity {
    i32  entity_id{0};
    Uuid uuid{};
    /// The type's id in minecraft:entity_type. **Mojang's**, never ours.
    i32 type{0};
    f64 x{0.0};
    f64 y{0.0};
    f64 z{0.0};
    /// Degrees. Sent as a byte of 1/256 turns, which is why 0.5 degrees is the
    /// finest thing the protocol can say about a rotation.
    f32 pitch{0.0F};
    f32 yaw{0.0F};
    f32 head_yaw{0.0F};
    /// Type-specific. A falling block's block state, a projectile's owner.
    i32 data{0};
    /// Blocks per tick. Sent in units of 1/8000 of a block per tick, clamped to
    /// an i16, so anything past about 4 blocks per tick saturates.
    f64 velocity_x{0.0};
    f64 velocity_y{0.0};
    f64 velocity_z{0.0};
};

[[nodiscard]] std::vector<u8> encode_spawn_entity(const SpawnEntity& spawn);

/// Update Entity Position: a delta, on-ground flag, no rotation.
///
/// The delta unit is **1/4096 of a block**, and that was measured rather than
/// read: a mob teleported by a known amount with its Pos read on both sides
/// produced 3072 for three quarters of a block, and -4608 for -1.125. An i16
/// therefore reaches a little under 8 blocks, which is why a longer move has to
/// become a teleport.
[[nodiscard]] std::vector<u8> encode_entity_position(i32 entity_id, f64 dx, f64 dy, f64 dz,
                                                     bool on_ground);

[[nodiscard]] std::vector<u8> encode_entity_position_rotation(i32 entity_id, f64 dx, f64 dy,
                                                              f64 dz, f32 yaw, f32 pitch,
                                                              bool on_ground);

[[nodiscard]] std::vector<u8> encode_entity_rotation(i32 entity_id, f32 yaw, f32 pitch,
                                                     bool on_ground);

/// True when a move fits in a delta packet at all.
///
/// A delta of exactly 8 blocks does not fit: 8 * 4096 is 32768, one past an
/// i16. Getting this bound wrong by one wraps the delta and sends the entity
/// eight blocks the other way.
[[nodiscard]] bool fits_in_delta(f64 dx, f64 dy, f64 dz) noexcept;

/// The distance a delta packet actually carries for a requested move.
///
/// Rounded to 1/4096, which is what the client will apply. A sender that
/// computes its next delta from the true position rather than from this throws
/// the remainder away every tick, and the client drifts with nothing in the
/// protocol to notice.
[[nodiscard]] f64 quantised_delta(f64 blocks) noexcept;

/// Velocity, in blocks per tick. Clamped to what an i16 of 1/8000ths can hold.
[[nodiscard]] std::vector<u8> encode_entity_velocity(i32 entity_id, f64 x, f64 y, f64 z);

/// Remove several entities at once. Vanilla batches them, and so does this.
[[nodiscard]] std::vector<u8> encode_remove_entities(std::span<const i32> entity_ids);

// ── Being hit ───────────────────────────────────────────────────────────────

/// Entity Animation. 0 swing main hand, 1 take damage, 2 leave bed, 3 swing
/// off hand, 4 critical effect, 5 magic critical effect.
[[nodiscard]] std::vector<u8> encode_entity_animation(i32 entity_id, u8 animation);

/// Entity Event: one byte with a different meaning per entity class.
///
/// Note the entity id is a plain **i32**, not a varint — the only entity packet
/// in the protocol that does it this way, and the reason is historical rather
/// than good.
[[nodiscard]] std::vector<u8> encode_entity_event(i32 entity_id, i8 status);

/// Damage Event.
///
/// `cause` and `direct` are entity ids or nullopt; on the wire they travel as
/// id + 1 with zero meaning absent, which is why they cannot be plain ints
/// here. A real server sending unattributed damage writes both as zero, and
/// that was captured.
[[nodiscard]] std::vector<u8> encode_damage_event(i32 entity_id, i32 damage_type,
                                                  std::optional<i32> cause,
                                                  std::optional<i32> direct);

/// The red flash. `yaw` is the direction the hit came from, in degrees.
[[nodiscard]] std::vector<u8> encode_hurt_animation(i32 entity_id, f32 yaw);

// ── Attributes ──────────────────────────────────────────────────────────────

/// One attribute and the value the client should use for it.
///
/// The name is the registry key spelled out — this is one of the few packets
/// that carries a string rather than an id. Captured: a zombie arrives with
/// `minecraft:generic.movement_speed` and the f64 0.23000000417232513, which is
/// exactly the base value `attribute … base get` reports for the type.
struct AttributeValue {
    std::string_view name;
    f64              value{0.0};
};

/// Update Attributes. Modifiers are not supported yet and none is written; a
/// caller that needs them will have to add them here rather than pack them into
/// the base value, which is the mistake this comment exists to prevent.
[[nodiscard]] std::vector<u8> encode_update_attributes(i32                          entity_id,
                                                       std::span<const AttributeValue> attributes);

}  // namespace ov::net
