// Explosions, against the game.
//
// A single shot cannot be compared to a single shot: the energy of every one of
// the 1352 rays is rolled, so vanilla does not make the same crater twice and
// neither do we. What *is* comparable is the shape over many shots, and this
// file compares three of them, cell by cell:
//
//   * the **union** — every cell any of K shots took;
//   * the **intersection** — the cells all K shots took;
//   * the **frequency** of each cell, shot for shot.
//
// The table comes from scripts/measure_blast.py crater, which fires K charges
// on a real 1.20.1 server inside a rebuilt box of one material and reads the
// box back out of the region files each time. It is derived from Mojang's data
// and never committed, so the parity case skips loudly rather than passing on
// an empty comparison.
#include "ov/gameplay/explosion.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;
using Catch::Approx;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] const registry::BlockRegistry* pack() {
    static const auto loaded = registry::BlockRegistry::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

/// A solid box of one material around the origin, air outside it.
///
/// The charge's own cell is **not** air. A primed TNT summoned inside a solid
/// box leaves the block it is standing in alone, so every ray pays for that
/// first block before it has gone anywhere — which is worth a whole block of
/// reach on soft material and is the difference between this bench and one
/// where the TNT block turned itself into air.
class BoxLevel final : public world::LevelView {
public:
    BoxLevel(const registry::BlockRegistry& registry, registry::BlockStateId material, i32 half,
             i32 up)
        : registry_{&registry}, material_{material}, half_{half}, up_{up} {
        air_ = registry.default_state(registry.find_block("minecraft:air").value());
    }

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const bool inside = std::abs(pos.x) <= half_ && std::abs(pos.z) <= half_ &&
                            std::abs(pos.y) <= up_;
        return inside ? material_ : air_;
    }
    [[nodiscard]] bool is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         material_{};
    registry::BlockStateId         air_{};
    i32                            half_{0};
    i32                            up_{0};
};

/// Nothing anywhere. A charge here has nothing to eat and nothing to break.
class EmptyLevel final : public world::LevelView {
public:
    explicit EmptyLevel(const registry::BlockRegistry& registry) : registry_{&registry} {
        air_ = registry.default_state(registry.find_block("minecraft:air").value());
    }
    [[nodiscard]] registry::BlockStateId block_at(BlockPos) const override { return air_; }
    [[nodiscard]] bool is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         air_{};
};

[[nodiscard]] registry::BlockStateId state_of(const registry::BlockRegistry& registry,
                                              std::string_view              name) {
    const auto block = registry.find_block(name);
    REQUIRE(block.has_value());
    return registry.default_state(*block);
}

/// Which cells one charge takes out of a solid box of `material`.
[[nodiscard]] std::vector<BlockPos> crater_in_box(const registry::BlockRegistry& registry,
                                                  std::string_view material, i64 seed,
                                                  i32 half = 9, i32 up = 7) {
    const Explosions   rules{registry};
    const BoxLevel     level{registry, state_of(registry, material), half, up};
    math::LegacyRandomSource rng{seed};
    ExplosionSpec      spec{};
    spec.centre = Vec3d{0.5, 0.06125, 0.5};
    spec.power  = kTntPower;
    std::vector<BlockPos> taken;
    rules.collect_blocks(level, spec, rng, taken);
    return taken;
}

/// How many cells one charge takes out of a solid box of `material`.
[[nodiscard]] usize destroyed_in_box(const registry::BlockRegistry& registry,
                                     std::string_view material, i64 seed) {
    return crater_in_box(registry, material, seed).size();
}

struct CraterTable {
    i32                                            trials{0};
    i32                                            half{0};
    i32                                            up{0};
    f32                                            power{4.0F};
    Vec3d                                          centre{};
    std::map<std::string, std::map<i64, i32>>      counts;
};

[[nodiscard]] i64 key_of(i32 dx, i32 dy, i32 dz) {
    return (static_cast<i64>(dx + 64) << 32) | (static_cast<i64>(dy + 64) << 16) |
           static_cast<i64>(dz + 64);
}

[[nodiscard]] std::optional<CraterTable> load_crater_table() {
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "normalized" / "blast_crater.txt";
    std::ifstream file{path};
    if (!file) {
        return std::nullopt;
    }
    CraterTable table;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream in{line};
        if (line[0] == '#') {
            std::string hash;
            std::string tag;
            in >> hash;
            while (in >> tag) {
                if (tag == "trials") {
                    in >> table.trials;
                } else if (tag == "half") {
                    in >> table.half;
                } else if (tag == "up") {
                    in >> table.up;
                } else if (tag == "power") {
                    in >> table.power;
                } else if (tag == "centre") {
                    in >> table.centre.x >> table.centre.y >> table.centre.z;
                }
            }
            continue;
        }
        std::string material;
        i32         dx = 0;
        i32         dy = 0;
        i32         dz = 0;
        i32         count = 0;
        in >> material >> dx >> dy >> dz >> count;
        table.counts[material][key_of(dx, dy, dz)] = count;
    }
    return table;
}

}  // namespace

TEST_CASE("the grid fires 1352 rays, not 4096", "[explosion]") {
    if (pack() == nullptr) {
        WARN("no registry.ovpack — run tools/ov_datagen/ovpack.py");
        return;
    }
    const Explosions rules{*pack()};
    // 16³ - 14³. Counting every cell of the grid would fire the interior
    // directions several times over and thicken the crater along the axes.
    CHECK(rules.ray_count() == 1352);
}

TEST_CASE("a fluid answers 100 and a dry block answers its own", "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const auto&      registry = *pack();
    const Explosions rules{registry};

    const auto stone = registry.find_block("minecraft:stone");
    REQUIRE(stone.has_value());
    CHECK(rules.resistance_of(registry.default_state(*stone)) == Approx(6.0F).margin(0.001));

    const auto obsidian = registry.find_block("minecraft:obsidian");
    REQUIRE(obsidian.has_value());
    CHECK(rules.resistance_of(registry.default_state(*obsidian)) == Approx(1200.0F).margin(0.1));

    // A waterlogged block gives water's answer, not its own: the game takes the
    // larger of the two, and that is why a flooded corridor survives.
    const auto slab = registry.find_block("minecraft:oak_slab");
    REQUIRE(slab.has_value());
    const std::vector<std::pair<std::string_view, std::string_view>> wet_properties{
        {"waterlogged", "true"}, {"type", "bottom"}};
    const auto wet = registry.state_for(*slab, wet_properties);
    REQUIRE(wet.has_value());
    CHECK(rules.resistance_of(*wet) == Approx(100.0F).margin(0.001));
}

TEST_CASE("a charge in open air takes nothing", "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const Explosions      rules{*pack()};
    const EmptyLevel      level{*pack()};
    math::LegacyRandomSource rng{1234};
    ExplosionSpec         spec{};
    spec.centre = Vec3d{0.5, 0.5, 0.5};
    std::vector<BlockPos> taken;
    rules.collect_blocks(level, spec, rng, taken);
    CHECK(taken.empty());
}

TEST_CASE("the same seed makes the same crater", "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const auto a = crater_in_box(*pack(), "minecraft:dirt", 42);
    const auto b = crater_in_box(*pack(), "minecraft:dirt", 42);
    REQUIRE_FALSE(a.empty());
    CHECK(a == b);
    // And a different one does not — compared cell by cell rather than by
    // size, because two different craters very often have the same size and a
    // count would let a generator that is never drawn from pass.
    const auto c = crater_in_box(*pack(), "minecraft:dirt", 43);
    CHECK(c != a);
}

TEST_CASE("harder blocks give up fewer cells, in the game's own order", "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const auto& registry = *pack();
    const auto  glass    = destroyed_in_box(registry, "minecraft:glass", 7);
    const auto  dirt     = destroyed_in_box(registry, "minecraft:dirt", 7);
    const auto  planks   = destroyed_in_box(registry, "minecraft:oak_planks", 7);
    const auto  stone    = destroyed_in_box(registry, "minecraft:stone", 7);
    const auto  obsidian = destroyed_in_box(registry, "minecraft:obsidian", 7);

    // The control the redstone campaign named: an ordering that came out with
    // obsidian more fragile than glass would be wrong whatever the numbers.
    CHECK(obsidian == 0);
    CHECK(stone < planks);
    CHECK(planks < dirt);
    CHECK(dirt < glass);
}

TEST_CASE("blast resistance is measured for every state the pack carries", "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const auto&      registry = *pack();
    const Explosions rules{registry};
    usize            missing = 0;
    for (usize index = 0; index < registry.state_count(); ++index) {
        if (!rules.resistance_measured(registry::BlockStateId{static_cast<u16>(index)})) {
            ++missing;
        }
    }
    // A block with no resistance would be the most fragile in the game. The
    // pack carries one for every block, so this is zero and stays zero.
    CHECK(missing == 0);
}

TEST_CASE("damage falls off with distance and stops at twice the power", "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const Explosions rules{*pack()};
    const EmptyLevel level{*pack()};
    ExplosionSpec    spec{};
    spec.centre = Vec3d{0.0, 0.0, 0.0};
    spec.power  = kTntPower;

    const auto at = [&](f64 distance) {
        const Vec3d feet{distance, 0.0, 0.0};
        return rules.hit_entity(level, spec, AABB::from_entity(feet, 0.6, 1.8), feet, 1.62);
    };

    const auto near   = at(1.0);
    const auto middle = at(4.0);
    const auto far    = at(7.9);
    const auto beyond = at(8.5);

    CHECK(near.touched);
    CHECK(middle.touched);
    CHECK(far.touched);
    CHECK_FALSE(beyond.touched);
    CHECK(near.damage > middle.damage);
    CHECK(middle.damage > far.damage);
    // Exposure is total in an empty world, so the impact is the distance term
    // alone and the damage is the floored formula on it.
    CHECK(near.exposure == Approx(1.0));
    const f64 impact = 1.0 - 1.0 / 8.0;
    CHECK(near.damage ==
          Approx(static_cast<f32>(static_cast<i32>((impact * impact + impact) / 2.0 * 7.0 * 8.0 +
                                                   1.0))));
    // The impulse points away from the charge, and its length is the impact.
    CHECK(near.impulse.x > 0.0);
    CHECK(near.impulse.length() == Approx(impact).margin(1e-9));
}

TEST_CASE("a chained TNT waits between an eighth and three eighths of its fuse",
          "[explosion]") {
    if (pack() == nullptr) {
        return;
    }
    const Explosions   rules{*pack()};
    math::LegacyRandomSource rng{99};
    i32                low  = 1000;
    i32                high = -1;
    for (i32 i = 0; i < 4000; ++i) {
        const i32 fuse = rules.chained_fuse(kTntFuseTicks, rng);
        low            = std::min(low, fuse);
        high           = std::max(high, fuse);
    }
    CHECK(low == 10);
    CHECK(high == 29);
}

TEST_CASE("the crater a real server made, cell by cell", "[explosion][parity]") {
    if (pack() == nullptr) {
        WARN("no registry.ovpack — run tools/ov_datagen/ovpack.py");
        return;
    }
    const auto table = load_crater_table();
    if (!table) {
        WARN("no blast_crater.txt — run scripts/measure_blast.py crater");
        return;
    }
    const auto&      registry = *pack();
    const Explosions rules{registry};

    usize total_cells = 0;
    usize union_agree  = 0;
    usize union_ours   = 0;
    usize union_theirs = 0;
    usize core_agree   = 0;
    usize core_ours    = 0;
    usize core_theirs  = 0;
    f64   frequency_gap = 0.0;

    for (const auto& [material, cells] : table->counts) {
        const BoxLevel level{registry, state_of(registry, material), table->half + 1,
                             table->up + 1};
        ExplosionSpec  spec{};
        // The measured cells are keyed **relative to the charge's own block**,
        // so the box here sits on the origin and the centre keeps only what is
        // inside that block. Using the absolute y the bench worked at would put
        // the charge forty blocks under the box, in open air, and take nothing
        // at all — which is exactly what the first run of this comparison did.
        spec.centre = Vec3d{table->centre.x - std::floor(table->centre.x),
                            table->centre.y - std::floor(table->centre.y),
                            table->centre.z - std::floor(table->centre.z)};
        spec.power  = table->power;

        std::map<i64, i32> mine;
        math::LegacyRandomSource rng{20230612};
        for (i32 trial = 0; trial < table->trials; ++trial) {
            std::vector<BlockPos> taken;
            rules.collect_blocks(level, spec, rng, taken);
            for (const BlockPos pos : taken) {
                // Only the window the measurement read back exists on both
                // sides; a cell outside it was never looked at over there.
                if (std::abs(pos.x) <= table->half && std::abs(pos.z) <= table->half &&
                    std::abs(pos.y) <= table->up) {
                    ++mine[key_of(pos.x, pos.y, pos.z)];
                }
            }
        }

        std::set<i64> keys;
        for (const auto& [key, count] : cells) {
            keys.insert(key);
        }
        for (const auto& [key, count] : mine) {
            keys.insert(key);
        }
        usize agree = 0;
        usize only_us = 0;
        usize only_them = 0;
        usize core_a = 0;
        usize core_u = 0;
        usize core_t = 0;
        f64   gap = 0.0;
        for (const i64 key : keys) {
            const auto theirs_it = cells.find(key);
            const auto ours_it   = mine.find(key);
            const i32  theirs    = theirs_it == cells.end() ? 0 : theirs_it->second;
            const i32  ours      = ours_it == mine.end() ? 0 : ours_it->second;
            if ((theirs != 0) == (ours != 0)) {
                ++agree;
            } else if (ours != 0) {
                ++only_us;
            } else {
                ++only_them;
            }
            const bool theirs_always = theirs == table->trials;
            const bool ours_always   = ours == table->trials;
            if (theirs_always == ours_always) {
                ++core_a;
            } else if (ours_always) {
                ++core_u;
            } else {
                ++core_t;
            }
            gap += std::abs(static_cast<f64>(theirs - ours)) / static_cast<f64>(table->trials);
        }
        total_cells += keys.size();
        union_agree += agree;
        union_ours += only_us;
        union_theirs += only_them;
        core_agree += core_a;
        core_ours += core_u;
        core_theirs += core_t;
        frequency_gap += gap;
        WARN("crater " << material << ": " << keys.size() << " cells | union " << agree
                       << " agree / " << only_us << " ours / " << only_them
                       << " theirs | core " << core_a << " agree / " << core_u << " ours / "
                       << core_t << " theirs | mean |df| "
                       << gap / static_cast<f64>(keys.size()));
    }

    WARN("crater total: " << total_cells << " cells | union " << union_agree << " agree / "
                          << union_ours << " ours only / " << union_theirs
                          << " theirs only | core " << core_agree << " agree / " << core_ours
                          << " ours only / " << core_theirs << " theirs only | mean |df| "
                          << frequency_gap / static_cast<f64>(total_cells));
    REQUIRE(total_cells > 0);
    // The union over K shots is a converging estimate on both sides, so a
    // handful of rim cells can differ. Anything worse than this is a shape
    // difference, not a tail.
    CHECK(static_cast<f64>(union_agree) / static_cast<f64>(total_cells) > 0.97);
}
