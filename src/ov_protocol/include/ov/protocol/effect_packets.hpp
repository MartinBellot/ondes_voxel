// The three packets a status effect travels in: Entity Effect, Remove Entity
// Effect, and Update Attributes with its modifiers.
//
// Every byte layout here was **captured from a real 1.20.1 server** by
// scripts/measure_effects.py (campaign `packets`) and is frozen in
// tests/test_effect_packets.cpp. Each packet was identified by its *payload* —
// the bot's entity id followed by the effect id just named at the console —
// never by the id a table promised. Two things the capture settled that a
// reading of the spec would not have:
//
//   * `effect give … true` (hide particles) clears the icon bit too. The flags
//     byte came back 0x00, not 0x04: the command's constructor ties the icon to
//     the particles.
//   * Darkness carries its factor data as a network NBT compound that still
//     has a root name (two zero bytes after the 0x0A) — 1.20.1 is before the
//     nameless network NBT of 1.20.2.
//
// Update Attributes carries only the attributes that changed: giving speed sent
// one property (movement speed) with one modifier, not the player's eight.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/protocol/types.hpp"

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::net {

/// The flags byte of Entity Effect.
namespace effect_flags {
inline constexpr u8 kAmbient  = 0x01;
inline constexpr u8 kVisible  = 0x02;
inline constexpr u8 kShowIcon = 0x04;
}  // namespace effect_flags

/// Darkness's fade state, as the client wants it at the moment the effect is
/// sent. Captured for a fresh darkness: everything zero, target 1.0, padding
/// 22 ticks.
struct EffectFactorData {
    i32  padding_duration{22};
    f32  factor_start{0.0F};
    f32  factor_target{1.0F};
    f32  factor_current{0.0F};
    i32  ticks_active{0};
    f32  factor_previous_frame{0.0F};
    bool had_effect_last_tick{false};

    friend bool operator==(const EffectFactorData&, const EffectFactorData&) noexcept = default;
};

/// Entity Effect (0x6C).
struct EntityEffect {
    i32 entity_id{0};
    /// The `minecraft:mob_effect` id — **1-based**, speed is 1.
    i32 effect_id{0};
    /// A byte on the wire. Amplifier 255 travels as 0xFF.
    u8 amplifier{0};
    /// Ticks, or -1 for infinite (sent as the five-byte varint of -1).
    i32                             duration{0};
    u8                              flags{0};
    std::optional<EffectFactorData> factor;

    friend bool operator==(const EntityEffect&, const EntityEffect&) noexcept = default;
};

[[nodiscard]] std::vector<u8> encode_entity_effect(const EntityEffect& packet);

enum class EffectPacketError : u8 { Truncated, BadVarInt, BadNbt, TrailingBytes };

[[nodiscard]] std::expected<EntityEffect, EffectPacketError> decode_entity_effect(
    std::span<const u8> payload);

/// Remove Entity Effect (0x3F): the entity id and the effect id, both varints.
[[nodiscard]] std::vector<u8> encode_remove_entity_effect(i32 entity_id, i32 effect_id);

struct RemoveEntityEffect {
    i32 entity_id{0};
    i32 effect_id{0};
};

[[nodiscard]] std::expected<RemoveEntityEffect, EffectPacketError> decode_remove_entity_effect(
    std::span<const u8> payload);

/// One modifier as the wire carries it: no name, just identity, amount and
/// operation (0 add, 1 multiply base, 2 multiply total).
struct WireModifier {
    Uuid uuid{};
    f64  amount{0.0};
    u8   operation{0};

    friend bool operator==(const WireModifier&, const WireModifier&) noexcept = default;
};

/// One attribute with its modifiers. The value field is the **base**, not the
/// computed total: captured, a player under speed II arrives with 0.1 and a
/// modifier of 0.4, and the client multiplies it out itself.
struct AttributeProperty {
    std::string_view                 name;
    f64                              base{0.0};
    std::span<const WireModifier>    modifiers;
};

/// Update Attributes (0x6A), with modifiers.
[[nodiscard]] std::vector<u8> encode_update_attributes_full(
    i32 entity_id, std::span<const AttributeProperty> properties);

/// The decoded form, owning its strings. For tests and for a client.
struct DecodedProperty {
    std::string               name;
    f64                       base{0.0};
    std::vector<WireModifier> modifiers;
};

struct DecodedUpdateAttributes {
    i32                          entity_id{0};
    std::vector<DecodedProperty> properties;
};

[[nodiscard]] std::expected<DecodedUpdateAttributes, EffectPacketError> decode_update_attributes(
    std::span<const u8> payload);

}  // namespace ov::net
