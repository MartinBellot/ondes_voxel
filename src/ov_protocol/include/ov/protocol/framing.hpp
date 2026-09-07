// Packet framing: turning a byte stream into packets and back.
//
// A connection carries a stream, not messages, so every packet is prefixed with
// its length. Two formats, and which one is in use changes mid-connection:
//
//   uncompressed   length(VarInt) id(VarInt) data
//   compressed     length(VarInt) uncompressedLength(VarInt) [zlib] id data
//
// The switch happens when the server sends Set Compression during login. From
// the very next packet, both sides use the second format — and a packet under
// the threshold still uses it, with uncompressedLength written as 0 and the
// body uncompressed. Getting that special case wrong desynchronises the stream
// at exactly the point where it is hardest to debug, because it only shows up
// once a real client is talking.
//
// Everything here reads bytes from an unauthenticated peer. The length prefix
// is attacker-controlled, so it is bounded before it becomes an allocation.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"

#include <expected>
#include <span>
#include <vector>

namespace ov::net {

enum class FrameError {
    /// The buffer does not yet hold a whole packet. Not an error on a socket:
    /// it means read more and try again.
    Incomplete,
    /// Length prefix beyond what the protocol allows.
    TooLarge,
    /// Malformed VarInt, or a length that contradicts the data.
    Malformed,
    /// A compressed packet failed to inflate.
    DecompressionFailed,
    /// A packet claimed to be compressed but is under the threshold, or claimed
    /// a size that does not match what it inflated to.
    BadCompression,
};

[[nodiscard]] std::string_view to_string(FrameError error) noexcept;

template<typename T>
using FrameResult = std::expected<T, FrameError>;

/// Vanilla's ceiling for a single packet: 2 MiB. A chunk packet is the largest
/// thing that legitimately approaches it.
inline constexpr u32 kMaxPacketLength = 2 * 1024 * 1024;

/// Compression off. The server chooses a threshold at login; -1 means never.
inline constexpr i32 kNoCompression = -1;

/// One decoded packet: its id and its body, with the length prefix consumed.
struct Packet {
    i32             id{0};
    std::vector<u8> body;
};

/// Reassembles packets from a socket's byte stream.
///
/// A socket read returns whatever arrived, which may be half a packet or three
/// and a half. This buffers the remainder and yields whole packets only.
class FrameDecoder {
public:
    /// Append bytes as they arrive from the socket.
    void feed(std::span<const u8> bytes);

    /// Take the next whole packet, or Incomplete when more bytes are needed.
    ///
    /// Incomplete is the normal case on a socket and must not be treated as a
    /// failure; every other error means the connection should be dropped.
    [[nodiscard]] FrameResult<Packet> next();

    /// Enable compression at the given threshold, or kNoCompression to disable.
    /// Takes effect for the next packet read, matching Set Compression.
    void set_compression_threshold(i32 threshold) noexcept { threshold_ = threshold; }

    [[nodiscard]] i32 compression_threshold() const noexcept { return threshold_; }

    /// Bytes buffered but not yet forming a packet.
    [[nodiscard]] usize pending() const noexcept { return buffer_.size() - consumed_; }

    void reset() noexcept;

private:
    std::vector<u8> buffer_;
    usize           consumed_{0};
    i32             threshold_{kNoCompression};
};

/// Encode one packet, applying compression when it is enabled and the body is
/// over the threshold.
[[nodiscard]] FrameResult<std::vector<u8>> encode_packet(i32 packet_id, std::span<const u8> body,
                                                         i32 threshold = kNoCompression);

/// Encode into an existing writer, for callers assembling several packets.
[[nodiscard]] FrameResult<void> encode_packet_into(io::ByteWriter& writer, i32 packet_id,
                                                   std::span<const u8> body,
                                                   i32                 threshold = kNoCompression);

}  // namespace ov::net
