#include "ov/protocol/types.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::net;

TEST_CASE("offline UUIDs match what Java produces", "[protocol][uuid]") {
    // These values were produced by running
    //
    //     UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes(UTF_8))
    //
    // under the JDK, not by this implementation. The project runs in offline
    // mode, so this identity is the only one a player has: their inventory,
    // their position and their advancements are filed under it. A mismatch
    // means the world forgets who they are between restarts, and nothing
    // reports it — the player simply arrives naked at spawn.
    struct Case {
        const char* name;
        const char* expected;
    };

    static constexpr Case kCases[] = {
        {"Notch", "b50ad385-829d-3141-a216-7e7d7539ba7f"},
        {"jeb_", "a762f560-4fce-3236-812a-b80efff0b62b"},
        {"Martin", "9d15c4fa-447a-3b79-8e19-04de8e88b931"},
        {"", "fc5bc365-aedf-30a8-8b89-04e462e29bde"},
        {"Ondes_VOXEL", "23552d0d-8633-3383-ab18-825f5be1c6a3"},
        // Non-ASCII, which also pins the UTF-8 encoding of the name itself.
        {"éàü", "bec8a32e-e55f-36b7-a22d-e38aa462a5e8"},
    };

    for (const auto& [name, expected] : kCases) {
        REQUIRE(Uuid::offline_player(name).to_string() == expected);
    }
}

TEST_CASE("an offline UUID is version 3 with the RFC 4122 variant", "[protocol][uuid]") {
    // Skipping these bits produces an id that looks entirely plausible and
    // matches nothing a real client or a real save would use.
    const Uuid uuid = Uuid::offline_player("Notch");

    const auto version = static_cast<u8>((uuid.most_significant >> 12) & 0xF);
    REQUIRE(version == 3);

    const auto variant = static_cast<u8>((uuid.least_significant >> 62) & 0x3);
    REQUIRE(variant == 2);  // binary 10: the RFC 4122 variant
}

TEST_CASE("the same name always yields the same identity", "[protocol][uuid]") {
    REQUIRE(Uuid::offline_player("Martin") == Uuid::offline_player("Martin"));
    // Case matters: "Martin" and "martin" are different players offline.
    REQUIRE_FALSE(Uuid::offline_player("Martin") == Uuid::offline_player("martin"));
}

TEST_CASE("an offline UUID round-trips through its text form", "[protocol][uuid]") {
    const Uuid uuid = Uuid::offline_player("Ondes_VOXEL");
    REQUIRE(Uuid::parse(uuid.to_string()).value() == uuid);
}
