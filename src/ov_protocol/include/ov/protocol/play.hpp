// The Play state of protocol 763 — the packets that put a player in a world.
//
// Packet ids are not guessable and not stable across versions, so none of them
// were written from memory. They were taken from PrismarineJS/minecraft-data
// (MIT, and the source CLAUDE.md names for this) and then **confirmed against a
// real 1.20.1 server**: a client written from the spec connected to the vanilla
// jar and recorded what actually arrived. The archived wiki page disagreed on
// every id checked, which is exactly why the capture exists.
//
// Only the packets a player needs in order to join, see the world and move are
// here. Everything else is a later milestone, and a half-written packet is
// worse than an absent one.
//
// Every encoder returns the packet **body**, without the id. The id is written
// by whatever frames the packet, so that compression and length prefixing have
// exactly one implementation.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"
#include "ov/world/chunk.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::net {

/// Clientbound Play packet ids, protocol 763.
namespace clientbound {
inline constexpr i32 kGameEvent           = 0x1F;
inline constexpr i32 kKeepAlive           = 0x23;
inline constexpr i32 kChunkDataAndLight   = 0x24;
inline constexpr i32 kLoginPlay           = 0x28;
inline constexpr i32 kPlayerAbilities     = 0x34;
inline constexpr i32 kSynchronizePosition = 0x3C;
inline constexpr i32 kSetCenterChunk      = 0x4E;
inline constexpr i32 kSetDefaultSpawn     = 0x50;
inline constexpr i32 kDisconnect          = 0x1A;
}  // namespace clientbound

/// Serverbound Play packet ids, protocol 763.
namespace serverbound {
inline constexpr i32 kConfirmTeleport      = 0x00;
inline constexpr i32 kClientInformation    = 0x08;
inline constexpr i32 kPluginMessage        = 0x0D;
inline constexpr i32 kKeepAlive            = 0x12;
inline constexpr i32 kSetPlayerPosition    = 0x14;
inline constexpr i32 kSetPlayerPositionRot = 0x15;
inline constexpr i32 kSetPlayerRotation    = 0x16;
inline constexpr i32 kSetPlayerOnGround    = 0x17;
}  // namespace serverbound

/// Everything the Login (play) packet needs that is not a fixed constant.
struct LoginPlay {
    i32              entity_id{0};
    u8               game_mode{0};  // 0 survival, 1 creative, 2 adventure, 3 spectator
    std::string_view dimension_type{"minecraft:overworld"};
    std::string_view dimension_name{"minecraft:overworld"};
    i64              hashed_seed{0};
    i32              max_players{20};
    i32              view_distance{10};
    i32              simulation_distance{10};
    bool             is_flat{true};

    /// The registry codec, as raw NBT bytes.
    ///
    /// Opaque here on purpose. It is built at data-generation time from the
    /// vanilla datapack, where the JSON-to-NBT type mapping can be declared
    /// field by field; the server only has to copy it. A codec the client
    /// cannot decode drops the connection before the world appears, and the
    /// failure looks like a network error rather than a data one.
    std::span<const u8> registry_codec;
};

[[nodiscard]] std::vector<u8> encode_login_play(const LoginPlay& login);

/// Where the player is, and the teleport id they must echo back.
///
/// The client will not move until it has confirmed one of these, so this is
/// what "spawning" actually means on the wire.
[[nodiscard]] std::vector<u8> encode_synchronize_position(f64 x, f64 y, f64 z, f32 yaw, f32 pitch,
                                                          i32 teleport_id);

/// The chunk the client should treat as the centre of its loaded area.
///
/// Sent before the chunks themselves; a client that receives chunks with no
/// centre keeps them and renders nothing.
[[nodiscard]] std::vector<u8> encode_set_center_chunk(i32 chunk_x, i32 chunk_z);

[[nodiscard]] std::vector<u8> encode_keep_alive(i64 id);

[[nodiscard]] std::vector<u8> encode_set_default_spawn(i32 x, i32 y, i32 z, f32 angle);

[[nodiscard]] std::vector<u8> encode_player_abilities(bool invulnerable, bool flying,
                                                      bool allow_flying, bool creative,
                                                      f32 flying_speed, f32 walking_speed);

/// Game Event. Reason 13 is "start waiting for level chunks", which is what
/// tells the client to leave the loading screen.
[[nodiscard]] std::vector<u8> encode_game_event(u8 reason, f32 value);

[[nodiscard]] std::vector<u8> encode_play_disconnect(std::string_view reason);

/// One chunk, with its heightmaps, its sections and its light.
///
/// The largest packet in the protocol and the one with the most ways to be
/// subtly wrong. The section payload is a plain concatenation with no
/// per-section length, so a single miscounted byte shifts everything after it
/// and the client disconnects with a decode error naming no field.
[[nodiscard]] std::vector<u8> encode_chunk_data(const world::Chunk& chunk);

// ── Serverbound ─────────────────────────────────────────────────────────────

/// What the client told us about itself. Only the fields the server acts on.
struct ClientInformation {
    std::string locale;
    u8          view_distance{10};
};

[[nodiscard]] std::optional<ClientInformation> parse_client_information(
    std::span<const u8> payload);

/// A movement update. Rotation-only and on-ground-only packets leave the
/// position fields unset, which is why they are optional rather than defaulted
/// to zero — defaulting would teleport the player to the world origin.
struct PlayerMovement {
    std::optional<f64> x;
    std::optional<f64> y;
    std::optional<f64> z;
    std::optional<f32> yaw;
    std::optional<f32> pitch;
    bool               on_ground{false};
};

[[nodiscard]] std::optional<PlayerMovement> parse_movement(i32                 packet_id,
                                                           std::span<const u8> payload);

[[nodiscard]] std::optional<i32> parse_confirm_teleport(std::span<const u8> payload);
[[nodiscard]] std::optional<i64> parse_keep_alive(std::span<const u8> payload);

}  // namespace ov::net
