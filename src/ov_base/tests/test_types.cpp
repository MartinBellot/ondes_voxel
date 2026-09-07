#include "ov/base/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <unordered_set>

using namespace ov;

namespace {
struct BlockStateTag {};

struct ItemTag {};

using BlockStateId = Id<BlockStateTag, u16>;
using ItemId       = Id<ItemTag, u16>;
}  // namespace

TEST_CASE("fixed-width aliases have the widths their names claim", "[types]") {
    // Palette packing, NBT and the wire protocol all depend on these exactly.
    STATIC_REQUIRE(sizeof(i8) == 1);
    STATIC_REQUIRE(sizeof(i16) == 2);
    STATIC_REQUIRE(sizeof(i32) == 4);
    STATIC_REQUIRE(sizeof(i64) == 8);
    STATIC_REQUIRE(sizeof(u8) == 1);
    STATIC_REQUIRE(sizeof(u16) == 2);
    STATIC_REQUIRE(sizeof(u32) == 4);
    STATIC_REQUIRE(sizeof(u64) == 8);
    STATIC_REQUIRE(sizeof(f32) == 4);
    STATIC_REQUIRE(sizeof(f64) == 8);
}

TEST_CASE("a strong id costs nothing over its representation", "[types]") {
    // BlockStateId is stored 4096 times per chunk section. It must be exactly
    // as small as the u16 it wraps, and trivially copyable.
    STATIC_REQUIRE(sizeof(BlockStateId) == sizeof(u16));
    STATIC_REQUIRE(std::is_trivially_copyable_v<BlockStateId>);
    STATIC_REQUIRE(std::is_trivially_destructible_v<BlockStateId>);
}

TEST_CASE("strong ids of different tags do not interconvert", "[types]") {
    // This is the entire point: a BlockStateId and an ItemId are both u16, and
    // swapping them compiles cleanly without the tag. It would produce a world
    // full of wrong blocks with no diagnostic anywhere.
    STATIC_REQUIRE_FALSE(std::is_convertible_v<BlockStateId, ItemId>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<ItemId, BlockStateId>);

    // Nor does a raw integer become an id implicitly.
    STATIC_REQUIRE_FALSE(std::is_convertible_v<u16, BlockStateId>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<BlockStateId, u16>);
}

TEST_CASE("strong ids compare and order by value", "[types]") {
    constexpr BlockStateId air{0};
    constexpr BlockStateId stone{1};

    REQUIRE(air == BlockStateId{0});
    REQUIRE(air != stone);
    REQUIRE(air < stone);
    REQUIRE(stone.value() == 1);

    // Default construction is id 0, which the registry reserves for
    // minecraft:air so that a zeroed section is an empty section.
    STATIC_REQUIRE(BlockStateId{}.value() == 0);
}

TEST_CASE("strong ids can key unordered containers", "[types]") {
    std::unordered_set<BlockStateId> seen;
    seen.insert(BlockStateId{7});
    seen.insert(BlockStateId{7});
    seen.insert(BlockStateId{9});

    REQUIRE(seen.size() == 2);
    REQUIRE(seen.contains(BlockStateId{7}));
    REQUIRE_FALSE(seen.contains(BlockStateId{8}));
}
