#include "ov/protocol/framing.hpp"
#include "ov/protocol/varint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ov;
using namespace ov::net;

namespace {

std::vector<u8> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string text_of(const std::vector<u8>& bytes) {
    return {bytes.begin(), bytes.end()};
}

}  // namespace

TEST_CASE("an uncompressed packet round-trips", "[protocol][framing]") {
    const auto encoded = encode_packet(0x00, bytes_of("hello"));
    REQUIRE(encoded.has_value());

    // length(1) + id(1) + body(5)
    REQUIRE(encoded->size() == 7);
    REQUIRE((*encoded)[0] == 6);
    REQUIRE((*encoded)[1] == 0x00);

    FrameDecoder decoder;
    decoder.feed(*encoded);
    const auto packet = decoder.next();
    REQUIRE(packet.has_value());
    REQUIRE(packet->id == 0x00);
    REQUIRE(text_of(packet->body) == "hello");
}

TEST_CASE("a partial packet asks for more rather than failing", "[protocol][framing]") {
    // A socket read returns whatever arrived. Incomplete is the normal case and
    // must be distinguishable from a real error, or every slow connection looks
    // like an attack.
    const auto   encoded = encode_packet(0x12, bytes_of("some payload here")).value();
    FrameDecoder decoder;

    for (usize i = 0; i + 1 < encoded.size(); ++i) {
        decoder.reset();
        decoder.feed(std::span<const u8>{encoded.data(), i});
        const auto packet = decoder.next();
        REQUIRE_FALSE(packet.has_value());
        REQUIRE(packet.error() == FrameError::Incomplete);
    }

    decoder.reset();
    decoder.feed(encoded);
    REQUIRE(decoder.next().has_value());
}

TEST_CASE("a packet arriving one byte at a time is reassembled", "[protocol][framing]") {
    const auto   encoded = encode_packet(0x07, bytes_of("dribbled in")).value();
    FrameDecoder decoder;

    for (usize i = 0; i + 1 < encoded.size(); ++i) {
        decoder.feed(std::span<const u8>{encoded.data() + i, 1});
        REQUIRE(decoder.next().error() == FrameError::Incomplete);
    }
    decoder.feed(std::span<const u8>{encoded.data() + encoded.size() - 1, 1});

    const auto packet = decoder.next();
    REQUIRE(packet.has_value());
    REQUIRE(text_of(packet->body) == "dribbled in");
}

TEST_CASE("several packets in one read are all decoded", "[protocol][framing]") {
    io::ByteWriter stream;
    REQUIRE(encode_packet_into(stream, 0x00, bytes_of("first")).has_value());
    REQUIRE(encode_packet_into(stream, 0x01, bytes_of("second")).has_value());
    REQUIRE(encode_packet_into(stream, 0x02, bytes_of("third")).has_value());

    FrameDecoder decoder;
    decoder.feed(stream.take());

    REQUIRE(text_of(decoder.next().value().body) == "first");
    REQUIRE(text_of(decoder.next().value().body) == "second");
    REQUIRE(text_of(decoder.next().value().body) == "third");
    REQUIRE(decoder.next().error() == FrameError::Incomplete);
}

TEST_CASE("a packet split across two reads is reassembled", "[protocol][framing]") {
    io::ByteWriter stream;
    REQUIRE(encode_packet_into(stream, 0x00, bytes_of("one")).has_value());
    REQUIRE(encode_packet_into(stream, 0x01, bytes_of("two")).has_value());
    const auto bytes = stream.take();

    // Cut in the middle of the second packet.
    FrameDecoder decoder;
    decoder.feed(std::span<const u8>{bytes.data(), 6});
    REQUIRE(text_of(decoder.next().value().body) == "one");
    REQUIRE(decoder.next().error() == FrameError::Incomplete);

    decoder.feed(std::span<const u8>{bytes.data() + 6, bytes.size() - 6});
    REQUIRE(text_of(decoder.next().value().body) == "two");
}

TEST_CASE("a large packet id round-trips", "[protocol][framing]") {
    const auto   encoded = encode_packet(300, bytes_of("x")).value();
    FrameDecoder decoder;
    decoder.feed(encoded);
    REQUIRE(decoder.next().value().id == 300);
}

TEST_CASE("an empty body round-trips", "[protocol][framing]") {
    // Status Request and Login Start's siblings carry no payload at all.
    const auto   encoded = encode_packet(0x00, {}).value();
    FrameDecoder decoder;
    decoder.feed(encoded);

    const auto packet = decoder.next();
    REQUIRE(packet.has_value());
    REQUIRE(packet->id == 0);
    REQUIRE(packet->body.empty());
}

// ── Compression ─────────────────────────────────────────────────────────────

TEST_CASE("a packet over the threshold is compressed and round-trips", "[protocol][framing]") {
    const std::string large(1000, 'a');

    const auto encoded = encode_packet(0x21, bytes_of(large), 256);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() < large.size());  // it actually shrank

    FrameDecoder decoder;
    decoder.set_compression_threshold(256);
    decoder.feed(*encoded);

    const auto packet = decoder.next();
    REQUIRE(packet.has_value());
    REQUIRE(packet->id == 0x21);
    REQUIRE(text_of(packet->body) == large);
}

TEST_CASE("a packet under the threshold uses the compressed framing uncompressed",
          "[protocol][framing]") {
    // The case that is easy to get wrong and is also the common one: once
    // compression is on, every packet uses the compressed framing, with a zero
    // inner length when the body was left alone.
    const auto encoded = encode_packet(0x03, bytes_of("small"), 256);
    REQUIRE(encoded.has_value());

    // length, then a zero inner length, then the payload.
    io::ByteReader reader{std::span<const u8>{*encoded}};
    REQUIRE(read_varint(reader).has_value());
    REQUIRE(read_varint(reader).value() == 0);

    FrameDecoder decoder;
    decoder.set_compression_threshold(256);
    decoder.feed(*encoded);

    const auto packet = decoder.next();
    REQUIRE(packet.has_value());
    REQUIRE(packet->id == 0x03);
    REQUIRE(text_of(packet->body) == "small");
}

TEST_CASE("compression can be switched on mid-stream", "[protocol][framing]") {
    // Set Compression arrives during login, and every packet after it uses the
    // other format. A decoder that cannot switch loses the connection there.
    io::ByteWriter stream;
    REQUIRE(encode_packet_into(stream, 0x00, bytes_of("before"), kNoCompression).has_value());
    const auto first = stream.take();

    const auto second = encode_packet(0x01, bytes_of(std::string(500, 'b')), 128).value();

    FrameDecoder decoder;
    decoder.feed(first);
    REQUIRE(text_of(decoder.next().value().body) == "before");

    decoder.set_compression_threshold(128);
    decoder.feed(second);
    REQUIRE(decoder.next().value().body.size() == 500);
}

// ── Hostile input ───────────────────────────────────────────────────────────
// The length prefix is the first thing an unauthenticated peer controls.

TEST_CASE("an oversized length is rejected before allocating", "[protocol][framing][malformed]") {
    // A handful of bytes claiming a 100 MB packet.
    io::ByteWriter writer;
    write_varint(writer, 100 * 1024 * 1024);
    writer.write_bytes(bytes_of("x"));

    FrameDecoder decoder;
    decoder.feed(writer.take());
    REQUIRE(decoder.next().error() == FrameError::TooLarge);
}

TEST_CASE("a negative length is rejected", "[protocol][framing][malformed]") {
    io::ByteWriter writer;
    write_varint(writer, -1);
    writer.write_bytes(bytes_of("xxxx"));

    FrameDecoder decoder;
    decoder.feed(writer.take());
    REQUIRE(decoder.next().error() == FrameError::Malformed);
}

TEST_CASE("a malformed length VarInt is rejected", "[protocol][framing][malformed]") {
    FrameDecoder decoder;
    decoder.feed(std::vector<u8>(6, 0xFF));
    REQUIRE(decoder.next().error() == FrameError::Malformed);
}

TEST_CASE("a packet claiming compression it did not use is rejected",
          "[protocol][framing][malformed]") {
    // An inner length under the threshold means the peer compressed something
    // it should not have, which would let it hide a large payload behind a
    // small declared size.
    io::ByteWriter frame;
    write_varint(frame, 10);  // claims to inflate to 10, under a threshold of 256
    frame.write_bytes(bytes_of("garbage"));
    const auto inner = frame.take();

    io::ByteWriter outer;
    write_varint(outer, static_cast<i32>(inner.size()));
    outer.write_bytes(std::span<const u8>{inner});

    FrameDecoder decoder;
    decoder.set_compression_threshold(256);
    decoder.feed(outer.take());
    REQUIRE(decoder.next().error() == FrameError::BadCompression);
}

TEST_CASE("a compressed packet that does not inflate is rejected",
          "[protocol][framing][malformed]") {
    io::ByteWriter frame;
    write_varint(frame, 500);  // over the threshold, so it must be zlib
    frame.write_bytes(bytes_of("this is not a zlib stream at all"));
    const auto inner = frame.take();

    io::ByteWriter outer;
    write_varint(outer, static_cast<i32>(inner.size()));
    outer.write_bytes(std::span<const u8>{inner});

    FrameDecoder decoder;
    decoder.set_compression_threshold(256);
    decoder.feed(outer.take());
    REQUIRE(decoder.next().error() == FrameError::DecompressionFailed);
}

TEST_CASE("a decoder survives arbitrary bytes", "[protocol][framing][malformed]") {
    // The contract is only that it returns. Run under ASan, this covers the
    // whole class of out-of-bounds reads on hostile framing.
    FrameDecoder    decoder;
    u32             state = 12345;
    std::vector<u8> noise(4096);
    for (auto& byte : noise) {
        state = state * 1664525u + 1013904223u;
        byte  = static_cast<u8>(state >> 24);
    }

    decoder.feed(noise);
    for (int i = 0; i < 50; ++i) {
        const auto packet = decoder.next();
        if (!packet.has_value() && packet.error() != FrameError::Incomplete) {
            break;
        }
    }
    SUCCEED();
}

TEST_CASE("errors have readable names", "[protocol][framing]") {
    REQUIRE(to_string(FrameError::Incomplete) == "packet is incomplete");
    REQUIRE(to_string(FrameError::TooLarge) == "packet exceeds the maximum length");
}
