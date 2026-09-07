#include "ov/io/byte_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace ov;
using namespace ov::io;

namespace {
std::span<const u8> as_span(const std::vector<u8>& v) { return {v.data(), v.size()}; }
}  // namespace

TEST_CASE("integers are read big-endian", "[io][reader]") {
    // NBT, the region header and the wire protocol are all big-endian. Reading
    // these in host order would produce plausible-looking nonsense.
    const std::vector<u8> data{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ByteReader reader{as_span(data)};

    REQUIRE(reader.read_u16().value() == 0x0102);
    REQUIRE(reader.read_u16().value() == 0x0304);
    REQUIRE(reader.read_u32().value() == 0x05060708);
    REQUIRE(reader.exhausted());
}

TEST_CASE("signed integers keep their sign", "[io][reader]") {
    const std::vector<u8> data{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x00};
    ByteReader reader{as_span(data)};

    REQUIRE(reader.read_i8().value() == -1);
    REQUIRE(reader.read_i16().value() == -1);
    REQUIRE(reader.read_i16().value() == -1);
    REQUIRE(reader.read_i16().value() == -32768);
}

TEST_CASE("64-bit values round-trip", "[io][reader]") {
    // Chunk palettes pack their block indices into i64 arrays, so the full
    // width matters, sign bit included.
    const std::vector<u8> data{0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ByteReader reader{as_span(data)};
    REQUIRE(reader.read_i64().value() == static_cast<i64>(0x8000000000000001ULL));
}

TEST_CASE("floats are IEEE-754 big-endian", "[io][reader]") {
    // 0x3F800000 is 1.0f; 0x400921FB54442D18 is pi as a double.
    const std::vector<u8> data{0x3F, 0x80, 0x00, 0x00,
                               0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2D, 0x18};
    ByteReader reader{as_span(data)};

    REQUIRE(reader.read_f32().value() == 1.0f);
    REQUIRE(reader.read_f64().value() == 3.141592653589793);
}

TEST_CASE("reading past the end fails instead of reading garbage", "[io][reader]") {
    // Region files and packets come from untrusted sources. A reader that
    // trusts its input is a remote crash, so this is a correctness test, not a
    // politeness one.
    const std::vector<u8> data{0x01, 0x02, 0x03};
    ByteReader reader{as_span(data)};

    REQUIRE(reader.read_u16().has_value());
    REQUIRE(reader.remaining() == 1);

    const auto overrun = reader.read_u32();
    REQUIRE_FALSE(overrun.has_value());
    REQUIRE(overrun.error() == ReadError::OutOfBounds);

    // A failed read must not consume anything.
    REQUIRE(reader.remaining() == 1);
    REQUIRE(reader.read_u8().value() == 0x03);
}

TEST_CASE("an empty buffer fails every read", "[io][reader]") {
    const std::vector<u8> empty;
    ByteReader reader{as_span(empty)};

    REQUIRE(reader.exhausted());
    REQUIRE(reader.remaining() == 0);
    REQUIRE(reader.read_u8().error() == ReadError::OutOfBounds);
    REQUIRE(reader.read_f64().error() == ReadError::OutOfBounds);
    REQUIRE(reader.skip(1).error() == ReadError::OutOfBounds);
    REQUIRE(reader.skip(0).has_value());
}

TEST_CASE("read_bytes borrows without copying", "[io][reader]") {
    const std::vector<u8> data{0x10, 0x11, 0x12, 0x13, 0x14};
    ByteReader reader{as_span(data)};

    const auto view = reader.read_bytes(3);
    REQUIRE(view.has_value());
    REQUIRE(view->size() == 3);
    // Same memory, not a copy: this is what keeps chunk parsing cheap.
    REQUIRE(view->data() == data.data());
    REQUIRE(reader.remaining() == 2);

    REQUIRE(reader.read_bytes(3).error() == ReadError::OutOfBounds);
    REQUIRE(reader.remaining() == 2);
}

TEST_CASE("a huge length is rejected rather than allocated", "[io][reader]") {
    // The shape of a real attack: a small buffer declaring a four-gigabyte
    // payload. It must fail on the bound, never on an allocation.
    const std::vector<u8> data{0x01, 0x02};
    ByteReader reader{as_span(data)};
    REQUIRE(reader.read_bytes(0xFFFFFFFFULL).error() == ReadError::OutOfBounds);
    REQUIRE(reader.remaining() == 2);
}

TEST_CASE("seek clamps to the end", "[io][reader]") {
    const std::vector<u8> data{0x01, 0x02, 0x03};
    ByteReader reader{as_span(data)};

    reader.seek(2);
    REQUIRE(reader.read_u8().value() == 0x03);

    reader.seek(9999);
    REQUIRE(reader.exhausted());
    REQUIRE(reader.position() == data.size());

    reader.seek(0);
    REQUIRE(reader.read_u8().value() == 0x01);
}

TEST_CASE("peek_remaining does not advance", "[io][reader]") {
    const std::vector<u8> data{0x01, 0x02, 0x03, 0x04};
    ByteReader reader{as_span(data)};
    REQUIRE(reader.read_u16().has_value());

    const auto rest = reader.peek_remaining();
    REQUIRE(rest.size() == 2);
    REQUIRE(rest[0] == 0x03);
    REQUIRE(reader.remaining() == 2);
}

TEST_CASE("errors have readable names", "[io][reader]") {
    REQUIRE(to_string(ReadError::OutOfBounds) == "out of bounds");
    REQUIRE(to_string(ReadError::InvalidLength) == "invalid length");
    REQUIRE(to_string(ReadError::MalformedEncoding) == "malformed encoding");
}
