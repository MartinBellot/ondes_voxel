// Accepting connections.
//
// Deliberately thin, and deliberately opaque: asio lives entirely behind this
// header. It is header-only and heavily templated, and letting it into a public
// header would put its weight into every translation unit that touches
// networking — the exact mechanism behind risk R5.
//
// The model is one thread running an event loop, with a callback per
// connection. That is enough for the status ping and for the vertical slice;
// when the tick loop and the job pool arrive, the loop moves onto its own
// thread with its own scheduling class rather than changing shape.
#pragma once

#include "ov/base/types.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ov::world {
class Chunk;
}  // namespace ov::world

namespace ov::net {

/// One client connection, from the server's point of view.
///
/// Handed to the callback as a shared pointer because a connection outlives the
/// call that created it: the handler returns immediately and the socket keeps
/// reading.
class Connection {
public:
    virtual ~Connection() = default;

    /// Queue bytes for sending. Returns immediately; the write completes on the
    /// event loop.
    virtual void send(std::span<const u8> bytes) = 0;

    /// Queue a chunk as `Chunk Data and Update Light`, from a snapshot
    /// (`world::Chunk::snapshot`) rather than from encoded bytes.
    ///
    /// Encoding a chunk is the largest piece of work in a packet, and the
    /// caller is the tick thread. The TCP connection encodes on its own thread,
    /// **in the same queue as `send`**: a packet sent after the chunk — a
    /// `Block Update` inside it — cannot overtake it. The default encodes here,
    /// on the calling thread, and calls `send`: what a connection without a
    /// thread of its own (a test double, an in-process channel) wants. Either
    /// way the bytes are those of `encode_chunk_data`, uncompressed, as the
    /// server framed them before.
    virtual void send_chunk(std::shared_ptr<const world::Chunk> chunk);

    /// Close after everything queued has been written.
    virtual void close() = 0;

    /// The peer's address, for logging.
    [[nodiscard]] virtual std::string peer_address() const = 0;

    /// Compression threshold for this connection's framing, or kNoCompression.
    virtual void set_compression_threshold(i32 threshold) = 0;
};

using ConnectionPtr = std::shared_ptr<Connection>;

/// Called once per decoded packet.
///
/// Returning false closes the connection — which is what a protocol error
/// should do, since there is no way to resynchronise a byte stream once a
/// packet has been misread.
using PacketHandler =
    std::function<bool(const ConnectionPtr&, i32 packet_id, std::span<const u8> body)>;

/// Called when a connection opens and when it closes, for logging and for
/// per-connection state.
using ConnectionHandler = std::function<void(const ConnectionPtr&)>;

/// A TCP listener that frames the Minecraft protocol.
class Listener {
public:
    /// Bind to a port. An empty address binds to every interface.
    ///
    /// Returns nullptr when the port cannot be bound — usually another server
    /// is already on 25565, which is worth saying plainly rather than exiting
    /// silently.
    [[nodiscard]] static std::unique_ptr<Listener> bind(u16 port, const std::string& address = {});

    virtual ~Listener() = default;

    void on_connect(ConnectionHandler handler) { on_connect_ = std::move(handler); }

    void on_disconnect(ConnectionHandler handler) { on_disconnect_ = std::move(handler); }

    void on_packet(PacketHandler handler) { on_packet_ = std::move(handler); }

    /// Run the event loop until stop() is called. Blocks the calling thread.
    virtual void run() = 0;

    /// Run for at most this many milliseconds, then return. For tests, and for
    /// driving the loop from a thread that has other work.
    virtual void poll_for(i64 milliseconds) = 0;

    /// Ask run() to return. Safe to call from another thread.
    virtual void stop() = 0;

    [[nodiscard]] virtual u16   port() const             = 0;
    [[nodiscard]] virtual usize connection_count() const = 0;

protected:
    ConnectionHandler on_connect_;
    ConnectionHandler on_disconnect_;
    PacketHandler     on_packet_;
};

}  // namespace ov::net
