#define OV_LOG_CATEGORY "netclient"

#include "ov/netclient/client.hpp"

#include "ov/base/log.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/chat_types.hpp"
#include "ov/protocol/client_play.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/framing.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/survival.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/world/chunk_storage.hpp"

#include <asio.hpp>

#include <atomic>
#include <mutex>
#include <thread>

namespace ov::netclient {

namespace {

/// The protocol this project speaks. Not a variable: 763 is 1.20.1, and
/// pretending to speak another version is how a client gets a chunk packet in
/// a layout it cannot read.
constexpr i32 kProtocolVersion = 763;

/// Handshake, next state 2.
constexpr i32 kStatePlay = 2;

enum class Stage : u8 { Handshaking, Login, Play, Closed };

/// Walk a Set Entity Metadata body, keeping the fields this client understands.
///
/// The format is `(index, type, value)*` terminated by 0xFF, with **no length
/// prefix per field**. That is the whole reason this function is written out
/// rather than reaching for the one field it wants: a value skipped by the
/// wrong width shifts everything after it, and the next index reads as data
/// from the middle of a float. Two types cannot be skipped without a parser
/// this module does not have — a compound tag and a particle — and meeting one
/// stops the walk instead of guessing its length.
///
/// Returns false when it stopped early. What was read before that is kept:
/// vanilla puts the fields in index order, so a prefix is still true.
[[nodiscard]] bool read_metadata(io::ByteReader& reader, ClientEvents::EntityChange& out) {
    for (;;) {
        const auto index = reader.read_u8();
        if (!index) {
            return false;
        }
        if (*index == 0xFF) {
            return true;
        }
        const auto type = net::read_varint(reader);
        if (!type) {
            return false;
        }
        switch (static_cast<net::MetadataType>(*type)) {
            case net::MetadataType::Byte:
            case net::MetadataType::Boolean:
                if (!reader.skip(1)) {
                    return false;
                }
                break;
            case net::MetadataType::Float:
                if (!reader.skip(4)) {
                    return false;
                }
                break;
            case net::MetadataType::VarInt:
            case net::MetadataType::VarLong:
            case net::MetadataType::Direction:
            case net::MetadataType::BlockState:
            case net::MetadataType::OptionalBlockState:
            case net::MetadataType::OptionalUnsignedInt:
            case net::MetadataType::Pose:
            case net::MetadataType::CatVariant:
            case net::MetadataType::FrogVariant:
            case net::MetadataType::PaintingVariant:
            case net::MetadataType::SnifferState:
                if (!net::read_varint(reader)) {
                    return false;
                }
                break;
            case net::MetadataType::String:
            case net::MetadataType::Component:
                if (!net::read_string(reader)) {
                    return false;
                }
                break;
            case net::MetadataType::OptionalComponent: {
                const auto present = reader.read_u8();
                if (!present) {
                    return false;
                }
                if (*present != 0 && !net::read_string(reader)) {
                    return false;
                }
                break;
            }
            case net::MetadataType::ItemStack: {
                auto stack = net::read_slot(reader);
                if (!stack) {
                    return false;
                }
                if (*index == net::metadata::kItemStack) {
                    out.stack = std::move(*stack);
                }
                break;
            }
            case net::MetadataType::Rotations:
            case net::MetadataType::Vector3:
                if (!reader.skip(12)) {
                    return false;
                }
                break;
            case net::MetadataType::BlockPos:
                if (!reader.skip(8)) {
                    return false;
                }
                break;
            case net::MetadataType::OptionalBlockPos:
            case net::MetadataType::OptionalUuid: {
                const auto present = reader.read_u8();
                if (!present) {
                    return false;
                }
                const usize width =
                    static_cast<net::MetadataType>(*type) == net::MetadataType::OptionalUuid ? 16
                                                                                             : 8;
                if (*present != 0 && !reader.skip(width)) {
                    return false;
                }
                break;
            }
            case net::MetadataType::VillagerData:
                for (int field = 0; field < 3; ++field) {
                    if (!net::read_varint(reader)) {
                        return false;
                    }
                }
                break;
            case net::MetadataType::OptionalGlobalPos: {
                const auto present = reader.read_u8();
                if (!present) {
                    return false;
                }
                if (*present != 0 && (!net::read_string(reader) || !reader.skip(8))) {
                    return false;
                }
                break;
            }
            case net::MetadataType::Quaternion:
                if (!reader.skip(16)) {
                    return false;
                }
                break;
            case net::MetadataType::CompoundTag:
            case net::MetadataType::Particle:
                // Refused by name rather than skipped by a guessed width.
                return false;
        }
    }
}

}  // namespace

std::string_view to_string(ClientError error) noexcept {
    switch (error) {
        case ClientError::CannotConnect:
            return "nothing is listening there";
        case ClientError::Disconnected:
            return "the connection closed";
        case ClientError::Rejected:
            return "the server refused the login";
    }
    return "unknown";
}

void ClientEvents::clear() {
    loaded.clear();
    unloaded.clear();
    changed.clear();
    teleport.reset();
    time_of_day.reset();
    health.reset();
    experience.reset();
    containers.clear();
    container_slots.clear();
    open_screen.reset();
    close_window.reset();
    game_mode.reset();
    abilities.reset();  // ── flight ──
    entities.clear();
    chat.clear();  // ── chat ──
    chat_types.reset();
    commands.reset();
    suggestions.clear();
}

struct Client::Impl {
    ClientDesc desc;

    asio::io_context      io;
    asio::ip::tcp::socket socket{io};
    std::thread           thread;

    net::FrameDecoder decoder;
    i32               threshold{net::kNoCompression};
    Stage             stage{Stage::Handshaking};

    std::atomic<bool> running{true};
    std::atomic<bool> playing{false};

    /// Guards everything below. Held only to move finished objects across, never
    /// while parsing and never while a socket call is in flight.
    mutable std::mutex mutex;
    ClientEvents       inbox;
    std::string        reason;

    world::WorldShape shape{world::WorldShape::overworld()};
    world::AirStates  air;

    std::array<u8, 16384> read_buffer{};

    // ── chat ──  The salt of an unsigned message: nothing checks it offline,
    // and vanilla sends a random one, so a splitmix sequence stands in for it.
    u64 salt_state{0x9E3779B97F4A7C15ULL};

    void send_raw(i32 packet_id, std::span<const u8> body);
    void handle(i32 packet_id, std::span<const u8> body);
    void handle_login(i32 packet_id, std::span<const u8> body);
    void handle_play(i32 packet_id, std::span<const u8> body);
    void start_read();
    void fail(std::string why);
};

void Client::Impl::fail(std::string why) {
    if (!running.exchange(false)) {
        return;
    }
    {
        const std::lock_guard lock(mutex);
        if (reason.empty()) {
            reason = std::move(why);
        }
    }
    playing = false;
    asio::error_code ignored;
    (void)socket.close(ignored);
}

void Client::Impl::send_raw(i32 packet_id, std::span<const u8> body) {
    if (!running) {
        return;
    }
    auto framed = net::encode_packet(packet_id, body, threshold);
    if (!framed) {
        fail("could not frame a packet");
        return;
    }
    // Posted rather than written here: send_position is called from the frame
    // thread, and asio sockets are not safe to touch from two at once.
    auto payload = std::make_shared<std::vector<u8>>(std::move(*framed));
    asio::post(io, [this, payload]() {
        if (!running) {
            return;
        }
        asio::error_code error;
        asio::write(socket, asio::buffer(*payload), error);
        if (error) {
            fail("write failed: " + error.message());
        }
    });
}

void Client::Impl::handle_login(i32 packet_id, std::span<const u8> body) {
    switch (packet_id) {
        case 0x00: {  // Disconnect
            io::ByteReader reader(body);
            auto           text = net::read_string(reader);
            fail(text ? *text : "the server refused the login");
            break;
        }
        case 0x02: {  // Login Success — from here on the stream is Play
            stage   = Stage::Play;
            playing = true;
            OV_LOG_INFO("logged in as {}", desc.username);

            // Client Information. Not cosmetic: the view distance here is what
            // the server sizes its chunk sending by, and a server that thinks
            // it is 2 sends nine chunks and stops.
            io::ByteWriter writer;
            net::write_string(writer, "en_gb");
            writer.write_u8(desc.view_distance);
            net::write_varint(writer, 0);  // chat mode: enabled
            writer.write_u8(1);            // chat colours
            writer.write_u8(0x7F);         // every skin part shown
            net::write_varint(writer, 1);  // main hand: right
            writer.write_u8(0);            // no text filtering
            writer.write_u8(1);            // listed in the player list
            send_raw(net::serverbound::kClientInformation, writer.data());
            break;
        }
        case 0x03: {  // Set Compression
            io::ByteReader reader(body);
            const auto     value = net::read_varint(reader);
            if (!value) {
                fail("malformed Set Compression");
                return;
            }
            threshold = *value;
            decoder.set_compression_threshold(*value);
            break;
        }
        default:
            // Login Plugin Request and anything else: ignored. An offline
            // login has no encryption step to answer.
            break;
    }
}

void Client::Impl::handle_play(i32 packet_id, std::span<const u8> body) {
    io::ByteReader reader(body);

    switch (packet_id) {
        case net::clientbound::kKeepAlive: {
            const auto id = reader.read_i64();
            if (!id) {
                fail("malformed Keep Alive");
                return;
            }
            io::ByteWriter writer;
            writer.write_i64(*id);
            send_raw(net::serverbound::kKeepAlive, writer.data());
            break;
        }

        case net::clientbound::kSynchronizePosition: {
            const auto x     = reader.read_f64();
            const auto y     = reader.read_f64();
            const auto z     = reader.read_f64();
            const auto yaw   = reader.read_f32();
            const auto pitch = reader.read_f32();
            const auto flags = reader.read_u8();
            const auto id    = net::read_varint(reader);
            if (!x || !y || !z || !yaw || !pitch || !flags || !id) {
                fail("malformed Synchronize Position");
                return;
            }
            // Confirming is not optional. A server with an unacknowledged
            // teleport outstanding silently ignores every block interaction
            // while still accepting digging — which is exactly the failure
            // that cost this project a day from the other side of the wire.
            io::ByteWriter writer;
            net::write_varint(writer, *id);
            send_raw(net::serverbound::kConfirmTeleport, writer.data());

            PlayerInput moved;
            moved.position = Vec3d{*x, *y, *z};
            moved.yaw      = *yaw;
            moved.pitch    = *pitch;
            const std::lock_guard lock(mutex);
            inbox.teleport = moved;
            break;
        }

        case net::clientbound::kChunkDataAndLight: {
            auto chunk = net::parse_chunk_data(body, shape, air, desc.registry);
            if (!chunk) {
                OV_LOG_WARN("a chunk packet would not parse; dropping it");
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.loaded.push_back(std::make_unique<world::Chunk>(std::move(*chunk)));
            break;
        }

        case net::clientbound::kUnloadChunk: {
            const auto x = reader.read_i32();
            const auto z = reader.read_i32();
            if (!x || !z) {
                fail("malformed Unload Chunk");
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.unloaded.emplace_back(*x, *z);
            break;
        }

        case net::clientbound::kBlockUpdate: {
            const auto packed = net::read_position_raw(reader);
            const auto state  = net::read_varint(reader);
            if (!packed || !state) {
                fail("malformed Block Update");
                return;
            }
            const auto position = net::unpack_position(*packed);
            ClientEvents::BlockChange change;
            change.x     = position.x;
            change.y     = position.y;
            change.z     = position.z;
            change.state = registry::BlockStateId{static_cast<u16>(*state)};
            const std::lock_guard lock(mutex);
            inbox.changed.push_back(change);
            break;
        }

        case net::clientbound::kUpdateTime: {
            const auto world_age = reader.read_i64();
            const auto time      = reader.read_i64();
            if (!world_age || !time) {
                return;
            }
            const std::lock_guard lock(mutex);
            // Negative means the cycle is frozen; the magnitude is still the
            // time, so it is the absolute value that matters.
            inbox.time_of_day = *time < 0 ? -*time : *time;
            break;
        }

        case net::clientbound::kLoginPlay: {
            // Only the first three fields, and only for the game mode: the
            // registry codec that follows is the client's business and this
            // client hard-codes what it needs. Entity id (Int), hardcore
            // (Bool), then game mode (Unsigned Byte).
            const auto entity_id = reader.read_i32();
            const auto hardcore  = reader.read_u8();
            const auto mode      = reader.read_u8();
            if (!entity_id || !hardcore || !mode) {
                return;
            }
            // ── chat ──  The chat types are in the codec: a Player Chat
            // Message names one by its index there.
            auto chat_types = net::read_login_chat_types(body);
            if (!chat_types) {
                OV_LOG_WARN("Login (play): the registry codec did not read; chat types unknown");
            }
            const std::lock_guard lock(mutex);
            inbox.game_mode = *mode;
            if (chat_types) {
                inbox.chat_types = std::move(*chat_types);
            }
            break;
        }

        case net::clientbound::kPlayerAbilities: {  // ── flight ──
            // Flags (Byte), Flying Speed (Float), Field of View Modifier
            // (Float), in the order the server's encoder writes them.
            const auto flags  = reader.read_u8();
            const auto flying = reader.read_f32();
            const auto walk   = reader.read_f32();
            if (!flags || !flying || !walk) {
                return;
            }
            ClientEvents::Abilities granted;
            granted.invulnerable  = (*flags & 0x01) != 0;
            granted.flying        = (*flags & 0x02) != 0;
            granted.may_fly       = (*flags & 0x04) != 0;
            granted.instant_build = (*flags & 0x08) != 0;
            granted.flying_speed  = *flying;
            granted.walk_speed    = *walk;
            const std::lock_guard lock(mutex);
            inbox.abilities = granted;
            break;
        }

        // ── What a renderer reads: everything that moves ────────────────
        //
        // Eight packets, all of them things the server has been sending since
        // mobs landed. Every one is decoded in the field order the encoder in
        // ov/protocol/{play,entity,survival}.hpp writes, and the two orders
        // that differ between packets are called out where they bite:
        // Spawn Entity writes **pitch before yaw**, the delta packets write
        // yaw before pitch.

        case net::clientbound::kSpawnEntity: {
            const auto id       = net::read_varint(reader);
            const auto uuid     = net::read_uuid(reader);
            const auto type     = net::read_varint(reader);
            const auto x        = reader.read_f64();
            const auto y        = reader.read_f64();
            const auto z        = reader.read_f64();
            const auto pitch    = net::read_angle(reader);
            const auto yaw      = net::read_angle(reader);
            const auto head_yaw = net::read_angle(reader);
            const auto data     = net::read_varint(reader);
            if (!id || !uuid || !type || !x || !y || !z || !pitch || !yaw || !head_yaw ||
                !data) {
                fail("malformed Spawn Entity");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind     = ClientEvents::EntityChangeKind::Spawn;
            change.id       = *id;
            change.type     = *type;
            change.position = Vec3d{*x, *y, *z};
            change.pitch    = *pitch;
            change.yaw      = *yaw;
            change.head_yaw = *head_yaw;
            change.data     = *data;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kSpawnPlayer: {
            const auto id    = net::read_varint(reader);
            const auto uuid  = net::read_uuid(reader);
            const auto x     = reader.read_f64();
            const auto y     = reader.read_f64();
            const auto z     = reader.read_f64();
            const auto yaw   = net::read_angle(reader);
            const auto pitch = net::read_angle(reader);
            if (!id || !uuid || !x || !y || !z || !yaw || !pitch) {
                fail("malformed Spawn Player");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind = ClientEvents::EntityChangeKind::Spawn;
            change.id   = *id;
            // A player has no type id on the wire: the packet *is* the type.
            // Resolving the number here rather than in the renderer keeps the
            // renderer's switch over one registry instead of two.
            change.type     = ClientEvents::kSpawnedAsPlayer;
            change.position = Vec3d{*x, *y, *z};
            change.yaw      = *yaw;
            change.head_yaw = *yaw;
            change.pitch    = *pitch;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kSpawnExperienceOrb: {
            const auto id    = net::read_varint(reader);
            const auto x     = reader.read_f64();
            const auto y     = reader.read_f64();
            const auto z     = reader.read_f64();
            const auto value = reader.read_i16();
            if (!id || !x || !y || !z || !value) {
                fail("malformed Spawn Experience Orb");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind     = ClientEvents::EntityChangeKind::Spawn;
            change.id       = *id;
            change.type     = ClientEvents::kSpawnedAsExperienceOrb;
            change.position = Vec3d{*x, *y, *z};
            change.data     = *value;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kEntityPosition:
        case net::clientbound::kEntityPositionRotation: {
            const auto id = net::read_varint(reader);
            const auto dx = reader.read_i16();
            const auto dy = reader.read_i16();
            const auto dz = reader.read_i16();
            if (!id || !dx || !dy || !dz) {
                fail("malformed Update Entity Position");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind = ClientEvents::EntityChangeKind::Move;
            change.id   = *id;
            // 1/4096 of a block. Measured, not read: see the comment on
            // encode_entity_position. Undoing the quantisation with the wrong
            // divisor is a drift nothing in the protocol can notice.
            constexpr f64 kDeltaUnit = 1.0 / 4096.0;
            change.position          = Vec3d{static_cast<f64>(*dx) * kDeltaUnit,
                                             static_cast<f64>(*dy) * kDeltaUnit,
                                             static_cast<f64>(*dz) * kDeltaUnit};
            if (packet_id == net::clientbound::kEntityPositionRotation) {
                const auto yaw   = net::read_angle(reader);
                const auto pitch = net::read_angle(reader);
                if (!yaw || !pitch) {
                    fail("malformed Update Entity Position and Rotation");
                    return;
                }
                change.yaw      = *yaw;
                change.pitch    = *pitch;
                change.head_yaw = *yaw;
                change.data     = 1;  // the rotation fields are meaningful
            }
            const auto on_ground = reader.read_u8();
            change.on_ground     = on_ground && *on_ground != 0;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kEntityRotation: {
            const auto id    = net::read_varint(reader);
            const auto yaw   = net::read_angle(reader);
            const auto pitch = net::read_angle(reader);
            if (!id || !yaw || !pitch) {
                fail("malformed Update Entity Rotation");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind     = ClientEvents::EntityChangeKind::Move;
            change.id       = *id;
            change.yaw      = *yaw;
            change.pitch    = *pitch;
            change.head_yaw = *yaw;
            change.data     = 1;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kEntityTeleport: {
            const auto id    = net::read_varint(reader);
            const auto x     = reader.read_f64();
            const auto y     = reader.read_f64();
            const auto z     = reader.read_f64();
            const auto yaw   = net::read_angle(reader);
            const auto pitch = net::read_angle(reader);
            if (!id || !x || !y || !z || !yaw || !pitch) {
                fail("malformed Teleport Entity");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind      = ClientEvents::EntityChangeKind::Teleport;
            change.id        = *id;
            change.position  = Vec3d{*x, *y, *z};
            change.yaw       = *yaw;
            change.pitch     = *pitch;
            change.head_yaw  = *yaw;
            const auto on_ground = reader.read_u8();
            change.on_ground     = on_ground && *on_ground != 0;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kEntityHeadRotation: {
            const auto id  = net::read_varint(reader);
            const auto yaw = net::read_angle(reader);
            if (!id || !yaw) {
                fail("malformed Entity Head Rotation");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind     = ClientEvents::EntityChangeKind::HeadRotation;
            change.id       = *id;
            change.head_yaw = *yaw;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kRemoveEntities: {
            const auto count = net::read_varint(reader);
            if (!count || *count < 0) {
                fail("malformed Remove Entities");
                return;
            }
            const std::lock_guard lock(mutex);
            for (i32 index = 0; index < *count; ++index) {
                const auto id = net::read_varint(reader);
                if (!id) {
                    fail("malformed Remove Entities");
                    return;
                }
                ClientEvents::EntityChange change;
                change.kind = ClientEvents::EntityChangeKind::Remove;
                change.id   = *id;
                inbox.entities.push_back(std::move(change));
            }
            break;
        }

        case net::clientbound::kEntityMetadata: {
            const auto id = net::read_varint(reader);
            if (!id) {
                fail("malformed Set Entity Metadata");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind = ClientEvents::EntityChangeKind::Metadata;
            change.id   = *id;
            // The metadata format has no length prefix per field, so a type
            // this client cannot skip means the rest of the packet is
            // unreadable. It stops there rather than guessing — the alternative
            // is decoding another field's bytes as this one's value, which
            // looks like data instead of like an error.
            if (!read_metadata(reader, change)) {
                OV_LOG_WARN("entity {}: metadata stopped at a field this client cannot skip",
                            *id);
            }
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        // ── What the interface reads ────────────────────────────────────
        //
        // Five packets the server has been sending since survival landed and
        // nothing on this side was listening to. Each is parsed by
        // ov/protocol/client_play.hpp, which is round-trip tested against the
        // encoder the server writes them with.

        case net::clientbound::kSetHealth: {
            const auto health = net::parse_set_health(body);
            if (!health) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.health = *health;
            break;
        }

        case net::clientbound::kSetExperience: {
            const auto experience = net::parse_set_experience(body);
            if (!experience) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.experience = *experience;
            break;
        }

        case net::clientbound::kContainerContent: {
            auto content = net::parse_container_content(body);
            if (!content) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.containers.push_back(std::move(*content));
            break;
        }

        case net::clientbound::kContainerSlot: {
            auto slot = net::parse_container_slot(body);
            if (!slot) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.container_slots.push_back(std::move(*slot));
            break;
        }

        case net::clientbound::kOpenScreen: {
            auto screen = net::parse_open_screen(body);
            if (!screen) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.open_screen = std::move(*screen);
            break;
        }

        case net::clientbound::kCloseContainer: {
            const auto window = net::parse_clientbound_close_container(body);
            if (!window) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.close_window = *window;
            break;
        }

        case net::clientbound::kDisconnect: {
            auto text = net::read_string(reader);
            fail(text ? *text : "disconnected");
            break;
        }

        // ── chat ────────────────────────────────────────────────────────
        //
        // Read with the protocol's own parsers, the ones the server's
        // encoders are round-tripped against. A packet that does not parse
        // is logged and dropped: the stream is framed, so the next packet is
        // still readable.
        case net::clientbound::kSystemChat: {
            auto chat = net::parse_system_chat(body);
            if (!chat) {
                OV_LOG_WARN("malformed System Chat Message ({} bytes)", body.size());
                return;
            }
            ClientEvents::ChatEvent event;
            event.kind    = ClientEvents::ChatEvent::Kind::System;
            event.json    = std::move(chat->content);
            event.overlay = chat->overlay;
            const std::lock_guard lock(mutex);
            inbox.chat.push_back(std::move(event));
            break;
        }
        case net::clientbound::kPlayerChat: {
            auto chat = net::parse_player_chat(body);
            if (!chat) {
                OV_LOG_WARN("malformed Player Chat Message ({} bytes)", body.size());
                return;
            }
            ClientEvents::ChatEvent event;
            event.kind          = ClientEvents::ChatEvent::Kind::Player;
            event.body          = std::move(chat->body);
            event.unsigned_json = std::move(chat->unsigned_content);
            event.chat_type     = chat->chat_type;
            event.sender_json   = std::move(chat->name_json);
            event.target_json   = std::move(chat->target_json);
            const std::lock_guard lock(mutex);
            inbox.chat.push_back(std::move(event));
            break;
        }
        case net::clientbound::kDisguisedChat: {
            auto chat = net::parse_disguised_chat(body);
            if (!chat) {
                OV_LOG_WARN("malformed Disguised Chat Message ({} bytes)", body.size());
                return;
            }
            ClientEvents::ChatEvent event;
            event.kind        = ClientEvents::ChatEvent::Kind::Disguised;
            event.json        = std::move(chat->message_json);
            event.chat_type   = chat->chat_type;
            event.sender_json = std::move(chat->name_json);
            event.target_json = std::move(chat->target_json);
            const std::lock_guard lock(mutex);
            inbox.chat.push_back(std::move(event));
            break;
        }
        case net::clientbound::kSetTitleText:
        case net::clientbound::kSetSubtitleText:
        case net::clientbound::kSetActionBarText: {
            auto text = net::read_string(reader, net::kMaxChatComponentLength);
            if (!text) {
                return;
            }
            ClientEvents::ChatEvent event;
            event.kind = packet_id == net::clientbound::kSetTitleText
                             ? ClientEvents::ChatEvent::Kind::Title
                             : (packet_id == net::clientbound::kSetSubtitleText
                                    ? ClientEvents::ChatEvent::Kind::Subtitle
                                    : ClientEvents::ChatEvent::Kind::ActionBar);
            event.json = std::move(*text);
            const std::lock_guard lock(mutex);
            inbox.chat.push_back(std::move(event));
            break;
        }
        case net::clientbound::kSetTitleAnimationTimes: {
            const auto fade_in  = reader.read_i32();
            const auto stay     = reader.read_i32();
            const auto fade_out = reader.read_i32();
            if (!fade_in || !stay || !fade_out) {
                return;
            }
            ClientEvents::ChatEvent event;
            event.kind     = ClientEvents::ChatEvent::Kind::TitleTimes;
            event.fade_in  = *fade_in;
            event.stay     = *stay;
            event.fade_out = *fade_out;
            const std::lock_guard lock(mutex);
            inbox.chat.push_back(std::move(event));
            break;
        }
        case net::clientbound::kClearTitles: {
            const auto reset = reader.read_u8();
            if (!reset) {
                return;
            }
            ClientEvents::ChatEvent event;
            event.kind  = ClientEvents::ChatEvent::Kind::ClearTitles;
            event.reset = *reset != 0;
            const std::lock_guard lock(mutex);
            inbox.chat.push_back(std::move(event));
            break;
        }
        case net::clientbound::kCommands: {
            auto graph = net::parse_commands(body);
            if (!graph) {
                OV_LOG_WARN("malformed Commands packet ({} bytes); no completion", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.commands = std::move(*graph);
            break;
        }
        case net::clientbound::kCommandSuggestions: {
            auto response = net::parse_suggestions_response(body);
            if (!response) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.suggestions.push_back(std::move(*response));
            break;
        }
        // ── end chat ────────────────────────────────────────────────────

        default:
            // Everything else — entities, inventory, sound — is not needed to
            // stand in a world and see it. Ignoring by default rather than
            // failing is right for a client: an unknown packet is a feature it
            // has not implemented, not a corrupt stream.
            break;
    }
}

void Client::Impl::handle(i32 packet_id, std::span<const u8> body) {
    switch (stage) {
        case Stage::Login:
            handle_login(packet_id, body);
            break;
        case Stage::Play:
            handle_play(packet_id, body);
            break;
        default:
            break;
    }
}

void Client::Impl::start_read() {
    socket.async_read_some(
        asio::buffer(read_buffer), [this](const asio::error_code& error, usize count) {
            if (error) {
                fail(error == asio::error::eof ? "the server closed the connection"
                                               : error.message());
                return;
            }
            decoder.feed(std::span(read_buffer.data(), count));
            while (running) {
                auto packet = decoder.next();
                if (!packet) {
                    if (packet.error() == net::FrameError::Incomplete) {
                        break;
                    }
                    fail(std::string("bad frame: ") + std::string(to_string(packet.error())));
                    return;
                }
                handle(packet->id, packet->body);
            }
            if (running) {
                start_read();
            }
        });
}

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    impl_->running = false;
    impl_->io.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

std::expected<std::unique_ptr<Client>, ClientError> Client::connect(const ClientDesc& desc) {
    if (desc.registry == nullptr) {
        return std::unexpected(ClientError::CannotConnect);
    }

    std::unique_ptr<Client> self(new Client);
    Impl&                   impl = *self->impl_;
    impl.desc                    = desc;
    impl.air                     = world::AirStates::from(*desc.registry);

    asio::error_code           error;
    asio::ip::tcp::resolver    resolver(impl.io);
    const auto                 endpoints =
        resolver.resolve(desc.host, std::to_string(desc.port), error);
    if (error) {
        OV_LOG_ERROR("cannot resolve {}:{}: {}", desc.host, desc.port, error.message());
        return std::unexpected(ClientError::CannotConnect);
    }
    asio::connect(impl.socket, endpoints, error);
    if (error) {
        OV_LOG_ERROR("cannot connect to {}:{}: {}", desc.host, desc.port, error.message());
        return std::unexpected(ClientError::CannotConnect);
    }
    // Small writes are movement packets twenty times a second. Nagle would
    // hold each one back waiting for company it will not get.
    impl.socket.set_option(asio::ip::tcp::no_delay(true), error);

    // Handshake, then Login Start, back to back. There is nothing to wait for
    // in between: the server reads them in order off the same stream.
    {
        io::ByteWriter writer;
        net::write_varint(writer, kProtocolVersion);
        net::write_string(writer, desc.host);
        writer.write_i16(static_cast<i16>(desc.port));
        net::write_varint(writer, kStatePlay);
        impl.send_raw(0x00, writer.data());
    }
    impl.stage = Stage::Login;
    {
        io::ByteWriter writer;
        net::write_string(writer, desc.username);
        // The uuid is advisory — an offline server derives its own from the
        // name — but sending the one it will derive keeps the two in step.
        writer.write_u8(1);
        net::write_uuid(writer, net::Uuid::offline_player(desc.username));
        impl.send_raw(0x00, writer.data());
    }

    impl.start_read();
    impl.thread = std::thread([&impl]() {
        // The socket blocks; a frame must not. Everything this thread produces
        // crosses to the caller through one queue, and the caller stays the
        // only writer of the world it builds.
        impl.io.run();
        impl.running = false;
        impl.playing = false;
    });

    return self;
}

void Client::poll(ClientEvents& out) {
    out.clear();
    const std::lock_guard lock(impl_->mutex);
    out.loaded.swap(impl_->inbox.loaded);
    out.unloaded.swap(impl_->inbox.unloaded);
    out.changed.swap(impl_->inbox.changed);
    out.entities.swap(impl_->inbox.entities);
    out.teleport    = impl_->inbox.teleport;
    out.time_of_day = impl_->inbox.time_of_day;
    impl_->inbox.teleport.reset();
    impl_->inbox.time_of_day.reset();
    out.containers.swap(impl_->inbox.containers);
    out.container_slots.swap(impl_->inbox.container_slots);
    out.health      = impl_->inbox.health;
    out.experience  = impl_->inbox.experience;
    out.open_screen = std::move(impl_->inbox.open_screen);
    out.close_window = impl_->inbox.close_window;
    out.game_mode    = impl_->inbox.game_mode;
    impl_->inbox.game_mode.reset();
    out.abilities = impl_->inbox.abilities;  // ── flight ──
    impl_->inbox.abilities.reset();
    impl_->inbox.health.reset();
    impl_->inbox.experience.reset();
    impl_->inbox.open_screen.reset();
    impl_->inbox.close_window.reset();
    // ── chat ──
    out.chat.swap(impl_->inbox.chat);
    out.suggestions.swap(impl_->inbox.suggestions);
    out.chat_types = std::move(impl_->inbox.chat_types);
    out.commands   = std::move(impl_->inbox.commands);
    impl_->inbox.chat_types.reset();
    impl_->inbox.commands.reset();
}

void Client::send_abilities(bool flying) {  // ── flight ──
    io::ByteWriter writer;
    writer.write_u8(flying ? 0x02 : 0x00);
    impl_->send_raw(net::serverbound::kPlayerAbilities, writer.data());
}

bool Client::in_game() const noexcept {
    return impl_->playing;
}

bool Client::connected() const noexcept {
    return impl_->running;
}

std::string Client::disconnect_reason() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->reason;
}

void Client::send_position(const PlayerInput& input) {
    io::ByteWriter writer;
    writer.write_f64(input.position.x);
    writer.write_f64(input.position.y);
    writer.write_f64(input.position.z);
    writer.write_f32(input.yaw);
    writer.write_f32(input.pitch);
    writer.write_u8(input.on_ground ? 1 : 0);
    impl_->send_raw(net::serverbound::kSetPlayerPositionRot, writer.data());
}

void Client::send_dig(i32 x, i32 y, i32 z, i32 status, i32 face) {
    io::ByteWriter writer;
    net::write_varint(writer, status);
    net::write_position(writer, net::WirePosition{x, y, z});
    writer.write_u8(static_cast<u8>(face));
    net::write_varint(writer, 0);  // sequence
    impl_->send_raw(net::serverbound::kPlayerAction, writer.data());
}

void Client::send_place(i32 x, i32 y, i32 z, i32 face, f32 cursor_x, f32 cursor_y, f32 cursor_z) {
    io::ByteWriter writer;
    net::write_varint(writer, 0);  // main hand
    net::write_position(writer, net::WirePosition{x, y, z});
    net::write_varint(writer, face);
    writer.write_f32(cursor_x);
    writer.write_f32(cursor_y);
    writer.write_f32(cursor_z);
    writer.write_u8(0);            // not inside a block
    net::write_varint(writer, 0);  // sequence
    impl_->send_raw(net::serverbound::kUseItemOn, writer.data());
}

void Client::send_creative_slot(i16 slot, i32 item_id, i8 count, std::span<const u8> nbt) {
    // Through net::write_slot rather than by hand. The hand-rolled version
    // this replaces wrote the present flag, the id and the count and then
    // *stopped*: a Slot ends with its NBT, and an absent tag is a TAG_End
    // byte, not nothing. A vanilla server reading that would take the next
    // packet's first byte as the tag type and never recover.
    io::ByteWriter writer;
    writer.write_i16(slot);
    net::ItemStack stack;
    if (item_id > 0 && count > 0) {
        stack.item_id = item_id;
        stack.count   = count;
        stack.nbt.assign(nbt.begin(), nbt.end());
    }
    net::write_slot(writer, stack);
    impl_->send_raw(net::serverbound::kSetCreativeSlot, writer.data());
}

void Client::send_container_click(u8 window_id, i32 state_id, i16 slot, i8 button, i32 mode,
                                 const net::ItemStack& carried) {
    // The changed-slot array is sent empty. A vanilla server reads it and
    // ignores it, ours reads past it, and filling it would mean predicting the
    // result of a click the server has not applied yet — which is the one
    // thing an authoritative window must not do.
    impl_->send_raw(net::serverbound::kClickContainer,
                    net::encode_container_click(window_id, state_id, slot, button, mode, {},
                                                carried));
}

void Client::send_close_container(u8 window_id) {
    impl_->send_raw(net::serverbound::kCloseContainer,
                    net::encode_serverbound_close_container(window_id));
}

void Client::send_held_slot(i16 slot) {
    io::ByteWriter writer;
    writer.write_i16(slot);
    impl_->send_raw(net::serverbound::kSetHeldItem, writer.data());
}

// ── chat ──
namespace {

/// Milliseconds since the epoch: the timestamp vanilla puts on a message. A
/// wall clock, and rightly — it is what the message says about when it was
/// sent, and it never reaches a simulation.
[[nodiscard]] i64 now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] i64 next_salt(u64& state) {
    state += 0x9E3779B97F4A7C15ULL;
    u64 z = state;
    z     = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return static_cast<i64>(z ^ (z >> 31U));
}

}  // namespace

void Client::send_chat_message(std::string_view message) {
    net::ChatMessage chat;
    chat.message   = std::string(message);
    chat.timestamp = now_millis();
    chat.salt      = next_salt(impl_->salt_state);
    impl_->send_raw(net::serverbound::kChatMessage, net::encode_chat_message(chat));
}

void Client::send_chat_command(std::string_view command) {
    net::ChatCommand chat;
    chat.command   = std::string(command);
    chat.timestamp = now_millis();
    chat.salt      = next_salt(impl_->salt_state);
    impl_->send_raw(net::serverbound::kChatCommand, net::encode_chat_command(chat));
}

void Client::send_suggestions_request(i32 transaction, std::string_view text) {
    impl_->send_raw(net::serverbound::kCommandSuggestionsRequest,
                    net::encode_suggestions_request(
                        net::SuggestionsRequest{transaction, std::string(text)}));
}
// ── end chat ──

}  // namespace ov::netclient
