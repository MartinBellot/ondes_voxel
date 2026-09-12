#define OV_LOG_CATEGORY "netclient"

#include "ov/netclient/client.hpp"

#include "ov/base/log.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/chat_types.hpp"
#include "ov/protocol/client_play.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/entity_metadata.hpp"
#include "ov/protocol/breaking.hpp"        // ── breaking ──
#include "ov/protocol/effect_packets.hpp"  // ── breaking ──
#include "ov/protocol/framing.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/survival.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/world/chunk_storage.hpp"

#include <asio.hpp>

#include <algorithm>  // ── streaming ── query_spawn_progress
#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>

namespace ov::netclient {

namespace {

/// Boss Bar, clientbound 0x0B in 763 — the id our server's end fight sends
/// (src/ov_server/src/end_fight.hpp). ── music ──
constexpr i32 kBossBar = 0x0B;

/// The protocol this project speaks. Not a variable: 763 is 1.20.1, and
/// pretending to speak another version is how a client gets a chunk packet in
/// a layout it cannot read.
constexpr i32 kProtocolVersion = 763;

/// Handshake, next state 2.
constexpr i32 kStatePlay = 2;

enum class Stage : u8 { Handshaking, Login, Play, Closed };

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
    rain_level.reset();     // ── weather ──
    thunder_level.reset();  // ── weather ──
    time_frozen = false;
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
    sounds.clear();
    entity_sounds.clear();
    stop_sounds.clear();
    world_events.clear();
    dimension.reset();  // ── music ──
    biome_music.reset();
    boss_bars.clear();
    destroy_stages.clear();  // ── breaking ──
    own_entity_id.reset();
    own_effects.clear();
    explosions.clear();
    op_level.reset();  // ── allow-commands ──
    pickups.clear();
    death_message.reset();  // ── screens ──
    respawned = false;
    hardcore.reset();
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

    // ── breaking ── The Player Action sequence, and this player's entity id
    // (network thread only: Login (play) writes it, the effect packets read it).
    std::atomic<i32> sequence{0};
    i32              own_entity_id{-1};

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
            // Client Information waits for Login (play): see there.
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
            inbox.time_frozen = *time < 0;
            break;
        }

        // ── weather ── Game Event: one byte of kind, one float of value.
        case net::clientbound::kGameEvent: {
            const auto kind  = reader.read_u8();
            const auto value = reader.read_f32();
            if (!kind || !value) {
                return;
            }
            const std::lock_guard lock(mutex);
            if (*kind == 1) {
                inbox.rain_level = 0.0F;
            } else if (*kind == 2) {
                inbox.rain_level = 1.0F;
            } else if (*kind == 7) {
                inbox.rain_level = *value;
            } else if (*kind == 8) {
                inbox.thunder_level = *value;
            } else if (*kind == 3) {
                // ── breaking ── Change Game Mode. /gamemode sends only this, so
                // without it a client stays in the mode of its Login — and a
                // survival player the client still takes for creative breaks a
                // block every six ticks that the server never lets go.
                inbox.game_mode = static_cast<u8>(*value);
            }
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
            // ── music ── the biomes' music and the dimension, after the codec.
            auto world = net::read_login_world(body);

            // Client Information, now and not at Login Success. Sent at Login
            // Success it can reach a vanilla server still decoding in the
            // login state, whose three packets make 0x08 "Index 8 out of
            // bounds for length 3" and a disconnect on join; the vanilla client
            // sends it here. Not cosmetic either: the view distance is what the
            // server sizes its chunk sending by.
            {
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
            }
            own_entity_id = *entity_id;  // ── breaking ──
            const std::lock_guard lock(mutex);
            if (world) {  // ── music ──
                inbox.dimension   = std::move(world->dimension);
                inbox.biome_music = std::move(world->music);
            }
            inbox.own_entity_id = *entity_id;  // ── breaking ──
            inbox.game_mode = *mode;
            inbox.hardcore  = *hardcore != 0;  // ── screens ──
            inbox.player_entity_id = *entity_id;  // ── entity-models ──
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
            net::ParsedMetadata parsed = net::parse_entity_metadata(reader);
            if (!parsed.complete) {
                OV_LOG_WARN("entity {}: metadata stopped at a field this client cannot skip",
                            *id);
            }
            for (const net::MetadataValue& value : parsed.values) {
                if (value.index == net::metadata::kItemStack && value.stack) {
                    change.stack = *value.stack;
                }
            }
            change.metadata = std::move(parsed.values);
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        // ── entity-models ── What an entity wears and holds, when it dies,
        // when it is hurt, and what it rides. Each is parsed by
        // ov/protocol/entity_metadata.hpp, round-trip tested against the
        // encoder the server writes it with.
        case net::clientbound::kEntityEquipment: {
            auto parsed = net::parse_entity_equipment(body);
            if (!parsed) {
                fail("malformed Set Equipment");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind      = ClientEvents::EntityChangeKind::Equipment;
            change.id        = parsed->entity_id;
            change.equipment = std::move(parsed->entries);
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kEntityEvent: {
            const auto parsed = net::parse_entity_event(body);
            if (!parsed) {
                fail("malformed Entity Event");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind   = ClientEvents::EntityChangeKind::Event;
            change.id     = parsed->entity_id;
            change.status = parsed->status;
            const std::lock_guard lock(mutex);
            // ── allow-commands ── This player's permission level rides the same
            // packet: statuses 24 + level, level 0..4.
            const int status = parsed->status;
            if (parsed->entity_id == own_entity_id && status >= 24 && status <= 28) {
                inbox.op_level = status - 24;
            }
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kHurtAnimation:
        case net::clientbound::kDamageEvent: {
            // Either one starts the red flash. Vanilla's server sends Damage
            // Event to every viewer and Hurt Animation to the victim alone, so
            // a mob seen from outside is flashed by the first.
            std::optional<i32> victim;
            if (packet_id == net::clientbound::kHurtAnimation) {
                if (const auto hurt = net::parse_hurt_animation(body)) {
                    victim = hurt->entity_id;
                }
            } else {
                victim = net::parse_damage_event_victim(body);
            }
            if (!victim) {
                fail("malformed Damage Event / Hurt Animation");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind = ClientEvents::EntityChangeKind::Hurt;
            change.id   = *victim;
            const std::lock_guard lock(mutex);
            inbox.entities.push_back(std::move(change));
            break;
        }

        case net::clientbound::kSetPassengers: {
            auto parsed = net::parse_set_passengers(body);
            if (!parsed) {
                fail("malformed Set Passengers");
                return;
            }
            ClientEvents::EntityChange change;
            change.kind   = ClientEvents::EntityChangeKind::Passengers;
            change.id     = parsed->vehicle_id;
            change.riders = std::move(parsed->riders);
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

        // ── sound ───────────────────────────────────────────────────────
        //
        // A malformed one is skipped and said so, never a disconnect: a sound
        // the client cannot read costs a sound, not the session.

        case net::clientbound::kSoundEffect: {
            auto sound = net::parse_sound_effect(body);
            if (!sound) {
                OV_LOG_WARN("malformed Sound Effect ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.sounds.push_back(std::move(*sound));
            break;
        }

        case net::clientbound::kEntitySoundEffect: {
            auto sound = net::parse_entity_sound_effect(body);
            if (!sound) {
                OV_LOG_WARN("malformed Entity Sound Effect ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.entity_sounds.push_back(std::move(*sound));
            break;
        }

        case net::clientbound::kStopSound: {
            auto stop = net::parse_stop_sound(body);
            if (!stop) {
                OV_LOG_WARN("malformed Stop Sound ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.stop_sounds.push_back(std::move(*stop));
            break;
        }

        case net::clientbound::kWorldEvent: {
            const auto event = net::parse_world_event(body);
            if (!event) {
                OV_LOG_WARN("malformed World Event ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.world_events.push_back(*event);
            break;
        }

        case kBossBar: {  // ── music ──
            // UUID, action; Add carries title, health, colour, division, then
            // the flags; Update Flags carries the flags alone.
            const auto most   = reader.read_u64();
            const auto least  = reader.read_u64();
            const auto action = net::read_varint(reader);
            if (!most || !least || !action) {
                return;
            }
            ClientEvents::BossBarChange change;
            change.most   = *most;
            change.least  = *least;
            change.action = *action;
            if (*action == 0) {
                const auto title    = net::read_string(reader);
                const auto health   = reader.read_f32();
                const auto colour   = net::read_varint(reader);
                const auto division = net::read_varint(reader);
                const auto flags    = reader.read_u8();
                if (!title || !health || !colour || !division || !flags) {
                    return;
                }
                change.flags = *flags;
            } else if (*action == 5) {
                const auto flags = reader.read_u8();
                if (!flags) {
                    return;
                }
                change.flags = *flags;
            } else if (*action != 1) {
                return;  // health, title, style: not the music's business
            }
            const std::lock_guard lock(mutex);
            inbox.boss_bars.push_back(change);
            break;
        }

        // ── breaking ──
        case net::clientbound::kSetBlockDestroyStage: {
            const auto destroy = net::parse_block_destroy_stage(body);
            if (!destroy) {
                OV_LOG_WARN("malformed Set Block Destroy Stage ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.destroy_stages.push_back(*destroy);
            break;
        }

        case net::clientbound::kEntityEffect: {
            const auto effect = net::decode_entity_effect(body);
            if (!effect) {
                OV_LOG_WARN("malformed Entity Effect ({} bytes)", body.size());
                return;
            }
            if (effect->entity_id != own_entity_id) {
                return;  // another entity's: nothing here draws it yet
            }
            const std::lock_guard lock(mutex);
            inbox.own_effects.push_back(
                ClientEvents::OwnEffect{effect->effect_id, static_cast<i32>(effect->amplifier)});
            break;
        }

        case net::clientbound::kRemoveEntityEffect: {
            const auto removed = net::decode_remove_entity_effect(body);
            if (!removed) {
                OV_LOG_WARN("malformed Remove Entity Effect ({} bytes)", body.size());
                return;
            }
            if (removed->entity_id != own_entity_id) {
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.own_effects.push_back(ClientEvents::OwnEffect{removed->effect_id, -1});
            break;
        }
        // ── end breaking ──

        case net::clientbound::kExplosion: {
            auto explosion = net::parse_explosion(body);
            if (!explosion) {
                OV_LOG_WARN("malformed Explosion ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.explosions.push_back(std::move(*explosion));
            break;
        }

        case net::clientbound::kTakeItem: {
            const auto collected = net::read_varint(reader);
            const auto collector = net::read_varint(reader);
            const auto count     = net::read_varint(reader);
            if (!collected || !collector || !count) {
                OV_LOG_WARN("malformed Take Item Entity ({} bytes)", body.size());
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.pickups.push_back(ClientEvents::Pickup{*collected, *collector, *count});
            break;
        }
        // ── end sound ───────────────────────────────────────────────────

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

        // ── screens ─────────────────────────────────────────────────────
        // Combat Death: VarInt player id, then the cause as a JSON chat
        // component (1.20.1 still sends components as JSON strings).
        case net::clientbound::kCombatDeath: {
            const auto player  = net::read_varint(reader);
            auto       message = net::read_string(reader, 262144);
            if (!player || !message) {
                OV_LOG_WARN("Combat Death did not read; the death screen has no cause");
                return;
            }
            const std::lock_guard lock(mutex);
            inbox.death_message = std::move(*message);
            break;
        }
        case net::clientbound::kRespawn: {
            auto dimension = net::read_respawn_dimension(body);  // ── music ──
            const std::lock_guard lock(mutex);
            inbox.respawned = true;
            if (dimension) {  // ── music ──
                inbox.dimension = std::move(*dimension);
            }
            break;
        }
        // ── end screens ─────────────────────────────────────────────────

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

// ── streaming ──
std::optional<i32> Client::query_spawn_progress(const std::string& host, u16 port) {
    asio::io_context        io;
    asio::ip::tcp::socket   socket(io);
    asio::error_code        error;
    asio::ip::tcp::resolver resolver(io);
    const auto              endpoints = resolver.resolve(host, std::to_string(port), error);
    if (error) {
        return std::nullopt;
    }
    asio::connect(socket, endpoints, error);
    if (error) {
        return std::nullopt;
    }
    // A server on this machine answers in microseconds; one that has stopped
    // answering must not freeze the frame that asked.
#if defined(_WIN32)
    const DWORD timeout_ms = 500;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), static_cast<int>(sizeof timeout_ms));
#else
    timeval timeout{};
    timeout.tv_usec = 500000;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 static_cast<socklen_t>(sizeof timeout));
#endif

    const auto send_frame = [&](i32 id, std::span<const u8> body) {
        io::ByteWriter packet;
        net::write_varint(packet, id);
        packet.write_bytes(body);
        io::ByteWriter frame;
        net::write_varint(frame, static_cast<i32>(packet.data().size()));
        frame.write_bytes(packet.data());
        const std::span<const u8> bytes = frame.data();
        asio::write(socket, asio::buffer(bytes.data(), bytes.size()), error);
    };
    {
        io::ByteWriter handshake;
        net::write_varint(handshake, kProtocolVersion);
        net::write_string(handshake, host);
        handshake.write_i16(static_cast<i16>(port));
        net::write_varint(handshake, 1);  // next state: status
        send_frame(0x00, handshake.data());
    }
    send_frame(0x00, {});  // Status Request
    if (error) {
        return std::nullopt;
    }

    // One frame back: a VarInt length, the packet id 0x00, one JSON string.
    const auto read_varint_byte = [&]() -> std::optional<i32> {
        i32 value = 0;
        for (i32 shift = 0; shift < 35; shift += 7) {
            u8 byte = 0;
            asio::read(socket, asio::buffer(&byte, 1), error);
            if (error) {
                return std::nullopt;
            }
            value |= static_cast<i32>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
        }
        return std::nullopt;
    };
    const auto length = read_varint_byte();
    if (!length || *length <= 0 || *length > (1 << 20)) {
        return std::nullopt;
    }
    std::string body(static_cast<usize>(*length), '\0');
    asio::read(socket, asio::buffer(body.data(), body.size()), error);
    if (error) {
        return std::nullopt;
    }
    constexpr std::string_view kKey = "\"ondesSpawnProgress\":";
    const auto                 at   = body.find(kKey);
    if (at == std::string::npos) {
        return 100;
    }
    i32 progress = 0;
    for (usize i = at + kKey.size(); i < body.size() && body[i] >= '0' && body[i] <= '9'; ++i) {
        progress = std::min(progress * 10 + (body[i] - '0'), 100);
    }
    return progress;
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
    out.time_frozen = impl_->inbox.time_frozen;
    impl_->inbox.teleport.reset();
    impl_->inbox.time_of_day.reset();
    // ── weather ── copied like every other field: the inbox is not handed out
    // whole, and a field left out here is a packet read and thrown away.
    out.rain_level    = impl_->inbox.rain_level;
    out.thunder_level = impl_->inbox.thunder_level;
    impl_->inbox.rain_level.reset();
    impl_->inbox.thunder_level.reset();
    out.containers.swap(impl_->inbox.containers);
    out.container_slots.swap(impl_->inbox.container_slots);
    out.health      = impl_->inbox.health;
    out.experience  = impl_->inbox.experience;
    out.open_screen = std::move(impl_->inbox.open_screen);
    out.close_window = impl_->inbox.close_window;
    out.game_mode    = impl_->inbox.game_mode;
    impl_->inbox.game_mode.reset();
    out.player_entity_id = impl_->inbox.player_entity_id;  // ── entity-models ──
    impl_->inbox.player_entity_id.reset();
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
    // ── screens ──
    out.death_message = std::move(impl_->inbox.death_message);
    out.respawned     = impl_->inbox.respawned;
    // ── breaking ── handed out like the rest: left out, the others' cracks,
    // this player's id and its Haste were read off the wire and thrown away —
    // the timed digs of scripts/measure_breaking.py found it, Haste II
    // counting 23 ticks where it should take 17.
    out.destroy_stages.swap(impl_->inbox.destroy_stages);
    out.own_effects.swap(impl_->inbox.own_effects);
    out.own_entity_id = impl_->inbox.own_entity_id;
    impl_->inbox.own_entity_id.reset();
    out.op_level = impl_->inbox.op_level;  // ── allow-commands ──
    impl_->inbox.op_level.reset();
    out.hardcore      = impl_->inbox.hardcore;
    // ── music ──
    out.dimension   = std::move(impl_->inbox.dimension);
    out.biome_music = std::move(impl_->inbox.biome_music);
    out.boss_bars.swap(impl_->inbox.boss_bars);
    impl_->inbox.dimension.reset();
    impl_->inbox.biome_music.reset();
    impl_->inbox.death_message.reset();
    impl_->inbox.respawned = false;
    impl_->inbox.hardcore.reset();
}

void Client::send_respawn() {  // ── screens ──
    io::ByteWriter writer;
    net::write_varint(writer, 0);  // perform respawn
    impl_->send_raw(net::serverbound::kClientCommand, writer.data());
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
    // ── breaking ── The archive: "Incremented with each action".
    const i32  sequence = impl_->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto body     = net::encode_player_action(status, net::WirePosition{x, y, z},
                                                    static_cast<u8>(face), sequence);
    impl_->send_raw(net::serverbound::kPlayerAction, body);
}

void Client::send_swing(bool off_hand) {  // ── breaking ──
    const auto body = net::encode_swing_arm(off_hand ? net::Hand::Off : net::Hand::Main);
    impl_->send_raw(net::serverbound::kSwingArm, body);
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
