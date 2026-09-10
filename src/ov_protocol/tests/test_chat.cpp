// The chat and command packets, byte for byte.
//
// Two kinds of test. The round trips pin that each encoder and its parser agree
// field for field — a single miscounted field shifts the rest of the packet and
// the client disconnects naming nothing. The hostile inputs pin that every
// parser refuses rather than crashes: all of these come off a socket.
//
// The wire bytes checked against a real server live in the capture
// (`scripts/capture_commands.py`) and in `scripts/check_commands.py`, which
// decodes our Commands packet with a second decoder written separately.
#include "ov/protocol/chat.hpp"

#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ov;
using namespace ov::net;

TEST_CASE("chat command round-trips, signatures included", "[chat]") {
    ChatCommand command;
    command.command   = "time set day";
    command.timestamp = 1'700'000'000'000;
    command.salt      = -42;
    ChatCommand::ArgumentSignature signature;
    signature.name = "message";
    signature.signature.fill(0xAB);
    command.signatures.push_back(signature);
    command.message_count = 3;
    command.acknowledged  = {0x01, 0x02, 0x0F};

    const auto bytes = encode_chat_command(command);
    const auto back  = parse_chat_command(bytes);
    REQUIRE(back);
    CHECK(back->command == "time set day");
    CHECK(back->timestamp == command.timestamp);
    CHECK(back->salt == -42);
    REQUIRE(back->signatures.size() == 1);
    CHECK(back->signatures[0].name == "message");
    CHECK(back->signatures[0].signature == signature.signature);
    CHECK(back->message_count == 3);
    CHECK(back->acknowledged == command.acknowledged);
}

TEST_CASE("the probe's unsigned chat command decodes", "[chat]") {
    // Exactly what scripts/capture_commands.py sends and the vanilla jar
    // accepted: string, two longs, no signatures, count 0, three zero bytes.
    io::ByteWriter writer;
    write_string(writer, "seed");
    writer.write_i64(5);
    writer.write_i64(0);
    write_varint(writer, 0);
    write_varint(writer, 0);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u8(0);
    const auto parsed = parse_chat_command(writer.data());
    REQUIRE(parsed);
    CHECK(parsed->command == "seed");
    CHECK(parsed->signatures.empty());
}

TEST_CASE("chat message round-trips with and without a signature", "[chat]") {
    ChatMessage message;
    message.message   = "hello world";
    message.timestamp = 12;
    message.salt      = 34;
    auto unsigned_bytes = encode_chat_message(message);
    auto back           = parse_chat_message(unsigned_bytes);
    REQUIRE(back);
    CHECK(back->message == "hello world");
    CHECK_FALSE(back->signature);

    ChatSignature signature{};
    signature.fill(7);
    message.signature = signature;
    const auto signed_bytes = encode_chat_message(message);
    CHECK(signed_bytes.size() == unsigned_bytes.size() + 256);
    back = parse_chat_message(signed_bytes);
    REQUIRE(back);
    REQUIRE(back->signature);
    CHECK((*back->signature)[255] == 7);
}

TEST_CASE("chat parsers refuse truncation and trailing bytes", "[chat]") {
    ChatMessage message;
    message.message = "x";
    const auto bytes = encode_chat_message(message);
    for (usize cut = 0; cut < bytes.size(); ++cut) {
        CHECK_FALSE(parse_chat_message(std::span{bytes}.first(cut)));
    }
    auto longer = bytes;
    longer.push_back(0);
    CHECK_FALSE(parse_chat_message(longer));

    ChatCommand command;
    command.command = "list";
    const auto c    = encode_chat_command(command);
    for (usize cut = 0; cut < c.size(); ++cut) {
        CHECK_FALSE(parse_chat_command(std::span{c}.first(cut)));
    }
}

TEST_CASE("a chat message over 256 characters is refused", "[chat]") {
    ChatMessage message;
    message.message = std::string(257, 'a');
    CHECK_FALSE(parse_chat_message(encode_chat_message(message)));
    message.message = std::string(256, 'a');
    CHECK(parse_chat_message(encode_chat_message(message)));
}

TEST_CASE("a chat command claiming nine signatures is refused", "[chat]") {
    io::ByteWriter writer;
    write_string(writer, "msg a b");
    writer.write_i64(0);
    writer.write_i64(0);
    write_varint(writer, 9);
    CHECK_FALSE(parse_chat_command(writer.data()));
}

TEST_CASE("suggestion request and response round-trip", "[chat]") {
    const auto request = parse_suggestions_request(
        encode_suggestions_request(SuggestionsRequest{7, "/time s"}));
    REQUIRE(request);
    CHECK(request->transaction == 7);
    CHECK(request->text == "/time s");

    SuggestionsResponse response;
    response.transaction = 7;
    response.start       = 6;
    response.length      = 1;
    response.matches     = {{"set", std::nullopt}, {"query", std::string{R"({"text":"q"})"}}};
    const auto back      = parse_suggestions_response(encode_suggestions_response(response));
    REQUIRE(back);
    CHECK(back->start == 6);
    CHECK(back->length == 1);
    CHECK(back->matches == response.matches);
}

TEST_CASE("system chat round-trips", "[chat]") {
    const auto bytes = encode_system_chat(R"({"translate":"commands.time.set","with":["1000"]})",
                                          false);
    const auto back  = parse_system_chat(bytes);
    REQUIRE(back);
    CHECK(back->content == R"({"translate":"commands.time.set","with":["1000"]})");
    CHECK_FALSE(back->overlay);
}

TEST_CASE("player chat round-trips unsigned", "[chat]") {
    PlayerChat chat;
    chat.sender      = Uuid{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    chat.index       = 4;
    chat.body        = "hi";
    chat.timestamp   = 99;
    chat.salt        = 1;
    chat.chat_type   = 0;
    chat.name_json   = R"({"text":"ovprobe"})";
    chat.target_json = R"({"text":"other"})";
    const auto back  = parse_player_chat(encode_player_chat(chat));
    REQUIRE(back);
    CHECK(back->sender == chat.sender);
    CHECK(back->index == 4);
    CHECK(back->body == "hi");
    CHECK_FALSE(back->signature);
    CHECK(back->name_json == chat.name_json);
    CHECK(back->target_json == chat.target_json);
    CHECK_FALSE(back->unsigned_content);
}

TEST_CASE("disguised chat round-trips", "[chat]") {
    DisguisedChat chat{R"({"text":"hello"})", 4, R"({"text":"Server"})", std::nullopt};
    const auto    back = parse_disguised_chat(encode_disguised_chat(chat));
    REQUIRE(back);
    CHECK(back->message_json == chat.message_json);
    CHECK(back->chat_type == 4);
    CHECK_FALSE(back->target_json);
}

TEST_CASE("a commands graph round-trips, properties by parser", "[chat][commands]") {
    CommandGraphWire graph;
    // 0 root → 1 "time" → 2 "set" → 3 <time:minecraft:time min 0>
    //                   → 4 <n:brigadier:integer 1..64>, 5 redirect to 1
    //                   → 6 <targets:minecraft:entity flags 3> with ask_server
    graph.nodes.resize(7);
    graph.nodes[0].flags    = command_flags::kRoot;
    graph.nodes[0].children = {1, 5};
    graph.nodes[1]          = {command_flags::kLiteral, {2, 4, 6}, -1, "time", -1, {}, {}};
    graph.nodes[2]          = {command_flags::kLiteral, {3}, -1, "set", -1, {}, {}};
    graph.nodes[3] = {static_cast<u8>(command_flags::kArgument | command_flags::kExecutable),
                      {},
                      -1,
                      "time",
                      40,
                      {0, 0, 0, 0},
                      {}};
    graph.nodes[4] = {static_cast<u8>(command_flags::kArgument | command_flags::kExecutable),
                      {},
                      -1,
                      "n",
                      3,
                      {0x03, 0, 0, 0, 1, 0, 0, 0, 64},
                      {}};
    graph.nodes[5] = {static_cast<u8>(command_flags::kLiteral | command_flags::kHasRedirect),
                      {},
                      1,
                      "alias",
                      -1,
                      {},
                      {}};
    graph.nodes[6] = {static_cast<u8>(command_flags::kArgument | command_flags::kExecutable |
                                      command_flags::kHasSuggestions),
                      {},
                      -1,
                      "targets",
                      6,
                      {0x03},
                      "minecraft:ask_server"};
    graph.root = 0;

    const auto bytes = encode_commands(graph);
    const auto back  = parse_commands(bytes);
    REQUIRE(back);
    CHECK(back->root == 0);
    REQUIRE(back->nodes.size() == graph.nodes.size());
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        CHECK(back->nodes[i] == graph.nodes[i]);
    }
    CHECK(encode_commands(*back) == bytes);
}

TEST_CASE("the commands parser refuses what a client must refuse", "[chat][commands]") {
    const auto graph_with = [](CommandNodeWire node) {
        CommandGraphWire graph;
        graph.nodes.resize(2);
        graph.nodes[0].children = {1};
        graph.nodes[1]          = std::move(node);
        return encode_commands(graph);
    };
    // A parser id outside 1.20.1's registry: the rest cannot be read.
    CHECK_FALSE(parse_commands(graph_with({command_flags::kArgument, {}, -1, "x", 49, {}, {}})));
    // A child index past the end.
    CHECK_FALSE(parse_commands(graph_with({command_flags::kLiteral, {7}, -1, "x", -1, {}, {}})));
    // Node type 3 does not exist.
    CHECK_FALSE(parse_commands(graph_with({0x03, {}, -1, "x", -1, {}, {}})));
    // A string mode outside 0..2.
    CHECK_FALSE(parse_commands(graph_with({command_flags::kArgument, {}, -1, "x", 5, {3}, {}})));
    // And every truncation of a valid one.
    const auto valid = graph_with({command_flags::kArgument, {}, -1, "x", 3, {0x01, 0, 0, 0, 5}, {}});
    REQUIRE(parse_commands(valid));
    for (usize cut = 0; cut < valid.size(); ++cut) {
        CHECK_FALSE(parse_commands(std::span{valid}.first(cut)));
    }
}

TEST_CASE("update section blocks packs as the protocol page says", "[chat]") {
    const std::vector<SectionBlock> blocks{{1, 2, 3, 1}, {15, 15, 15, 24134}};
    const auto bytes = encode_update_section_blocks(-2, -4, 5, blocks);
    const auto back  = parse_update_section_blocks(bytes);
    REQUIRE(back);
    CHECK(back->section_x == -2);
    CHECK(back->section_y == -4);
    CHECK(back->section_z == 5);
    CHECK(back->blocks == blocks);
    // The first entry: state 1 << 12 | x 1 << 8 | z 3 << 4 | y 2 = 0x1132.
    CHECK(bytes[8] == 2);  // count
    CHECK(bytes[9] == 0xB2);
    CHECK(bytes[10] == 0x22);
}

TEST_CASE("server data, titles and difficulty encode their few fields", "[chat]") {
    const auto data = encode_server_data(R"({"text":"m"})", {}, false);
    CHECK(data.back() == 0);
    CHECK(data[data.size() - 2] == 0);  // no icon
    CHECK(encode_change_difficulty(3, false) == std::vector<u8>{3, 0});
    CHECK(encode_title_animation_times(10, 70, 20) ==
          std::vector<u8>{0, 0, 0, 10, 0, 0, 0, 70, 0, 0, 0, 20});
    CHECK(encode_clear_titles(true) == std::vector<u8>{1});
    CHECK(parse_change_difficulty(std::vector<u8>{2}) == u8{2});
    CHECK_FALSE(parse_change_difficulty(std::vector<u8>{4}));
}
