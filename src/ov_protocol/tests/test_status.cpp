#include "ov/io/byte_writer.hpp"
#include "ov/protocol/status.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::net;

TEST_CASE("the protocol version is 763", "[protocol][status]") {
    // 763 is Minecraft 1.20 and 1.20.1. A client comparing this against its own
    // shows "outdated server" or "outdated client" rather than connecting.
    STATIC_REQUIRE(kProtocolVersion == 763);
    REQUIRE(std::string_view{kVersionName} == "1.20.1");
}

TEST_CASE("the status JSON carries every field a client reads", "[protocol][status]") {
    // A client that cannot parse this shows the server as unreachable rather
    // than reporting an error, so a missing field looks like a network problem.
    ServerStatus status;
    status.description    = "Ondes VOXEL";
    status.max_players    = 20;
    status.online_players = 3;

    const std::string json = status.to_json();

    REQUIRE(json.find(R"("name":"1.20.1")") != std::string::npos);
    REQUIRE(json.find(R"("protocol":763)") != std::string::npos);
    REQUIRE(json.find(R"("max":20)") != std::string::npos);
    REQUIRE(json.find(R"("online":3)") != std::string::npos);
    REQUIRE(json.find(R"("text":"Ondes VOXEL")") != std::string::npos);
    // Left out at its default, false, as the 1.20.1 jar leaves it out; and
    // no empty sample either.
    REQUIRE(json.find("enforcesSecureChat") == std::string::npos);
    REQUIRE(json.find("sample") == std::string::npos);
    REQUIRE(json.front() == '{');
    REQUIRE(json.back() == '}');
}

TEST_CASE("a description with quotes does not break the JSON", "[protocol][status]") {
    // The description comes from a config file. An unescaped quote produces a
    // document the client cannot parse, and the server simply looks offline.
    ServerStatus status;
    status.description = R"(He said "hi" \ then left)";

    const std::string json = status.to_json();
    REQUIRE(json.find(R"(\"hi\")") != std::string::npos);
    REQUIRE(json.find(R"(\\)") != std::string::npos);
}

TEST_CASE("control characters are escaped", "[protocol][status]") {
    ServerStatus status;
    status.description = std::string{"line\nbreak\ttab"} + '\x01';

    const std::string json = status.to_json();
    REQUIRE(json.find("\\n") != std::string::npos);
    REQUIRE(json.find("\\t") != std::string::npos);
    REQUIRE(json.find("\\u0001") != std::string::npos);
    // No raw control byte survived into the document.
    REQUIRE(json.find('\n') == std::string::npos);
}

TEST_CASE("the player sample carries names and ids", "[protocol][status]") {
    ServerStatus status;
    status.sample = {"alice", "bob"};

    const std::string json = status.to_json();
    REQUIRE(json.find(R"("name":"alice")") != std::string::npos);
    REQUIRE(json.find(R"("name":"bob")") != std::string::npos);
    // A sample entry without an id is rejected by the client.
    REQUIRE(json.find(R"("id":)") != std::string::npos);
}

TEST_CASE("the favicon is omitted when absent", "[protocol][status]") {
    ServerStatus without;
    REQUIRE(without.to_json().find("favicon") == std::string::npos);

    ServerStatus with;
    with.favicon = "data:image/png;base64,iVBORw0KGgo=";
    REQUIRE(with.to_json().find("favicon") != std::string::npos);
}

TEST_CASE("a handshake round-trips", "[protocol][status]") {
    io::ByteWriter writer;
    write_varint(writer, 763);
    write_string(writer, "mc.example.com");
    writer.write_u16(25565);
    write_varint(writer, 2);  // login
    const auto body = writer.take();

    const auto handshake = parse_handshake(body);
    REQUIRE(handshake.has_value());
    REQUIRE(handshake->protocol_version == 763);
    REQUIRE(handshake->server_address == "mc.example.com");
    REQUIRE(handshake->server_port == 25565);
    REQUIRE(handshake->next_state == NextState::Login);
}

TEST_CASE("a status handshake is recognised", "[protocol][status]") {
    io::ByteWriter writer;
    write_varint(writer, 763);
    write_string(writer, "localhost");
    writer.write_u16(25565);
    write_varint(writer, 1);  // status

    const auto handshake = parse_handshake(writer.take());
    REQUIRE(handshake.has_value());
    REQUIRE(handshake->next_state == NextState::Status);
}

TEST_CASE("a malformed handshake is rejected", "[protocol][status][malformed]") {
    // The very first packet of any connection, from an unauthenticated peer.
    REQUIRE_FALSE(parse_handshake({}).has_value());
    REQUIRE_FALSE(parse_handshake(std::vector<u8>{0xFF}).has_value());

    // A next-state that is neither 1 nor 2.
    io::ByteWriter writer;
    write_varint(writer, 763);
    write_string(writer, "x");
    writer.write_u16(25565);
    write_varint(writer, 99);
    REQUIRE_FALSE(parse_handshake(writer.take()).has_value());

    // Truncated before the port.
    io::ByteWriter short_one;
    write_varint(short_one, 763);
    write_string(short_one, "x");
    REQUIRE_FALSE(parse_handshake(short_one.take()).has_value());
}

TEST_CASE("a pong echoes the payload unchanged", "[protocol][status]") {
    // The client measures latency from this, and checks the value came back.
    const i64  payload = 0x0123456789ABCDEFLL;
    const auto body    = encode_pong(payload);

    REQUIRE(body.size() == 8);
    io::ByteReader reader{std::span<const u8>{body}};
    REQUIRE(reader.read_i64().value() == payload);
}

TEST_CASE("the status response is one JSON string", "[protocol][status]") {
    const ServerStatus status;
    const auto         body = encode_status_response(status);

    io::ByteReader reader{std::span<const u8>{body}};
    const auto     json = read_string(reader, 32767);
    REQUIRE(json.has_value());
    REQUIRE(json->find("763") != std::string::npos);
    REQUIRE(reader.exhausted());
}
