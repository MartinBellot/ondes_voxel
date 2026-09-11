// The client's side of the entity packets: reading what the server wrote.
//
// entity.hpp holds the encoders the server uses. This file holds the reverse,
// for the one reader the project has — our own client — and for the tests that
// round-trip every field through both. Nothing here interprets a value: index
// 17 is a sheep's fleece on a sheep and a pig's saddle on a pig, and which one
// it is belongs to whoever knows the entity's type. This only guarantees that
// the bytes were walked with the right width, so that a field is never read out
// of the middle of the one before it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net {

namespace clientbound {
/// Set Passengers. A vehicle id and the ids now riding it, the whole list every
/// time: a dismount is the same packet with the rider missing. Confirmed
/// against the real server by the minecart work (rails_session.cpp).
inline constexpr i32 kSetPassengers = 0x59;
}  // namespace clientbound

/// One metadata field, decoded to the width its type declares.
///
/// Deliberately a flat record rather than a variant: every consumer asks for
/// "the integer at index 17" or "the text at index 2", and a flat record lets
/// it do so without knowing which of fourteen integer-carrying wire types the
/// server chose. `type` is kept so a consumer that does care can check.
struct MetadataValue {
    u8           index{0};
    MetadataType type{MetadataType::Byte};
    /// Byte, Boolean, VarInt, VarLong, Direction, BlockState, OptionalBlockState
    /// (0 = absent), Pose, the three variant types, SnifferState, and
    /// OptionalUnsignedInt (-1 when absent; the wire's value + 1 undone).
    i64 integer{0};
    /// Float.
    f32 real{0.0F};
    /// String, Component, and a present OptionalComponent: the raw JSON of a
    /// component, not its flattened text. Flattening needs a language table,
    /// which this layer does not have.
    std::string text;
    /// False for an absent OptionalComponent, OptionalBlockPos, OptionalUuid
    /// or OptionalGlobalPos. True otherwise.
    bool present{true};
    /// Rotations and Vector3 in the first three, Quaternion in all four.
    std::array<f32, 4> vector{};
    /// VillagerData: type, profession, level, as the three varints arrive.
    std::array<i32, 3> villager{};
    /// BlockPos and a present OptionalBlockPos, packed as on the wire.
    i64 position{0};
    /// ItemStack.
    std::optional<ItemStack> stack;
};

/// Everything a Set Entity Metadata body said, in the order it said it.
struct ParsedMetadata {
    std::vector<MetadataValue> values;
    /// False when the walk stopped early: a field whose width this reader
    /// cannot know (a compound tag or a particle), or a truncated body. What
    /// was read before the stop is kept and true — vanilla writes the fields
    /// in index order, so a prefix is still a correct prefix.
    bool complete{true};
};

/// Walk the `(index, type, value)*` list up to its 0xFF terminator.
[[nodiscard]] ParsedMetadata parse_entity_metadata(io::ByteReader& reader);

/// The six equipment slots, numbered as Set Equipment numbers them.
enum class EquipmentSlot : u8 {
    MainHand = 0,
    OffHand  = 1,
    Feet     = 2,
    Legs     = 3,
    Chest    = 4,
    Head     = 5,
};
inline constexpr usize kEquipmentSlots = 6;

struct EquipmentEntry {
    EquipmentSlot slot{EquipmentSlot::MainHand};
    ItemStack     stack;
};

/// Set Equipment (0x55): an entity id, then entries whose slot byte carries a
/// continuation flag in its top bit — set on every entry but the last.
struct EntityEquipment {
    i32                         entity_id{0};
    std::vector<EquipmentEntry> entries;
};

[[nodiscard]] std::vector<u8> encode_entity_equipment(i32                             entity_id,
                                                      std::span<const EquipmentEntry> entries);
[[nodiscard]] std::optional<EntityEquipment> parse_entity_equipment(std::span<const u8> payload);

struct Passengers {
    i32              vehicle_id{0};
    std::vector<i32> riders;
};

[[nodiscard]] std::vector<u8>           encode_set_passengers(const Passengers& passengers);
[[nodiscard]] std::optional<Passengers> parse_set_passengers(std::span<const u8> payload);

/// Entity Event: the fixed i32 id and one status byte (encode_entity_event).
struct EntityEventPacket {
    i32 entity_id{0};
    i8  status{0};
};
[[nodiscard]] std::optional<EntityEventPacket> parse_entity_event(std::span<const u8> payload);

/// The entity statuses this client acts on. Named here rather than as magic
/// numbers at the call site; each is the byte the server writes
/// (server.cpp, fire_session.cpp and projectiles.cpp all send 3 on a death).
namespace entity_status {
/// A living entity died: the death animation starts.
inline constexpr i8 kDeath = 3;
}  // namespace entity_status

/// Hurt Animation (0x21): which entity, and the direction of the blow.
struct HurtAnimationPacket {
    i32 entity_id{0};
    f32 yaw{0.0F};
};
[[nodiscard]] std::optional<HurtAnimationPacket> parse_hurt_animation(std::span<const u8> payload);

/// Damage Event (0x18): only the victim is kept. The type, the attacker and
/// the source position decide sounds and particles, not how the victim looks.
[[nodiscard]] std::optional<i32> parse_damage_event_victim(std::span<const u8> payload);

}  // namespace ov::net
