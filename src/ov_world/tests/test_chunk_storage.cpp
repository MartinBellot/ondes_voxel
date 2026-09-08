#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::world;

TEST_CASE("a chunk declares the version that wrote it", "[world][storage][version]") {
    // Read before anything else, and read on its own: a file from another
    // version may use shapes this one does not have, and vanilla upgrades old
    // saves through a converter this project does not implement. The only
    // honest answers are "this version" and "refuse" — and refusing has to
    // happen before anything is written back, because replacing a chunk we
    // failed to understand is how a save gets destroyed by the program meant
    // to open it.
    nbt::Document document;
    document.root = nbt::Tag::make_compound();
    REQUIRE_FALSE(chunk_data_version(document).has_value());

    document.root.compound()->push_back(
        nbt::CompoundEntry{"DataVersion", nbt::Tag{kDataVersion1201}});
    REQUIRE(chunk_data_version(document) == kDataVersion1201);

    // A future version reads back as itself rather than as an error, so the
    // caller can say which one it refused.
    document.root.compound()->clear();
    document.root.compound()->push_back(nbt::CompoundEntry{"DataVersion", nbt::Tag{i32{4082}}});
    REQUIRE(chunk_data_version(document) == 4082);
}
