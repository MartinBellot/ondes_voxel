#define OV_LOG_CATEGORY "server"

#include "world_structures.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_nbt.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "ov/worldgen/structure_stage.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <utility>

namespace ov::server {

namespace {

/// The generator as the structure layer sees it: the biome at a block, the two
/// first-free heights of a column from the noise, and the base column.
///
/// The two height rules are the ones `ov_structparity` and `ov_structblocks`
/// measured the placement and the level-C blocks with: the surface counts the
/// sea as filled, the ocean floor does not. The column is the generator's own
/// extent — 319 to -64 in the overworld, 127 to 0 in the Nether and the End.
class GeneratorSampler final : public worldgen::StructureWorldSampler {
public:
    explicit GeneratorSampler(const worldgen::ChunkGenerator& generator)
        : generator_(&generator),
          low_(generator.gen_min_y()),
          high_(generator.gen_min_y() + generator.gen_depth() - 1) {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return generator_->biome_name_at(x, y, z);
    }

    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        // ── jigsaw ── WORLD_SURFACE_WG is the block the noise stage would
        // write, aquifer included: a dry pocket below the sea level is air.
        // Measured on the jigsaw joints (docs/provenance/jigsaw.md § 3.1): the
        // taiga village of reference-987654321 starts at 57, not at the 62 the
        // global sea gives, and reference-1234567890 goes from 16/18 to 18/18.
        if (generator_->aquifer_active()) {
            worldgen::AquiferSampler aquifer{*generator_->aquifer()};
            for (i32 y = high_; y >= low_; --y) {
                if (aquifer.compute(x, y, z, generator_->density_at(x, y, z)).substance !=
                    worldgen::Substance::Air) {
                    return y + 1;
                }
            }
            return low_;
        }
        const i32 sea = generator_->sea_level();
        for (i32 y = high_; y >= low_; --y) {
            if (y < sea || generator_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return low_;
    }

    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        for (i32 y = high_; y >= low_; --y) {
            if (generator_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return low_;
    }

    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return generator_->is_solid(x, y, z);
    }

private:
    const worldgen::ChunkGenerator* generator_;
    i32                             low_;
    i32                             high_;
};

}  // namespace

// Declaration order is destruction order reversed: the placer borrows the
// sets, the builder the tags.
struct WorldStructures::Impl {
    std::optional<worldgen::StructureSetRegistry> sets;
    std::optional<worldgen::BlockTags>            tags;
    std::optional<worldgen::StructurePlacer>      placer;
    std::optional<worldgen::StructureBuilder>     builder;
};

WorldStructures::WorldStructures(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WorldStructures::~WorldStructures() = default;

std::filesystem::path WorldStructures::default_jar(const std::filesystem::path& data_root) {
    if (const char* configured = std::getenv("OV_SERVER_JAR"); configured != nullptr) {
        return std::filesystem::path{configured};
    }
    std::error_code       ignored;
    std::filesystem::path root = std::filesystem::absolute(data_root, ignored).lexically_normal();
    if (!root.has_filename()) {
        root = root.parent_path();
    }
    return root.parent_path() / "tools" / "vanilla" / "server.jar";
}

std::unique_ptr<WorldStructures> WorldStructures::load(const std::filesystem::path& data,
                                                       const std::filesystem::path& server_jar,
                                                       const registry::BlockRegistry& blocks,
                                                       const std::vector<std::string_view>& biomes) {
    auto impl = std::make_unique<Impl>();
    auto sets = worldgen::StructureSetRegistry::load(data);
    if (!sets) {
        OV_LOG_ERROR("structures: structure sets: {}", worldgen::to_string(sets.error()));
        return nullptr;
    }
    impl->sets.emplace(std::move(*sets));
    auto placer = worldgen::StructurePlacer::load(data, *impl->sets);
    if (!placer) {
        OV_LOG_ERROR("structures: {}", worldgen::to_string(placer.error()));
        return nullptr;
    }
    impl->placer.emplace(std::move(*placer));
    impl->placer->restrict_to_biomes(biomes);

    auto tags = worldgen::BlockTags::load(data, blocks);
    if (!tags) {
        OV_LOG_ERROR("structures: the block tags did not load");
        return nullptr;
    }
    impl->tags.emplace(std::move(*tags));
    std::string detail;
    auto builder = worldgen::StructureBuilder::load(server_jar, data, blocks, *impl->tags, &detail);
    if (!builder) {
        OV_LOG_ERROR("structures: the templates could not be read from {}: {}",
                     server_jar.string(), detail);
        return nullptr;
    }
    impl->builder.emplace(std::move(*builder));
    // ── jigsaw ── the biome of a jigsaw start is read where its piece lands
    impl->placer->set_jigsaw(impl->builder->jigsaw());
    return std::unique_ptr<WorldStructures>(new WorldStructures(std::move(impl)));
}

WorldStructures::StackStage::StackStage()  = default;
WorldStructures::StackStage::~StackStage() = default;

std::unique_ptr<WorldStructures::StackStage> WorldStructures::make_stage(
    const worldgen::ChunkGenerator& generator, const registry::BlockRegistry& blocks,
    const registry::Registries& registries, i64 seed) const {
    auto out     = std::make_unique<StackStage>();
    out->placer  = &*impl_->placer;
    out->sampler = std::make_unique<GeneratorSampler>(generator);
    out->stage   = std::make_unique<worldgen::StructureStage>(*impl_->placer, *impl_->builder,
                                                            out->sampler.get(), blocks,
                                                            &registries, seed);
    out->stage->refuse(worldgen::StructureKind::RuinedPortal,
                       "ruined portal: its height search is not implemented, it would stand at y 0");
    out->stage->refuse(
        worldgen::StructureKind::BuriedTreasure,
        "buried treasure: its downward search is not implemented, its chest would hang at y 90");
    return out;
}

void WorldStructures::StackStage::record(world::Chunk& chunk) {
    const i32 chunk_x = chunk.position().x;
    const i32 chunk_z = chunk.position().z;
    const auto column =
        worldgen::BoundingBox::chunk_column(chunk_x, chunk_z, -2048, 2047);

    // A start is referenced by its box — 12 blocks wider for a structure with
    // terrain adaptation, which is why the search reaches one chunk further
    // than the pieces do. Those starts are cached already: the stage keeps
    // `kReach` chunks around every decorated chunk, and a finished chunk's
    // neighbours were all decorated.
    std::vector<worldgen::StructureReference> references;
    // ── jigsaw ── Each set within its own reach, plus one chunk for the
    // terrain adaptation margin: the grid candidates only, not every chunk of
    // a 27 x 27 square. The order is the stage's, z then x, as before.
    auto reaching = stage->starts_reaching(chunk_x, chunk_z, 1);
    std::stable_sort(reaching.begin(), reaching.end(),
                     [](const worldgen::StructureStart* a, const worldgen::StructureStart* b) {
                         return std::tie(a->chunk_z, a->chunk_x) < std::tie(b->chunk_z, b->chunk_x);
                     });
    for (const worldgen::StructureStart* start : reaching) {
        worldgen::BoundingBox box = start->box;
        const worldgen::StructureDefinition* definition =
            placer != nullptr ? placer->find(start->structure) : nullptr;
        if (definition != nullptr && definition->terrain_adaptation) {
            constexpr i32 kMargin = worldgen::kTerrainAdaptationMargin;
            box = {box.min_x - kMargin, box.min_y, box.min_z - kMargin,
                   box.max_x + kMargin, box.max_y, box.max_z + kMargin};
        }
        if (box.intersects(column)) {
            references.push_back({start->structure, start->chunk_x, start->chunk_z});
        }
    }
    const auto& here = stage->starts_at(chunk_x, chunk_z);
    if (here.empty() && references.empty()) {
        return;
    }
    chunk.set_structures(worldgen::chunk_structures_to_nbt(here, references));
}

void WorldStructures::StackStage::report(std::string_view dimension) {
    for (const auto& [reason, count] : stage->stats().refused) {
        if (reported.insert(reason).second) {
            OV_LOG_INFO("structures ({}): not placed — {} (counted from now on)", dimension,
                        reason);
        }
    }
}

}  // namespace ov::server
