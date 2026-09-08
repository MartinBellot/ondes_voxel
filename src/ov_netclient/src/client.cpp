#define OV_LOG_CATEGORY "netclient"

#include "ov/netclient/client.hpp"

#include "ov/base/log.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/framing.hpp"
#include "ov/protocol/play.hpp"
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

        case net::clientbound::kDisconnect: {
            auto text = net::read_string(reader);
            fail(text ? *text : "disconnected");
            break;
        }

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
    out.teleport    = impl_->inbox.teleport;
    out.time_of_day = impl_->inbox.time_of_day;
    impl_->inbox.teleport.reset();
    impl_->inbox.time_of_day.reset();
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

void Client::send_creative_slot(i16 slot, i32 item_id, i8 count) {
    io::ByteWriter writer;
    writer.write_i16(slot);
    if (item_id <= 0 || count <= 0) {
        writer.write_u8(0);
    } else {
        writer.write_u8(1);
        net::write_varint(writer, item_id);
        writer.write_u8(static_cast<u8>(count));
    }
    impl_->send_raw(net::serverbound::kSetCreativeSlot, writer.data());
}

void Client::send_held_slot(i16 slot) {
    io::ByteWriter writer;
    writer.write_i16(slot);
    impl_->send_raw(net::serverbound::kSetHeldItem, writer.data());
}

}  // namespace ov::netclient
