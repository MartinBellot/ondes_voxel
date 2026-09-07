#include "ov/protocol/framing.hpp"

#include "ov/io/compression.hpp"
#include "ov/protocol/varint.hpp"

#include <algorithm>

namespace ov::net {
namespace {

/// Read a VarInt from a raw span without committing a reader position.
///
/// The length prefix has to be peeked: if the whole packet has not arrived yet,
/// nothing may be consumed, or the next read starts mid-value.
struct PeekedVarInt {
    i32  value{0};
    u32  size{0};
    bool complete{false};
    bool malformed{false};
};

[[nodiscard]] PeekedVarInt peek_varint(std::span<const u8> data) noexcept {
    u32 result = 0;
    u32 shift  = 0;

    for (u32 i = 0; i < kMaxVarIntBytes; ++i) {
        if (i >= data.size()) {
            return PeekedVarInt{0, 0, false, false};  // more bytes needed
        }
        const u8 byte = data[i];
        result |= static_cast<u32>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return PeekedVarInt{static_cast<i32>(result), i + 1, true, false};
        }
        shift += 7;
    }
    return PeekedVarInt{0, 0, false, true};
}

}  // namespace

std::string_view to_string(FrameError error) noexcept {
    switch (error) {
        case FrameError::Incomplete: return "packet is incomplete";
        case FrameError::TooLarge: return "packet exceeds the maximum length";
        case FrameError::Malformed: return "malformed packet framing";
        case FrameError::DecompressionFailed: return "packet decompression failed";
        case FrameError::BadCompression: return "packet compression is inconsistent";
    }
    return "unknown framing error";
}

void FrameDecoder::feed(std::span<const u8> bytes) {
    // Drop what has already been handed out before appending, so a long-lived
    // connection does not grow a buffer of everything it ever received.
    if (consumed_ > 0 && consumed_ == buffer_.size()) {
        buffer_.clear();
        consumed_ = 0;
    } else if (consumed_ > 64 * 1024) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<isize>(consumed_));
        consumed_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void FrameDecoder::reset() noexcept {
    buffer_.clear();
    consumed_ = 0;
}

FrameResult<Packet> FrameDecoder::next() {
    const std::span<const u8> available{buffer_.data() + consumed_, buffer_.size() - consumed_};

    const PeekedVarInt length = peek_varint(available);
    if (length.malformed) {
        return std::unexpected{FrameError::Malformed};
    }
    if (!length.complete) {
        return std::unexpected{FrameError::Incomplete};
    }
    if (length.value < 0) {
        return std::unexpected{FrameError::Malformed};
    }
    // Bounded before it becomes an allocation: this number comes from the peer.
    if (static_cast<u32>(length.value) > kMaxPacketLength) {
        return std::unexpected{FrameError::TooLarge};
    }

    const usize total = length.size + static_cast<usize>(length.value);
    if (available.size() < total) {
        return std::unexpected{FrameError::Incomplete};
    }

    // The frame is entirely here; from now on it is safe to consume.
    std::span<const u8> frame = available.subspan(length.size, static_cast<usize>(length.value));
    consumed_ += total;

    std::vector<u8> payload;

    if (threshold_ >= 0) {
        // Compressed format: an inner length says how big the body inflates to,
        // or zero when it was left uncompressed because it is under the
        // threshold. That zero case is the one that is easy to forget, and it
        // is also the common one — most packets are small.
        const PeekedVarInt uncompressed = peek_varint(frame);
        if (uncompressed.malformed || !uncompressed.complete) {
            return std::unexpected{FrameError::Malformed};
        }
        if (uncompressed.value < 0) {
            return std::unexpected{FrameError::Malformed};
        }

        const std::span<const u8> rest = frame.subspan(uncompressed.size);

        if (uncompressed.value == 0) {
            payload.assign(rest.begin(), rest.end());
        } else {
            if (static_cast<u32>(uncompressed.value) > kMaxPacketLength) {
                return std::unexpected{FrameError::TooLarge};
            }
            // A peer must not compress something under the threshold: doing so
            // would let it hide a large payload behind a small one, and vanilla
            // rejects it too.
            if (uncompressed.value < threshold_) {
                return std::unexpected{FrameError::BadCompression};
            }

            auto inflated = io::zlib_decompress(rest, kMaxPacketLength);
            if (!inflated) {
                return std::unexpected{FrameError::DecompressionFailed};
            }
            // The declared size and the actual size must agree, or the frame
            // contradicts itself.
            if (inflated->size() != static_cast<usize>(uncompressed.value)) {
                return std::unexpected{FrameError::BadCompression};
            }
            payload = std::move(*inflated);
        }
    } else {
        payload.assign(frame.begin(), frame.end());
    }

    // The packet id is the first VarInt of the payload; the rest is the body.
    io::ByteReader reader{std::span<const u8>{payload}};
    const auto     packet_id = read_varint(reader);
    if (!packet_id) {
        return std::unexpected{FrameError::Malformed};
    }

    const auto body = reader.peek_remaining();
    return Packet{*packet_id, std::vector<u8>{body.begin(), body.end()}};
}

FrameResult<void> encode_packet_into(io::ByteWriter& writer, i32 packet_id,
                                     std::span<const u8> body, i32 threshold) {
    io::ByteWriter payload;
    write_varint(payload, packet_id);
    payload.write_bytes(body);
    const auto uncompressed = payload.take();

    if (uncompressed.size() > kMaxPacketLength) {
        return std::unexpected{FrameError::TooLarge};
    }

    if (threshold < 0) {
        write_varint(writer, static_cast<i32>(uncompressed.size()));
        writer.write_bytes(std::span<const u8>{uncompressed});
        return {};
    }

    if (uncompressed.size() < static_cast<usize>(threshold)) {
        // Under the threshold: still the compressed framing, with a zero inner
        // length and an uncompressed body.
        io::ByteWriter frame;
        write_varint(frame, 0);
        frame.write_bytes(std::span<const u8>{uncompressed});
        const auto framed = frame.take();

        write_varint(writer, static_cast<i32>(framed.size()));
        writer.write_bytes(std::span<const u8>{framed});
        return {};
    }

    auto compressed = io::zlib_compress(uncompressed);
    if (!compressed) {
        return std::unexpected{FrameError::DecompressionFailed};
    }

    io::ByteWriter frame;
    write_varint(frame, static_cast<i32>(uncompressed.size()));
    frame.write_bytes(std::span<const u8>{*compressed});
    const auto framed = frame.take();

    write_varint(writer, static_cast<i32>(framed.size()));
    writer.write_bytes(std::span<const u8>{framed});
    return {};
}

FrameResult<std::vector<u8>> encode_packet(i32 packet_id, std::span<const u8> body, i32 threshold) {
    io::ByteWriter writer;
    const auto     result = encode_packet_into(writer, packet_id, body, threshold);
    if (!result) {
        return std::unexpected{result.error()};
    }
    return writer.take();
}

}  // namespace ov::net
