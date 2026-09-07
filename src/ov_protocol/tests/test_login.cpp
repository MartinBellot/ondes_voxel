#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/login.hpp"
#include "ov/protocol/varint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::net;

TEST_CASE("a login start without a uuid parses", "[protocol][login]") {
    io::ByteWriter writer;
    write_string(writer, "Martin");
    writer.write_u8(0);  // no uuid

    const auto login = parse_login_start(writer.take());
    REQUIRE(login.has_value());
    REQUIRE(login->name == "Martin");
    REQUIRE_FALSE(login->claimed_uuid.has_value());
}

TEST_CASE("a login start with a uuid parses and keeps the stream in sync", "[protocol][login]") {
    // Since 1.19.3 the client may send a uuid it believes is its own. Skipping
    // the field rather than reading it leaves the reader mid-packet, and every
    // later field is read from the wrong offset.
    const Uuid claimed{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

    io::ByteWriter writer;
    write_string(writer, "Martin");
    writer.write_u8(1);
    write_uuid(writer, claimed);

    const auto login = parse_login_start(writer.take());
    REQUIRE(login.has_value());
    REQUIRE(login->name == "Martin");
    REQUIRE(login->claimed_uuid.has_value());
    REQUIRE(*login->claimed_uuid == claimed);
}

TEST_CASE("a claimed uuid is advisory, never the identity", "[protocol][login]") {
    // The point of reading it is stream synchronisation, not trust. Honouring
    // it would let anyone claim anyone else's saved data by asking for their
    // uuid, which offline mode has no way to refuse.
    const Uuid claimed{1, 2};

    io::ByteWriter writer;
    write_string(writer, "Martin");
    writer.write_u8(1);
    write_uuid(writer, claimed);

    const auto login = parse_login_start(writer.take());
    REQUIRE(login.has_value());
    REQUIRE(Uuid::offline_player(login->name) != claimed);
}

TEST_CASE("a malformed login start is rejected", "[protocol][login][malformed]") {
    REQUIRE_FALSE(parse_login_start({}).has_value());

    // Claims a uuid follows, then ends.
    io::ByteWriter truncated;
    write_string(truncated, "Martin");
    truncated.write_u8(1);
    REQUIRE_FALSE(parse_login_start(truncated.take()).has_value());

    // A name longer than the protocol allows.
    io::ByteWriter long_name;
    write_string(long_name, std::string(200, 'a'));
    long_name.write_u8(0);
    REQUIRE_FALSE(parse_login_start(long_name.take()).has_value());
}

TEST_CASE("player names follow vanilla's rule", "[protocol][login]") {
    // The name becomes a UUID, a file name and a scoreboard entry, and each of
    // those has its own opinion about what a name may contain. Enforcing the
    // rule once, here, is what keeps those opinions from mattering.
    REQUIRE(is_valid_player_name("Notch"));
    REQUIRE(is_valid_player_name("jeb_"));
    REQUIRE(is_valid_player_name("Ondes_VOXEL"));
    REQUIRE(is_valid_player_name("abc"));
    REQUIRE(is_valid_player_name("0123456789abcdef"));  // exactly 16

    REQUIRE_FALSE(is_valid_player_name(""));
    REQUIRE_FALSE(is_valid_player_name("ab"));                 // under 3
    REQUIRE_FALSE(is_valid_player_name("0123456789abcdefg"));  // over 16
    REQUIRE_FALSE(is_valid_player_name("has space"));
    REQUIRE_FALSE(is_valid_player_name("héllo"));
    REQUIRE_FALSE(is_valid_player_name("semi;colon"));
    REQUIRE_FALSE(is_valid_player_name("../escape"));
}

TEST_CASE("login success carries the identity and no properties", "[protocol][login]") {
    const Uuid uuid = Uuid::offline_player("Martin");
    const auto body = encode_login_success(uuid, "Martin");

    io::ByteReader reader{std::span<const u8>{body}};
    REQUIRE(read_uuid(reader).value() == uuid);
    REQUIRE(read_string(reader, 16).value() == "Martin");
    // Properties carry the skin and cape signed by Mojang; offline has none.
    REQUIRE(read_varint(reader).value() == 0);
    REQUIRE(reader.exhausted());
}

TEST_CASE("set compression carries the threshold", "[protocol][login]") {
    // The vector is named rather than inlined: a span over a temporary dangles
    // as soon as the full expression ends, and the reader would be pointing at
    // freed memory. The first version of this test did exactly that.
    const auto     enabled = encode_set_compression(256);
    io::ByteReader on{std::span<const u8>{enabled}};
    REQUIRE(read_varint(on).value() == 256);

    // A negative threshold disables compression.
    const auto     disabled = encode_set_compression(-1);
    io::ByteReader off{std::span<const u8>{disabled}};
    REQUIRE(read_varint(off).value() == -1);
}

TEST_CASE("a disconnect reason is a JSON chat component", "[protocol][login]") {
    const auto     body = encode_login_disconnect("Server is full");
    io::ByteReader reader{std::span<const u8>{body}};

    const auto component = read_string(reader, 32767);
    REQUIRE(component.has_value());
    REQUIRE(*component == R"({"text":"Server is full"})");
}

TEST_CASE("a disconnect reason with quotes stays parseable", "[protocol][login]") {
    // The reason can carry a player name. An unescaped quote makes the client
    // show a generic error instead of the reason — losing the one thing the
    // message existed to convey.
    const auto     body = encode_login_disconnect(R"(Unknown player "bob\")");
    io::ByteReader reader{std::span<const u8>{body}};

    const auto component = read_string(reader, 32767).value();
    REQUIRE(component.find(R"(\"bob)") != std::string::npos);
    REQUIRE(component.front() == '{');
    REQUIRE(component.back() == '}');
}
