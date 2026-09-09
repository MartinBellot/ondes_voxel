#include "ov/protocol/client_play.hpp"

#include "ov/protocol/play.hpp"
#include "ov/protocol/survival.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::net;

// Round trips, on purpose. Every parser here has an encoder on the other side
// of the same wire, written from the same page of the protocol at a different
// time; a parser tested against bytes it was written next to proves nothing,
// and a parser tested against the encoder catches exactly the mistake that
// matters — the two disagreeing about field order.

TEST_CASE("set health round-trips", "[client_play]") {
    const auto bytes  = encode_set_health(7.5F, 13, 2.25F);
    const auto parsed = parse_set_health(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->health == 7.5F);
    CHECK(parsed->food == 13);
    CHECK(parsed->saturation == 2.25F);
}

TEST_CASE("set experience round-trips", "[client_play]") {
    const auto bytes  = encode_set_experience(0.375F, 30, 1395);
    const auto parsed = parse_set_experience(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->bar == 0.375F);
    CHECK(parsed->level == 30);
    CHECK(parsed->total == 1395);
}

TEST_CASE("container content round-trips, empty slots included", "[client_play]") {
    std::vector<ItemStack> slots(46);
    slots[36] = ItemStack{1, 64, {}};
    slots[9]  = ItemStack{792, 3, {}};
    const ItemStack carried{4, 12, {}};

    const auto bytes  = encode_container_content(0, 7, slots, carried);
    const auto parsed = parse_container_content(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->window_id == 0);
    CHECK(parsed->state_id == 7);
    REQUIRE(parsed->slots.size() == 46);
    CHECK(parsed->slots[36].item_id == 1);
    CHECK(parsed->slots[36].count == 64);
    CHECK(parsed->slots[9].item_id == 792);
    // An absent slot is a count of zero, not a separate representation.
    CHECK(parsed->slots[0].empty());
    CHECK(parsed->carried.item_id == 4);
    CHECK(parsed->carried.count == 12);
}

TEST_CASE("a container slot may name the cursor rather than a window", "[client_play]") {
    // −1 is the cursor. Read as an unsigned byte it becomes 255, and the update
    // that puts a stack on the cursor is silently dropped.
    const auto bytes  = encode_container_slot(-1, 3, -1, ItemStack{5, 1, {}});
    const auto parsed = parse_container_slot(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->window_id == -1);
    CHECK(parsed->slot == -1);
    CHECK(parsed->stack.item_id == 5);
}

TEST_CASE("open screen round-trips with its title as json", "[client_play]") {
    const auto bytes  = encode_open_screen(3, 2, "Chest");
    const auto parsed = parse_open_screen(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->window_id == 3);
    CHECK(parsed->type == 2);
    CHECK(parsed->title_json == R"({"text":"Chest"})");
}

TEST_CASE("a click the client sends is a click the server parses", "[client_play]") {
    // The one that matters most: our own client's clicks have to be readable
    // by the same parser the server already runs against a vanilla client.
    const std::array<ClickChange, 2> changed{ClickChange{13, ItemStack{1, 2, {}}},
                                             ClickChange{27, ItemStack{}}};
    const ItemStack                  carried{9, 5, {}};

    const auto bytes = encode_container_click(4, 11, 13, 1, 0, changed, carried);
    const auto click = parse_container_click(bytes);
    REQUIRE(click.has_value());
    CHECK(click->window_id == 4);
    CHECK(click->state_id == 11);
    CHECK(click->slot == 13);
    CHECK(click->button == 1);
    CHECK(click->mode == 0);
    CHECK(click->carried.item_id == 9);
    CHECK(click->carried.count == 5);
}

TEST_CASE("dropping the cursor uses slot -999", "[client_play]") {
    const auto bytes = encode_container_click(0, 1, -999, 0, 4, {}, ItemStack{});
    const auto click = parse_container_click(bytes);
    REQUIRE(click.has_value());
    CHECK(click->slot == -999);
    CHECK(click->mode == 4);
}

TEST_CASE("both directions of close container round-trip", "[client_play]") {
    const auto from_server = parse_clientbound_close_container(encode_close_container(2));
    REQUIRE(from_server.has_value());
    CHECK(*from_server == 2);

    const auto from_client = parse_close_container(encode_serverbound_close_container(2));
    REQUIRE(from_client.has_value());
    CHECK(*from_client == 2);
}

TEST_CASE("a truncated packet is refused rather than half-read", "[client_play]") {
    CHECK_FALSE(parse_set_health(std::span<const u8>{}).has_value());
    CHECK_FALSE(parse_set_experience(std::array<u8, 2>{0, 0}).has_value());
    CHECK_FALSE(parse_container_content(std::array<u8, 1>{0}).has_value());
    CHECK_FALSE(parse_open_screen(std::array<u8, 1>{0}).has_value());
}
