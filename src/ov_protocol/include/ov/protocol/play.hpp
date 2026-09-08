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
#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/types.hpp"
#include "ov/world/chunk.hpp"

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
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
inline constexpr i32 kAcknowledgeDig      = 0x06;
inline constexpr i32 kBlockUpdate         = 0x0A;
inline constexpr i32 kUnloadChunk         = 0x1E;
inline constexpr i32 kSpawnPlayer         = 0x03;
inline constexpr i32 kRemoveEntities      = 0x3E;
inline constexpr i32 kPlayerInfoRemove    = 0x39;
inline constexpr i32 kPlayerInfoUpdate    = 0x3A;
inline constexpr i32 kEntityHeadRotation  = 0x42;
inline constexpr i32 kEntityTeleport      = 0x68;
inline constexpr i32 kBlockEntityData     = 0x08;
inline constexpr i32 kOpenSignEditor      = 0x31;
inline constexpr i32 kOpenScreen          = 0x30;
inline constexpr i32 kContainerContent    = 0x12;
inline constexpr i32 kContainerSlot       = 0x14;
inline constexpr i32 kCloseContainer      = 0x11;
inline constexpr i32 kUpdateTime          = 0x5E;
inline constexpr i32 kSpawnEntity         = 0x01;
inline constexpr i32 kEntityMetadata      = 0x52;
/// Take Item Entity — the pickup animation, then the item is removed.
inline constexpr i32 kTakeItem = 0x67;
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
inline constexpr i32 kPlayerAction         = 0x1D;
inline constexpr i32 kSetHeldItem          = 0x28;
inline constexpr i32 kSetCreativeSlot      = 0x2B;
inline constexpr i32 kUseItemOn            = 0x31;
inline constexpr i32 kUpdateSign           = 0x2E;
inline constexpr i32 kClickContainer       = 0x0B;
inline constexpr i32 kCloseContainer       = 0x0C;
}  // namespace serverbound

/// A block position as it travels: 26 bits x, 26 bits z, 12 bits y, all signed.
///
/// Declared before everything else because both directions use it — the server
/// names a block in an update, the client names one it clicked.
struct WirePosition {
    i32 x{0};
    i32 y{0};
    i32 z{0};
};

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

/// Tell the client to forget a chunk.
///
/// Note the field order: **x then z**, unlike the chunk packet's own header
/// which is also x then z but which people routinely mirror wrongly. Sending
/// them swapped unloads a chunk the player is standing in and keeps one they
/// have left, which looks like chunks failing at random.
[[nodiscard]] std::vector<u8> encode_unload_chunk(i32 chunk_x, i32 chunk_z);

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

/// Tell a client one block entity changed.
///
/// `type` is the numeric id from minecraft:block_entity_type — one of the
/// registries the client hard-codes, so it is Mojang's number and not ours.
[[nodiscard]] std::vector<u8> encode_block_entity_data(WirePosition position, i32 type,
                                                       const nbt::Tag& data);

/// Ask the client to open its sign editor. Sent when a sign is placed; without
/// it a fresh sign can never be written on.
[[nodiscard]] std::vector<u8> encode_open_sign_editor(WirePosition position, bool front);

/// The four lines a player typed on one side of a sign.
struct SignUpdate {
    WirePosition               position;
    bool                       front{true};
    std::array<std::string, 4> lines;
};

[[nodiscard]] std::optional<SignUpdate> parse_update_sign(std::span<const u8> payload);

// ── Containers ──────────────────────────────────────────────────────────────

/// One stack in a slot.
///
/// An empty slot is `count == 0`, not a separate representation: the wire has a
/// present flag and everything above it treats absence as an empty stack, so
/// keeping two ways to say "nothing" only invites them to disagree.
struct ItemStack {
    i32 item_id{0};
    i8  count{0};

    /// The item's NBT, kept as raw bytes rather than parsed.
    ///
    /// The server does not read enchantments or names yet, and re-encoding a
    /// tag it does not understand is how data gets lost. Passing the bytes
    /// through unchanged cannot.
    std::vector<u8> nbt;

    [[nodiscard]] bool empty() const noexcept { return count <= 0; }
};

void                                   write_slot(io::ByteWriter& writer, const ItemStack& stack);
[[nodiscard]] std::optional<ItemStack> read_slot(io::ByteReader& reader);

/// Ask the client to open a container screen.
///
/// `type` is an index into minecraft:menu — another registry the client
/// hard-codes. A generic 9x3 chest is not the same number as a 9x6 one, and
/// getting it wrong opens a window of the wrong size over the right data.
[[nodiscard]] std::vector<u8> encode_open_screen(i32 window_id, i32 type, std::string_view title);

/// Every slot of an open window at once.
[[nodiscard]] std::vector<u8> encode_container_content(u8 window_id, i32 state_id,
                                                       std::span<const ItemStack> slots,
                                                       const ItemStack&           carried);

[[nodiscard]] std::vector<u8> encode_container_slot(i8 window_id, i32 state_id, i16 slot,
                                                    const ItemStack& stack);

[[nodiscard]] std::vector<u8> encode_close_container(u8 window_id);

/// A click inside a window.
///
/// `mode` is the interesting field: 0 is an ordinary click, 1 a shift-click, 2
/// a number key, 5 a drag, 6 a double-click. They are entirely different
/// operations sharing one packet, and treating them alike duplicates items.
struct ContainerClick {
    u8        window_id{0};
    i32       state_id{0};
    i16       slot{0};
    i8        button{0};
    i32       mode{0};
    ItemStack carried;
};

[[nodiscard]] std::optional<ContainerClick> parse_container_click(std::span<const u8> payload);

[[nodiscard]] std::optional<u8> parse_close_container(std::span<const u8> payload);

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

/// Breaking a block.
///
/// `status` 0 is "started digging", which in creative means the block is gone
/// already — the client has removed it locally and is telling us. 2 is
/// "finished digging" in survival. Both have to act, or creative mode does
/// nothing and survival breaks nothing.
struct PlayerAction {
    i32          status{0};
    WirePosition position;
    i8           face{0};
    i32          sequence{0};
};

[[nodiscard]] std::optional<PlayerAction> parse_player_action(std::span<const u8> payload);

/// Placing a block against a face of an existing one.
///
/// The position is the block clicked, not where the new block goes: the face
/// says which side, and the new block lands one step along it. Placing at the
/// clicked position instead replaces whatever was aimed at.
struct UseItemOn {
    WirePosition position;
    i32          face{0};

    /// Where on the face the player clicked, 0..1 along each axis. Only the
    /// vertical one is used so far, and it is what decides whether a slab is a
    /// top or a bottom — clicking the upper half of a block's side places the
    /// upper slab.
    f32 cursor_x{0.0F};
    f32 cursor_y{0.0F};
    f32 cursor_z{0.0F};

    i32 sequence{0};
};

[[nodiscard]] std::optional<UseItemOn> parse_use_item_on(std::span<const u8> payload);

/// The block one step along `face` from `position`. Faces are ordered
/// -Y, +Y, -Z, +Z, -X, +X, which is the order the protocol uses everywhere.
[[nodiscard]] WirePosition offset_by_face(WirePosition position, i32 face) noexcept;

/// What the client put in a creative slot. An empty slot carries no item.
struct CreativeSlot {
    i16                slot{0};
    std::optional<i32> item_id;
    i8                 count{0};
};

[[nodiscard]] std::optional<CreativeSlot> parse_set_creative_slot(std::span<const u8> payload);

[[nodiscard]] std::optional<i16> parse_set_held_item(std::span<const u8> payload);

/// Add a player to the tab list.
///
/// Not cosmetic: the client builds its player *entity* from this list. A spawn
/// packet for a uuid it has never been told about produces an entity with no
/// name and no skin, or none at all — so this always goes first.
[[nodiscard]] std::vector<u8> encode_player_info_add(const Uuid& uuid, std::string_view name,
                                                     i32 game_mode);

[[nodiscard]] std::vector<u8> encode_player_info_remove(const Uuid& uuid);

/// The entity type a dropped stack is, in Mojang's registry.
inline constexpr i32 kItemEntityType = 54;

/// Make a non-player entity appear. Only what an item needs is exposed: no
/// rotation, no velocity, because a dropped stack has neither that matters.
///
/// Ids captured from a real 1.20.1 server rather than read off a summary: the
/// archived wiki gave the wrong number for every packet checked against it.
[[nodiscard]] std::vector<u8> encode_spawn_entity(i32 entity_id, const Uuid& uuid, i32 type, f64 x,
                                                  f64 y, f64 z);

/// Tell the client what a dropped entity is holding.
///
/// Metadata index 8, type 7 — a Slot. Without it the entity exists and renders
/// as nothing at all, which looks exactly like a packet that never arrived.
[[nodiscard]] std::vector<u8> encode_item_metadata(i32 entity_id, i32 item_id, i8 count);

/// The pickup animation: the stack flies to the collector before vanishing.
///
/// Cosmetic, and the client still needs the removal afterwards — but without it
/// items blink out of existence a block away from the player.
[[nodiscard]] std::vector<u8> encode_take_item(i32 collected, i32 collector, i32 count);

/// Make another player appear in the world.
[[nodiscard]] std::vector<u8> encode_spawn_player(i32 entity_id, const Uuid& uuid, f64 x, f64 y,
                                                  f64 z, f32 yaw, f32 pitch);

/// Move an entity to an absolute position.
///
/// Vanilla prefers relative moves for small steps, which cost six bytes instead
/// of twenty-eight. Absolute is used here because it cannot drift: a relative
/// stream that loses or reorders one packet leaves the entity permanently
/// offset, and there is nothing in the protocol to notice.
[[nodiscard]] std::vector<u8> encode_entity_teleport(i32 entity_id, f64 x, f64 y, f64 z, f32 yaw,
                                                     f32 pitch, bool on_ground);

/// The head turns independently of the body, and a client told only the body
/// rotation renders a player permanently looking straight ahead.
[[nodiscard]] std::vector<u8> encode_entity_head_rotation(i32 entity_id, f32 yaw);

[[nodiscard]] std::vector<u8> encode_remove_entity(i32 entity_id);

/// Tell every client a block changed. `state` is a block **state** id.
[[nodiscard]] std::vector<u8> encode_block_update(WirePosition position, i32 state);

/// The world's age and the time of day, both in ticks.
///
/// Sent every twenty ticks in vanilla. The client needs it to place the sun,
/// and anything measuring the server's clock from outside needs it too: it is
/// the only packet that carries a tick number.
///
/// A negative time of day means the daylight cycle is frozen at its absolute
/// value, which is how vanilla says "doDaylightCycle false" without a gamerule
/// packet.
[[nodiscard]] std::vector<u8> encode_update_time(i64 world_age, i64 time_of_day);

/// Confirm a predicted change. Without this the client rolls the block back
/// after a moment, which looks like the server ignoring the player.
[[nodiscard]] std::vector<u8> encode_acknowledge_dig(i32 sequence);

}  // namespace ov::net
