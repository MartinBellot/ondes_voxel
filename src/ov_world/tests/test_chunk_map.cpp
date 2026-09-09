#include "ov/registry/block_states.hpp"
#include "ov/world/chunk_map.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace ov;
using namespace ov::world;

namespace {

/// A chunk with nothing in it, which is all these tests need: they are about
/// who keeps a chunk alive, not about what is inside one.
[[nodiscard]] Chunk empty_chunk(ChunkPos pos) {
    return Chunk{pos, WorldShape::overworld(), AirStates{}, nullptr};
}

}  // namespace

TEST_CASE("with no tickets nothing is wanted", "[chunk_map]") {
    ChunkMap     map;
    LevelChanges changes;
    map.refresh(changes);

    REQUIRE(changes.wanted.empty());
    REQUIRE(changes.unwanted.empty());
    REQUIRE(map.level_of(ChunkPos{0, 0}) == LoadLevel::kUnloaded);
    REQUIRE_FALSE(map.is_wanted(ChunkPos{0, 0}));
}

TEST_CASE("a view-distance ticket wants exactly its square", "[chunk_map]") {
    constexpr i32 kViewDistance = 8;

    ChunkMap map;
    map.set_ticket(TicketType::Player, 1, ChunkPos{0, 0},
                   LoadLevel::for_view_distance(kViewDistance));

    LevelChanges changes;
    map.refresh(changes);

    // 17 x 17 — the number the server has been streaming since before any of
    // this existed, and the one the mandate counts.
    REQUIRE(changes.wanted.size() == 289);

    REQUIRE(map.level_of(ChunkPos{0, 0}) == LoadLevel::kLoaded - kViewDistance);
    REQUIRE(map.level_of(ChunkPos{8, 0}) == LoadLevel::kLoaded);
    REQUIRE(map.level_of(ChunkPos{8, 8}) == LoadLevel::kLoaded);
    REQUIRE(map.is_wanted(ChunkPos{-8, 8}));

    // One step further out and nothing reaches it. Chebyshev, not Euclidean:
    // a view distance of eight is a square, not a circle.
    REQUIRE(map.level_of(ChunkPos{9, 0}) == LoadLevel::kUnloaded);
    REQUIRE_FALSE(map.is_wanted(ChunkPos{9, 9}));
}

TEST_CASE("ticking is a smaller square than loading", "[chunk_map]") {
    ChunkMap map;
    map.set_ticket(TicketType::Player, 1, ChunkPos{0, 0}, LoadLevel::for_view_distance(8));
    LevelChanges changes;
    map.refresh(changes);

    // The distinction the level exists for: the far ring is sent to the client
    // and not simulated.
    REQUIRE(map.is_ticking(ChunkPos{0, 0}));
    REQUIRE(map.is_ticking(ChunkPos{6, 0}));
    REQUIRE_FALSE(map.is_ticking(ChunkPos{7, 0}));
    REQUIRE(map.is_wanted(ChunkPos{7, 0}));
}

TEST_CASE("moving a ticket moves the square rather than adding one", "[chunk_map]") {
    ChunkMap map;
    map.set_ticket(TicketType::Player, 7, ChunkPos{0, 0}, LoadLevel::for_view_distance(2));
    LevelChanges changes;
    map.refresh(changes);
    REQUIRE(changes.wanted.size() == 25);
    REQUIRE(map.ticket_count() == 1);

    // One step east. Five columns arrive, five leave, and the ticket count does
    // not move: a player walking is a move, which is what stops a wandering
    // player from pinning the whole world.
    map.set_ticket(TicketType::Player, 7, ChunkPos{1, 0}, LoadLevel::for_view_distance(2));
    map.refresh(changes);
    REQUIRE(map.ticket_count() == 1);
    REQUIRE(changes.wanted.size() == 5);
    REQUIRE(changes.unwanted.size() == 5);
    REQUIRE_FALSE(map.is_wanted(ChunkPos{-2, 0}));
    REQUIRE(map.is_wanted(ChunkPos{3, 0}));
}

TEST_CASE("two tickets overlap and a chunk keeps the lowest level", "[chunk_map]") {
    ChunkMap map;
    map.set_ticket(TicketType::Player, 1, ChunkPos{0, 0}, LoadLevel::for_view_distance(4));
    map.set_ticket(TicketType::Player, 2, ChunkPos{2, 0}, LoadLevel::for_view_distance(4));
    LevelChanges changes;
    map.refresh(changes);

    // (1,0) is one chunk from the first ticket and one from the second; the
    // lowest wins, and both give the same here.
    REQUIRE(map.level_of(ChunkPos{1, 0}) == LoadLevel::for_view_distance(4) + 1);
    // (2,0) is the second ticket's own chunk and two chunks from the first.
    REQUIRE(map.level_of(ChunkPos{2, 0}) == LoadLevel::for_view_distance(4));

    // Dropping one leaves the other's square standing.
    map.remove_ticket(TicketType::Player, 2);
    map.refresh(changes);
    REQUIRE(map.is_wanted(ChunkPos{4, 0}));
    REQUIRE_FALSE(map.is_wanted(ChunkPos{5, 0}));
}

TEST_CASE("dropping the last ticket makes everything unwanted", "[chunk_map]") {
    ChunkMap map;
    map.set_ticket(TicketType::Player, 1, ChunkPos{10, -10}, LoadLevel::for_view_distance(3));
    LevelChanges changes;
    map.refresh(changes);
    const usize wanted = changes.wanted.size();
    REQUIRE(wanted == 49);

    map.remove_ticket(TicketType::Player, 1);
    map.refresh(changes);
    REQUIRE(changes.wanted.empty());
    REQUIRE(changes.unwanted.size() == wanted);
    REQUIRE(map.level_of(ChunkPos{10, -10}) == LoadLevel::kUnloaded);
}

TEST_CASE("a ticket type is its own reason", "[chunk_map]") {
    // The same owner id in two types is two tickets. Without that, a forceload
    // and a player who happen to share a number would cancel each other.
    ChunkMap map;
    map.set_ticket(TicketType::Player, 5, ChunkPos{0, 0}, LoadLevel::for_view_distance(1));
    map.set_ticket(TicketType::Forced, 5, ChunkPos{50, 50}, LoadLevel::for_view_distance(1));
    REQUIRE(map.ticket_count() == 2);

    LevelChanges changes;
    map.refresh(changes);
    REQUIRE(map.is_wanted(ChunkPos{0, 0}));
    REQUIRE(map.is_wanted(ChunkPos{50, 50}));

    map.remove_ticket(TicketType::Player, 5);
    map.refresh(changes);
    REQUIRE_FALSE(map.is_wanted(ChunkPos{0, 0}));
    REQUIRE(map.is_wanted(ChunkPos{50, 50}));
}

TEST_CASE("wanted chunks come out nearest first, and in a stable order",
          "[chunk_map]") {
    ChunkMap map;
    map.set_ticket(TicketType::Player, 1, ChunkPos{0, 0}, LoadLevel::for_view_distance(3));
    LevelChanges changes;
    map.refresh(changes);

    std::vector<ChunkPos> order;
    map.wanted_chunks(order);
    REQUIRE(order.size() == 49);
    REQUIRE(order.front() == ChunkPos{0, 0});

    // Levels never go down as the list is walked: that is what "nearest first"
    // means here, expressed through the level rather than through a distance
    // the map would have to be told about.
    for (usize i = 1; i < order.size(); ++i) {
        REQUIRE(map.level_of(order[i - 1]) <= map.level_of(order[i]));
    }

    // And the order is total, not the hash table's. Two runs that differed
    // would make the server send chunks in a different sequence each time.
    std::vector<ChunkPos> again;
    map.wanted_chunks(again);
    REQUIRE(again == order);
}

TEST_CASE("storage hands a chunk over and gives it back", "[chunk_map]") {
    ChunkMap map;
    REQUIRE(map.find(ChunkPos{3, 4}) == nullptr);
    REQUIRE_FALSE(map.contains(ChunkPos{3, 4}));

    map.publish(ChunkPos{3, 4}, empty_chunk(ChunkPos{3, 4}));
    REQUIRE(map.resident() == 1);
    REQUIRE(map.contains(ChunkPos{3, 4}));
    REQUIRE(map.find(ChunkPos{3, 4})->position() == ChunkPos{3, 4});

    auto taken = map.evict(ChunkPos{3, 4});
    REQUIRE(taken.has_value());
    REQUIRE(taken->position() == ChunkPos{3, 4});
    REQUIRE(map.resident() == 0);
    REQUIRE_FALSE(map.evict(ChunkPos{3, 4}).has_value());
}

TEST_CASE("a chunk is only ever asked for once", "[chunk_map]") {
    // The bug this prevents: without an in-flight set, a tick loop that scans
    // its pending list every tick asks the generator for the same chunk twenty
    // times a second until the first answer comes back.
    ChunkMap map;
    REQUIRE(map.mark_in_flight(ChunkPos{1, 1}));
    REQUIRE_FALSE(map.mark_in_flight(ChunkPos{1, 1}));
    REQUIRE(map.is_in_flight(ChunkPos{1, 1}));
    REQUIRE(map.in_flight_count() == 1);

    // Publishing is what ends the flight; a caller that had to remember to
    // clear it would eventually forget.
    map.publish(ChunkPos{1, 1}, empty_chunk(ChunkPos{1, 1}));
    REQUIRE_FALSE(map.is_in_flight(ChunkPos{1, 1}));
    REQUIRE(map.in_flight_count() == 0);
}

TEST_CASE("for_each visits every resident chunk once", "[chunk_map]") {
    ChunkMap map;
    for (i32 x = 0; x < 4; ++x) {
        for (i32 z = 0; z < 4; ++z) {
            map.publish(ChunkPos{x, z}, empty_chunk(ChunkPos{x, z}));
        }
    }

    std::vector<ChunkPos> seen;
    map.for_each([&](ChunkPos pos, const Chunk& chunk) {
        REQUIRE(chunk.position() == pos);
        seen.push_back(pos);
    });
    REQUIRE(seen.size() == 16);
    std::ranges::sort(seen, [](ChunkPos a, ChunkPos b) { return a.packed() < b.packed(); });
    REQUIRE(std::ranges::adjacent_find(seen) == seen.end());
}
