// The chunk pipeline: the statuses, the neighbourhood rule, and the writes that
// leave the middle chunk.
//
// What a unit test can settle here is everything that is a *fact* rather than a
// degree of agreement with the real game:
//
//   * driving a chunk one status at a time produces exactly the chunk
//     `ChunkGenerator::generate()` produces, block for block — so splitting the
//     stages did not move a single block, and the game's status order (biomes
//     before noise) and this file's execution order (noise before biomes) are
//     interchangeable because neither stage reads the other's output;
//   * a chunk cannot reach `Features` until its eight neighbours have reached
//     `Carvers`, and cannot reach `Full` until they have reached `Features`;
//   * decoration really does write outside the chunk being decorated, and those
//     writes are still there afterwards — which is the whole reason this layer
//     exists and the one thing a single-chunk adaptor would have destroyed in
//     silence;
//   * the exclusion classes are disjoint, so the parallel scheme that is not
//     switched on yet is at least a checked rule rather than a hope.
//
// What the decoration is *worth* is not settled here and cannot be: the oracle
// for that is a world the real game wrote. tools/ov_genparity runs this
// pipeline over run/reference-1234567890 and reports how many of the game's
// ores land on the same block. See docs/provenance/pipeline-de-chunks.md.

#include "ov/registry/registries.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

[[nodiscard]] std::filesystem::path reports_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated";
}

[[nodiscard]] std::filesystem::path registry_pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
           "registry.ovpack";
}

constexpr i64 kSeed = 1234567890;

[[nodiscard]] bool data_present() {
    return std::filesystem::is_directory(data_root() / "worldgen") &&
           std::filesystem::is_regular_file(registry_pack());
}

/// The world as the structure placer needs to see it, over a generator.
///
/// The same two heightmap rules the parity harness uses: WORLD_SURFACE counts
/// water as something, OCEAN_FLOOR does not. Duplicated here rather than shared
/// because a test that borrowed the harness's version would stop testing the
/// day the harness changed.
class ColumnSampler final : public StructureWorldSampler {
public:
    explicit ColumnSampler(const ChunkGenerator& generator) : generator_{&generator} {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return generator_->biome_name_at(x, y, z);
    }

    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        const i32 sea = generator_->sea_level();
        for (i32 y = 319; y >= -64; --y) {
            if (generator_->is_solid(x, y, z) || y < sea) {
                return y + 1;
            }
        }
        return -64;
    }

    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        for (i32 y = 319; y >= -64; --y) {
            if (generator_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return -64;
    }

private:
    const ChunkGenerator* generator_;
};

}  // namespace

TEST_CASE("driving the statuses gives the chunk generate() gives", "[worldgen][pipeline]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(surface.has_value());

    const CarvingContext carving{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, carving};

    ChunkGenerator generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    REQUIRE(generator.set_carvers(&carvers, *registries).has_value());

    const auto shape = world::WorldShape::overworld();
    const auto air   = world::AirStates::from(*blocks);

    // The reference: one chunk, the whole of `generate()`.
    world::Chunk expected{ChunkPos{3, -5}, shape, air, &*blocks};
    generator.generate(expected);

    // The same chunk, one status at a time, with no decorator — so the only
    // thing that could differ is the split itself.
    ChunkPipeline pipeline{generator, nullptr, *blocks, shape, kSeed};
    const world::Chunk& built = pipeline.promote(3, -5, ChunkStatus::Carvers);

    usize differing = 0;
    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                if (built.get_block(x, y, z) != expected.get_block(x, y, z)) {
                    ++differing;
                }
            }
        }
    }
    CHECK(differing == 0);

    // The biomes too: they are the stage the split reordered relative to the
    // noise, and "neither reads the other" is exactly the claim being checked.
    usize differing_biomes = 0;
    for (i32 y = shape.min_y; y <= shape.max_y(); y += 4) {
        for (usize z = 0; z < 16; z += 4) {
            for (usize x = 0; x < 16; x += 4) {
                if (built.get_biome(x, y, z) != expected.get_biome(x, y, z)) {
                    ++differing_biomes;
                }
            }
        }
    }
    CHECK(differing_biomes == 0);
}

TEST_CASE("structure starts are decided before a block exists", "[worldgen][pipeline]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto sets = StructureSetRegistry::load(data_root());
    REQUIRE(sets.has_value());
    auto placer = StructurePlacer::load(data_root(), *sets);
    REQUIRE(placer.has_value());
    placer->restrict_to_biomes(biomes->biomes());

    ChunkGenerator generator{*router, *biomes, *blocks};
    const auto     shape = world::WorldShape::overworld();

    ChunkPipeline pipeline{generator, nullptr, *blocks, shape, kSeed};

    // No placer yet: the answer is empty, and empty is the honest answer rather
    // than a silent zero — nothing here pretends the structures happened.
    CHECK(pipeline.structure_starts(-7, -10).empty());
    CHECK(pipeline.stats().structure_starts == 0);

    ChunkPipeline with_structures{generator, nullptr, *blocks, shape, kSeed};
    ColumnSampler sampler{generator};
    with_structures.set_structures(&*placer, &sampler);

    // The mineshaft the real game started in chunk (-7, -10) at this seed.
    const auto starts = with_structures.structure_starts(-7, -10);
    REQUIRE(std::find(starts.begin(), starts.end(), "minecraft:mineshaft") != starts.end());

    // And the point of the status being where it is: the chunk is still empty.
    // A structure decision that needed generated blocks could not answer for a
    // chunk the world has not made, and the game answers for those constantly.
    CHECK(with_structures.status_of(-7, -10) == ChunkStatus::StructureStarts);
    CHECK(with_structures.stats().reached[static_cast<usize>(ChunkStatus::Noise)] == 0);
    CHECK(with_structures.stats().structure_starts >= 1);

    // A chunk with nothing in it costs nothing and says so.
    CHECK(with_structures.structure_starts(-6, -10).empty());
}

TEST_CASE("a chunk cannot decorate before its neighbours are carved",
          "[worldgen][pipeline]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(surface.has_value());
    auto features = FeatureRegistry::load(data_root(), *blocks);
    REQUIRE(features.has_value());
    auto decorator = Decorator::load(data_root(), *blocks, *features, *biomes);
    REQUIRE(decorator.has_value());

    const CarvingContext carving{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, carving};

    ChunkGenerator generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    REQUIRE(generator.set_carvers(&carvers, *registries).has_value());

    ChunkPipeline pipeline{generator, &*decorator, *blocks, world::WorldShape::overworld(), kSeed};

    // Nothing is cached before anything is asked for.
    CHECK(pipeline.status_of(0, 0) == ChunkStatus::Empty);

    (void)pipeline.promote(0, 0, ChunkStatus::Features);

    CHECK(pipeline.status_of(0, 0) == ChunkStatus::Features);
    // The eight neighbours were driven to the carvers by the rule, not by the
    // caller — and no further, because the feature stage's radius is one and
    // paying for more would be paying for nothing.
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            CHECK(pipeline.status_of(dx, dz) == ChunkStatus::Carvers);
        }
    }
    // And nothing two chunks away was touched at all.
    CHECK(pipeline.status_of(2, 0) == ChunkStatus::Empty);

    // Decoration wrote, and part of what it wrote left the middle chunk. This
    // is the number the whole layer exists for: a single-chunk adaptor would
    // report zero here and look perfectly healthy.
    const auto& after_features = pipeline.stats();
    CHECK(after_features.decorations == 1);
    CHECK(after_features.feature_writes > 0);
    CHECK(after_features.border_writes > 0);

    // Going to Full drives all eight neighbours through their own features, so
    // every write that can land in (0,0) has landed.
    (void)pipeline.promote(0, 0, ChunkStatus::Full);
    CHECK(pipeline.status_of(0, 0) == ChunkStatus::Full);
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            CHECK(pipeline.status_of(dx, dz) >= ChunkStatus::Features);
        }
    }
    CHECK(pipeline.stats().decorations == 9);

    // The cache is what keeps that from costing nine terrains per chunk: the
    // 5x5 the nine decorations need is generated once, twenty-five chunks, not
    // nine times nine.
    CHECK(pipeline.stats().reached[static_cast<usize>(ChunkStatus::Carvers)] == 25);

    // `take` is the server's path: it hands the finished chunk over and forgets
    // it, which is only safe because `Full` means nothing can write into it any
    // more. Checked here because the server has no headless client to prove it
    // end to end and this is the call it makes.
    const world::Chunk handed = pipeline.take(0, 0);
    CHECK(handed.position().x == 0);
    CHECK(handed.position().z == 0);
    CHECK(handed.non_air_count() > 0);
    CHECK(pipeline.status_of(0, 0) == ChunkStatus::Empty);

    // And trimming really drops what is far away rather than only saying so.
    pipeline.trim(0, 0, 1);
    CHECK(pipeline.status_of(2, 2) == ChunkStatus::Empty);
    CHECK(pipeline.status_of(1, 1) >= ChunkStatus::Carvers);
}

TEST_CASE("a write that leaves the middle chunk is still there afterwards",
          "[worldgen][pipeline]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(surface.has_value());
    auto features = FeatureRegistry::load(data_root(), *blocks);
    REQUIRE(features.has_value());
    auto decorator = Decorator::load(data_root(), *blocks, *features, *biomes);
    REQUIRE(decorator.has_value());

    const CarvingContext carving{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, carving};

    ChunkGenerator generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    REQUIRE(generator.set_carvers(&carvers, *registries).has_value());

    const auto shape = world::WorldShape::overworld();

    // Two pipelines over the same seed and the same chunk. The first stops at
    // the carvers — terrain and nothing else. The second decorates (0,0) and
    // only (0,0). Comparing the neighbour (1,0) between the two shows what
    // (0,0)'s decoration put into a chunk that has not been decorated itself.
    ChunkPipeline bare{generator, nullptr, *blocks, shape, kSeed};
    const world::Chunk& terrain = bare.promote(1, 0, ChunkStatus::Carvers);

    ChunkPipeline decorated{generator, &*decorator, *blocks, shape, kSeed};
    (void)decorated.promote(0, 0, ChunkStatus::Features);
    const world::Chunk& spilled = decorated.promote(1, 0, ChunkStatus::Carvers);

    usize changed = 0;
    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                if (terrain.get_block(x, y, z) != spilled.get_block(x, y, z)) {
                    ++changed;
                }
            }
        }
    }
    // Some of the blocks (0,0)'s features wrote landed in (1,0) and are still
    // there. Not "the count is plausible" — the count is nonzero, and that is
    // the property a single-chunk adaptor would break.
    CHECK(changed > 0);
    CHECK(decorated.stats().border_writes > 0);
}

TEST_CASE("two chunks of one exclusion class never write into the same chunk",
          "[worldgen][pipeline]") {
    // The rule a parallel scheduler would need, checked rather than asserted.
    // Decorating a chunk writes into the nine around it, so two chunks may run
    // at once exactly when those nine-chunk sets are disjoint.
    for (i32 z = -6; z <= 6; ++z) {
        for (i32 x = -6; x <= 6; ++x) {
            std::set<std::pair<i32, i32>> mine;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    mine.emplace(x + dx, z + dz);
                }
            }
            for (i32 oz = -6; oz <= 6; ++oz) {
                for (i32 ox = -6; ox <= 6; ++ox) {
                    if (ox == x && oz == z) {
                        continue;
                    }
                    if (ChunkPipeline::exclusive_class(ox, oz) !=
                        ChunkPipeline::exclusive_class(x, z)) {
                        continue;
                    }
                    for (i32 dz = -1; dz <= 1; ++dz) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            REQUIRE_FALSE(mine.contains({ox + dx, oz + dz}));
                        }
                    }
                }
            }
        }
    }

    // Nine classes, and every one of them used: a colouring that collapsed to
    // fewer would still pass the disjointness check above by leaving classes
    // empty.
    std::set<u8> seen;
    for (i32 z = -4; z <= 4; ++z) {
        for (i32 x = -4; x <= 4; ++x) {
            seen.insert(ChunkPipeline::exclusive_class(x, z));
        }
    }
    CHECK(seen.size() == 9);
}

// ── streaming ── The shared terrain cache changes how often terrain is
// computed, never what it is (TerrainCache in pipeline.hpp).
namespace {

/// The simplest correct cache: a map, no eviction, counted.
class MapTerrainCache final : public TerrainCache {
public:
    [[nodiscard]] bool fetch(i32 chunk_x, i32 chunk_z, world::Chunk& chunk,
                             std::vector<std::string_view>& starts) override {
        const auto found = items_.find(ChunkPos{chunk_x, chunk_z}.packed());
        if (found == items_.end()) {
            return false;
        }
        chunk  = found->second.first;
        starts = found->second.second;
        ++hits;
        return true;
    }

    void offer(i32 chunk_x, i32 chunk_z, const world::Chunk& chunk,
               const std::vector<std::string_view>& starts) override {
        items_.try_emplace(ChunkPos{chunk_x, chunk_z}.packed(), chunk, starts);
        ++offers;
    }

    usize hits{0};
    usize offers{0};

private:
    std::unordered_map<u64, std::pair<world::Chunk, std::vector<std::string_view>>> items_;
};

/// Cells, biome cells and heightmap columns that differ between two chunks.
[[nodiscard]] usize differences(const world::Chunk& a, const world::Chunk& b) {
    usize      differing = 0;
    const auto shape     = a.shape();
    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                differing += a.get_block(x, y, z) != b.get_block(x, y, z) ? 1U : 0U;
                if ((y & 3) == 0 && (x & 3) == 0 && (z & 3) == 0) {
                    differing += a.get_biome(x, y, z) != b.get_biome(x, y, z) ? 1U : 0U;
                }
            }
        }
    }
    for (const auto type : {world::HeightmapType::WorldSurface, world::HeightmapType::OceanFloor,
                            world::HeightmapType::MotionBlocking,
                            world::HeightmapType::MotionBlockingNoLeaves}) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                differing += a.heightmap(type).first_free(x, z) != b.heightmap(type).first_free(x, z)
                                 ? 1U
                                 : 0U;
            }
        }
    }
    return differing;
}

}  // namespace

TEST_CASE("terrain copied out of the cache is the terrain carved in place",
          "[worldgen][pipeline][streaming]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(surface.has_value());
    const CarvingContext carving{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, carving};
    ChunkGenerator       generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    REQUIRE(generator.set_carvers(&carvers, *registries).has_value());
    const auto shape = world::WorldShape::overworld();
    const auto air   = world::AirStates::from(*blocks);

    // Without a decorator the features stage does nothing at all — not even
    // carve the neighbours — so each three-by-three is carved explicitly.
    const auto carve_around = [](ChunkPipeline& pipeline, i32 centre_x, i32 centre_z) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                (void)pipeline.promote(centre_x + dx, centre_z + dz, ChunkStatus::Carvers);
            }
        }
    };

    // The reference carves everything itself, on demand, below.
    ChunkPipeline reference{generator, nullptr, *blocks, shape, kSeed};

    // One pipeline fills the cache; a second, next door, copies what it can.
    MapTerrainCache cache;
    ChunkPipeline   filler{generator, nullptr, *blocks, shape, kSeed};
    filler.set_terrain_cache(&cache);
    carve_around(filler, 0, 0);
    CHECK(cache.offers == 9);
    CHECK(filler.stats().terrain_hits == 0);

    ChunkPipeline copier{generator, nullptr, *blocks, shape, kSeed};
    copier.set_terrain_cache(&cache);
    carve_around(copier, 1, 0);  // x 0..2: six cached, three not
    CHECK(copier.stats().terrain_hits == 6);
    CHECK(cache.offers == 12);

    for (const auto [x, z] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, -1}, std::pair{0, 1}}) {
        CHECK(differences(copier.promote(x, z, ChunkStatus::Carvers),
                          reference.promote(x, z, ChunkStatus::Carvers)) == 0);
    }

    // A copy is the copier's own: writing into it leaves the cached chunk,
    // and the next copy, as they were (copy-on-write sections).
    world::Chunk                  mine{ChunkPos{0, 0}, shape, air, &*blocks};
    std::vector<std::string_view> starts;
    REQUIRE(cache.fetch(0, 0, mine, starts));
    const auto original = mine.get_block(1, 0, 1);
    mine.set_block(1, 0, 1,
                   original == registry::kAirState ? registry::BlockStateId{1} : registry::kAirState);
    world::Chunk again{ChunkPos{0, 0}, shape, air, &*blocks};
    REQUIRE(cache.fetch(0, 0, again, starts));
    CHECK(again.get_block(1, 0, 1) == original);
    CHECK(differences(again, reference.promote(0, 0, ChunkStatus::Carvers)) == 0);
}
