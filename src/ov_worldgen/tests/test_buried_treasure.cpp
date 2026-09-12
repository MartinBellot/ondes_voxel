// ── treasure ── The buried treasure's downward search, on one column whose
// blocks are made up: the rule reads states, not names.

#include "../src/buried_treasure.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>

using namespace ov;
using namespace ov::worldgen;

namespace {

constexpr registry::BlockStateId kAir{0};
constexpr registry::BlockStateId kWater{1};
constexpr registry::BlockStateId kSand{2};
constexpr registry::BlockStateId kSandstone{3};
constexpr registry::BlockStateId kGravel{4};
constexpr registry::BlockStateId kGranite{5};

/// One column, air above what is set; the ocean floor is the first block from
/// the top that is neither air nor water.
class Column final : public FeatureLevel {
public:
    std::map<i32, registry::BlockStateId> blocks;

    [[nodiscard]] registry::BlockStateId block_at(i32, i32 y, i32) const override {
        const auto found = blocks.find(y);
        return found != blocks.end() ? found->second : kAir;
    }
    bool set_block(i32, i32 y, i32, registry::BlockStateId state) override {
        blocks[y] = state;
        return true;
    }
    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override {
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            if (it->second != kAir && it->second != kWater) {
                return it->first + 1;
            }
        }
        return min_y();
    }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:beach";
    }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    void fill(i32 low, i32 high, registry::BlockStateId state) {
        for (i32 y = low; y <= high; ++y) {
            blocks[y] = state;
        }
    }
};

constexpr std::array<registry::BlockStateId, 2> kSupport{kSandstone, kGranite};

}  // namespace

TEST_CASE("a treasure goes down through the sand to the sandstone", "[structures][treasure]") {
    // reference-1234567890's treasure of chunk (4571, 3937): sand 61 to 62 over
    // sandstone from 59 up, the chest at 60 — in the sand's first layer that
    // rests on sandstone.
    Column beach;
    beach.fill(40, 59, kSandstone);
    beach.fill(60, 62, kSand);
    CHECK(buried_treasure_height(beach, kSupport, 0, 0) == 60);

    // Without sandstone among the supports, it would sink to the bottom.
    const std::array<registry::BlockStateId, 1> granite_only{kGranite};
    CHECK(buried_treasure_height(beach, granite_only, 0, 0) == -64);
}

TEST_CASE("a treasure under the sea goes down through the gravel, not the water",
          "[structures][treasure]") {
    // struct-locate-1234567890's treasure of chunk (-82, 15): water down to 49,
    // gravel at 47 to 48, granite below; the chest at 47.
    Column sea;
    sea.fill(30, 46, kGranite);
    sea.fill(47, 48, kGravel);
    sea.fill(49, 62, kWater);
    CHECK(buried_treasure_height(sea, kSupport, 0, 0) == 47);
}

TEST_CASE("a treasure stands on the floor itself when the floor is a support",
          "[structures][treasure]") {
    Column bare;
    bare.fill(30, 50, kSandstone);
    bare.fill(51, 62, kWater);
    CHECK(buried_treasure_height(bare, kSupport, 0, 0) == 51);
}
