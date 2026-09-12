// The Great Pyramid's traps fire under the server's own redstone.
//
// The pyramid (an original Ondes VOXEL structure, great_pyramid.hpp) is built
// on a flat synthetic desert, then handed to the same `Redstone` and `Pistons`
// the server ticks with. Each trap is checked twice: at rest it is consistent
// — nothing the engine would change on the first update, which matters
// because vanilla does not re-evaluate redstone when a chunk loads — and when
// its trigger is pulled, it fires.
#include "ov/gameplay/piston.hpp"
#include "ov/gameplay/redstone.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/worldgen/great_pyramid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<Redstone>                redstone;
    std::optional<Pistons>                 pistons;
};

[[nodiscard]] Loaded& loaded() {
    static Loaded state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        auto   blocks = registry::BlockRegistry::load(path);
        auto   regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
            out.redstone.emplace(*out.blocks, *out.registries);
            out.pistons.emplace(*out.blocks, *out.registries, *out.redstone);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] i64 key(i32 x, i32 y, i32 z) {
    return (static_cast<i64>(x) << 40) ^ (static_cast<i64>(y) << 20) ^ static_cast<i64>(z);
}

/// One sparse world, seen both as the structure level the pyramid is written
/// into and as the redstone world the traps run in.
class World final : public worldgen::StructureLevel, public RedstoneWorld {
public:
    explicit World(const registry::BlockRegistry& b) : blocks_{&b} {}

    // ── StructureLevel ──
    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto it = cells_.find(key(x, y, z)); it != cells_.end()) {
            return it->second;
        }
        return y < ground ? sand_ : registry::kAirState;
    }
    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        cells_[key(x, y, z)] = state;
        return true;
    }
    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override { return ground; }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:desert";
    }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }
    void set_block_entity(i32, i32, i32, nbt::Tag) override {}
    [[nodiscard]] const nbt::Tag* block_entity(i32, i32, i32) const override { return nullptr; }

    // ── RedstoneWorld ──
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        return block_at(pos.x, pos.y, pos.z);
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }
    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos.x, pos.y, pos.z)] = state;
    }
    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue,
                       world::TickPriority priority) override {
        queue.schedule(pos, what, delay, now, priority);
    }
    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue) const override {
        return queue.is_scheduled(pos, what);
    }
    [[nodiscard]] i64 game_time() const override { return now; }
    [[nodiscard]] i32 container_signal(BlockPos) const override { return -1; }
    [[nodiscard]] i32 entity_pressure(BlockPos pos) const override {
        return pos == standing ? 15 : 0;
    }

    void flag(BlockPos pos, std::string_view property, std::string_view value) {
        const registry::BlockStateId state = block_at(pos);
        const auto                   block = blocks_->block_of(state);
        const auto                   prop  = blocks_->find_property(block, property);
        REQUIRE(prop.has_value());
        for (u16 i = 0; i < prop->values.size(); ++i) {
            if (prop->values[i] == value) {
                set_block(pos, blocks_->with_property(state, *prop, i));
                return;
            }
        }
        FAIL("no such value");
    }
    [[nodiscard]] std::string_view name(BlockPos pos) const {
        return blocks_->block_name(blocks_->block_of(block_at(pos)));
    }
    [[nodiscard]] std::string_view value(BlockPos pos, std::string_view property) const {
        const registry::BlockStateId state = block_at(pos);
        const auto prop = blocks_->find_property(blocks_->block_of(state), property);
        return prop ? blocks_->property_value(state, *prop) : std::string_view{};
    }

    void settle(Pistons& pistons, Redstone& redstone, i64 ticks = 8) {
        for (i64 i = 0; i < ticks; ++i) {
            ++now;
            std::vector<world::ScheduledTick> due;
            queue.collect_due(now, due);
            for (const world::ScheduledTick& tick : due) {
                if (!pistons.scheduled_tick(*this, tick.pos, tick.what)) {
                    (void)redstone.scheduled_tick(*this, tick.pos, tick.what);
                }
            }
        }
    }

    i32                        ground{70};
    BlockPos                   standing{0, -999, 0};
    world::BlockTickScheduler  queue;
    i64                        now{0};

    void set_sand(registry::BlockStateId sand) { sand_ = sand; }

private:
    const registry::BlockRegistry*         blocks_;
    registry::BlockStateId                 sand_{registry::kAirState};
    std::map<i64, registry::BlockStateId>  cells_;
};

class Flat final : public worldgen::StructureWorldSampler {
public:
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:desert";
    }
    [[nodiscard]] i32 surface_height(i32, i32) const override { return 70; }
    [[nodiscard]] i32 ocean_floor_height(i32, i32) const override { return 70; }
    [[nodiscard]] std::optional<bool> base_solid(i32, i32 y, i32) const override { return y < 70; }
};

struct Built {
    worldgen::GreatPyramidLayout layout;
    std::unique_ptr<World>       world;
};

[[nodiscard]] Built build(const registry::BlockRegistry& blocks, i64 seed) {
    Built      out;
    const auto pyramid = worldgen::GreatPyramid::create(blocks);
    REQUIRE(pyramid);
    const Flat flat;
    const auto c = worldgen::great_pyramid_placement().candidate(seed, 0, 0);
    REQUIRE(pyramid->decide(seed, c.x, c.z, &flat, nullptr, &out.layout) ==
            worldgen::PyramidDecision::Placed);
    out.world = std::make_unique<World>(blocks);
    out.world->set_sand(blocks.default_state(*blocks.find_block("minecraft:sand")));
    for (i32 z = out.layout.box.min_z >> 4; z <= out.layout.box.max_z >> 4; ++z) {
        for (i32 x = out.layout.box.min_x >> 4; x <= out.layout.box.max_x >> 4; ++x) {
            (void)pyramid->place(*out.world, out.layout,
                                 worldgen::BoundingBox::chunk_column(x, z, -2048, 2047));
        }
    }
    return out;
}

#define REQUIRE_ENGINE()                                   \
    if (!loaded().redstone.has_value()) {                  \
        SKIP("registry.ovpack not built");                 \
    }                                                      \
    Redstone&                      redstone = *loaded().redstone; \
    Pistons&                       pistons  = *loaded().pistons;  \
    const registry::BlockRegistry& reg      = *loaded().blocks; \
    (void)redstone;                                        \
    (void)pistons

}  // namespace

TEST_CASE("the hall's pit: the plate primes the TNT under the floor", "[great_pyramid][traps]") {
    REQUIRE_ENGINE();
    for (const i64 seed : {i64{20260911}, i64{42}, i64{-3}, i64{777}}) {
        Built      b = build(reg, seed);
        World&     w = *b.world;
        const auto plate = b.layout.world(0, 0, -26);
        const auto tnt   = b.layout.world(0, -2, -26);
        REQUIRE(w.name(plate) == "minecraft:stone_pressure_plate");
        REQUIRE(w.name(tnt) == "minecraft:tnt");
        REQUIRE_FALSE(redstone.consumer_powered(w, tnt));  // at rest

        w.standing = plate;
        REQUIRE(redstone.plate_step(w, plate));
        REQUIRE(w.value(plate, "powered") == "true");
        REQUIRE(redstone.consumer_powered(w, tnt));  // fired
    }
}

TEST_CASE("the Queen's passage: tripping the wire fires both dispensers", "[great_pyramid][traps]") {
    REQUIRE_ENGINE();
    for (const i64 seed : {i64{20260911}, i64{42}, i64{-3}, i64{777}}) {
        Built      b = build(reg, seed);
        World&     w = *b.world;
        const auto hook_a = b.layout.world(5, 13, -11);
        const auto hook_b = b.layout.world(5, 13, -9);
        const auto disp_a = b.layout.world(5, 14, -12);
        const auto disp_b = b.layout.world(5, 14, -8);
        REQUIRE_FALSE(redstone.consumer_powered(w, disp_a));
        REQUIRE_FALSE(redstone.consumer_powered(w, disp_b));
        // What the string does when something crosses it: both hooks power.
        // (Entities on the string are the tick's job; the wiring is ours.)
        w.flag(hook_a, "powered", "true");
        w.flag(hook_b, "powered", "true");
        REQUIRE(redstone.consumer_powered(w, disp_a));
        REQUIRE(redstone.consumer_powered(w, disp_b));
    }
}

TEST_CASE("the labyrinth's arrow traps: the plate fires the dispenser behind it",
          "[great_pyramid][traps]") {
    REQUIRE_ENGINE();
    Built  b = build(reg, 20260911);
    World& w = *b.world;
    constexpr std::array<std::array<i32, 2>, 4> kDir{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
    std::vector<std::pair<BlockPos, BlockPos>> traps;  // plate, what it fires
    for (i32 j = 0; j < worldgen::GreatPyramidLayout::kMazeCells; ++j) {
        for (i32 i = 0; i < worldgen::GreatPyramidLayout::kMazeCells; ++i) {
            const auto kind = b.layout.dead_end_kind[worldgen::GreatPyramidLayout::cell(i, j)];
            if (kind != worldgen::DeadEnd::ArrowTrap && kind != worldgen::DeadEnd::TntTrap) {
                continue;
            }
            u8 exit = 0;
            for (u8 dir = 0; dir < 4; ++dir) {
                if (b.layout.open(i, j, dir)) {
                    exit = dir;
                }
            }
            const i32  u     = -29 + 2 * i;
            const i32  v     = -29 + 2 * j;
            const auto plate = b.layout.world(u, 14, v);
            const u8   back  = static_cast<u8>((exit + 2) & 3U);
            const auto target =
                kind == worldgen::DeadEnd::ArrowTrap
                    ? b.layout.world(u + kDir[back][0], 14, v + kDir[back][1])
                    : b.layout.world(u, 12, v);
            traps.emplace_back(plate, target);
        }
    }
    REQUIRE_FALSE(traps.empty());
    // Every trap at rest before any plate is pressed...
    for (const auto& [plate, target] : traps) {
        REQUIRE(w.value(plate, "powered") == "false");
        REQUIRE_FALSE(redstone.consumer_powered(w, target));
    }
    // ... and each fires when its plate is.
    for (const auto& [plate, target] : traps) {
        w.standing = plate;
        REQUIRE(redstone.plate_step(w, plate));
        REQUIRE(redstone.consumer_powered(w, target));
    }
}

TEST_CASE("the secret door: held shut by the lever, open when it is thrown",
          "[great_pyramid][traps][piston]") {
    REQUIRE_ENGINE();
    for (const i64 seed : {i64{20260911}, i64{42}, i64{-3}, i64{777}}) {
        Built      b     = build(reg, seed);
        World&     w     = *b.world;
        const auto lever = b.layout.world(-2, 27, 1);
        const auto lower = b.layout.world(-2, 26, 3);
        const auto upper = b.layout.world(-2, 27, 3);
        const auto head  = b.layout.world(-1, 26, 3);
        // The pistons face their heads.
        const i32       dx     = head.x - lower.x;
        const i32       dz     = head.z - lower.z;
        const Direction facing = dx > 0   ? Direction::East
                                 : dx < 0 ? Direction::West
                                 : dz > 0 ? Direction::South
                                          : Direction::North;
        // At rest the engine agrees with the stored state: both pistons want
        // to stay out — the upper one by the block the lever powers, the lower
        // one through the block above it.
        REQUIRE(pistons.wants_extended(w, upper, facing));
        REQUIRE(pistons.wants_extended(w, lower, facing));
        REQUIRE_FALSE(pistons.neighbour_changed(w, upper));
        REQUIRE_FALSE(pistons.neighbour_changed(w, lower));

        // Throw the lever: both retract and pull the door into the wall.
        w.flag(lever, "powered", "false");
        (void)pistons.neighbour_changed(w, upper);
        (void)pistons.neighbour_changed(w, lower);
        w.settle(pistons, redstone);
        const auto door_low  = b.layout.world(0, 26, 3);
        const auto door_high = b.layout.world(0, 27, 3);
        REQUIRE(w.name(door_low) == "minecraft:air");
        REQUIRE(w.name(door_high) == "minecraft:air");
        REQUIRE(w.value(upper, "extended") == "false");
        REQUIRE(w.value(lower, "extended") == "false");
        REQUIRE(w.name(b.layout.world(-1, 26, 3)) == "minecraft:cut_sandstone");
        REQUIRE(w.name(b.layout.world(-1, 27, 3)) == "minecraft:cut_sandstone");
    }
}
