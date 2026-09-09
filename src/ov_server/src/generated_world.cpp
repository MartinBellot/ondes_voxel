#define OV_LOG_CATEGORY "server"

#include "generated_world.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ov::server {

/// Held in declaration order, because that is destruction order reversed and
/// the generator must go before the router it points at.
struct GeneratedWorld::Impl {
    i64 seed{0};

    worldgen::NoiseRouter   router;
    worldgen::BiomeSource   biomes;
    worldgen::SurfaceSystem surface;
    worldgen::FeatureRegistry features;
    worldgen::Decorator     decorator;

    worldgen::CarvingContext carving;
    worldgen::CarverStage    carvers;

    std::optional<worldgen::ChunkGenerator> generator;
    std::optional<worldgen::ChunkPipeline>  pipeline;

    /// Block-registry biome index -> the id the chunk packet uses. Built once,
    /// by name; see the header for why the two numberings are not assumed to
    /// be the same one.
    std::vector<u16> to_codec;

    Impl(i64 world_seed, worldgen::NoiseRouter&& r, worldgen::BiomeSource&& b,
         worldgen::SurfaceSystem&& s, worldgen::FeatureRegistry&& f, worldgen::Decorator&& d)
        : seed(world_seed),
          router(std::move(r)),
          biomes(std::move(b)),
          surface(std::move(s)),
          features(std::move(f)),
          decorator(std::move(d)),
          carving{router.min_y(), router.height()},
          carvers{world_seed, carving} {}
};

GeneratedWorld::GeneratedWorld(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GeneratedWorld::~GeneratedWorld() = default;

std::unique_ptr<GeneratedWorld> GeneratedWorld::load(
    const std::filesystem::path& data_root, const registry::BlockRegistry& blocks,
    const registry::Registries& registries, std::span<const std::string_view> codec_biomes,
    i64 seed) {
    const auto data    = data_root / "vanilla" / "1.20.1" / "generated" / "data" / "minecraft";
    const auto reports = data_root / "vanilla" / "1.20.1" / "generated";

    auto router = worldgen::NoiseRouter::load(data, "overworld", seed);
    if (!router) {
        OV_LOG_ERROR("worldgen: router: {} — run tools/ov_datagen first",
                     worldgen::to_string(router.error()));
        return nullptr;
    }
    auto biomes = worldgen::BiomeSource::load(reports, "overworld");
    if (!biomes) {
        OV_LOG_ERROR("worldgen: biome source: {}", worldgen::to_string(biomes.error()));
        return nullptr;
    }
    auto surface = worldgen::SurfaceSystem::load(data, "overworld", seed, blocks);
    if (!surface) {
        OV_LOG_ERROR("worldgen: surface rules: {}", worldgen::to_string(surface.error()));
        return nullptr;
    }
    auto features = worldgen::FeatureRegistry::load(data, blocks);
    if (!features) {
        OV_LOG_ERROR("worldgen: features: {}", worldgen::to_string(features.error()));
        return nullptr;
    }
    auto decorator = worldgen::Decorator::load(data, blocks, *features, *biomes);
    if (!decorator) {
        OV_LOG_ERROR("worldgen: decorator: {}", worldgen::to_string(decorator.error()));
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(seed, std::move(*router), std::move(*biomes),
                                       std::move(*surface), std::move(*features),
                                       std::move(*decorator));

    impl->generator.emplace(impl->router, impl->biomes, blocks);
    impl->generator->set_surface_system(&impl->surface);
    if (auto attached = impl->generator->set_carvers(&impl->carvers, registries); !attached) {
        // Refused rather than carried on without: carving without the tag
        // leaves a crust of dirt over every cave, and a world that looks nearly
        // right is worse than one that refuses to start.
        OV_LOG_ERROR("worldgen: carvers: {}", worldgen::to_string(attached.error()));
        return nullptr;
    }

    impl->pipeline.emplace(*impl->generator, &impl->decorator, blocks,
                           world::WorldShape::overworld(), seed);

    // The two numberings, reconciled by name rather than assumed equal.
    std::unordered_map<std::string_view, u16> by_name;
    for (usize i = 0; i < codec_biomes.size(); ++i) {
        by_name.emplace(codec_biomes[i], static_cast<u16>(i));
    }
    impl->to_codec.assign(blocks.biome_count(), 0);
    usize unnamed = 0;
    for (u32 index = 0; index < blocks.biome_count(); ++index) {
        const auto found = by_name.find(blocks.biome_name(index));
        if (found == by_name.end()) {
            if (unnamed < 8) {
                OV_LOG_ERROR("worldgen: the registry codec does not name biome {}",
                             blocks.biome_name(index));
            }
            ++unnamed;
            continue;
        }
        impl->to_codec[index] = found->second;
    }
    if (unnamed != 0) {
        OV_LOG_ERROR(
            "worldgen: {} of {} biomes are absent from the codec; refusing to generate rather "
            "than sending chunks whose biomes mean something else",
            unnamed, blocks.biome_count());
        return nullptr;
    }

    OV_LOG_INFO("worldgen: overworld ready at seed {} — noise, biomes, surface, carvers, features",
                seed);
    return std::unique_ptr<GeneratedWorld>(new GeneratedWorld(std::move(impl)));
}

world::Chunk GeneratedWorld::generate(i32 chunk_x, i32 chunk_z) {
    // `take` rather than a copy: `Full` means all eight neighbours have run
    // their features, so nothing can write into this chunk again and the
    // pipeline has no reason to keep it. The server's own cache owns it now.
    world::Chunk chunk = impl_->pipeline->take(chunk_x, chunk_z);

    // Biome indices out of the block registry, translated into the ids the
    // chunk packet carries. 1536 cells a chunk, once, against a world that
    // would otherwise be painted in another world's colours.
    const auto shape = chunk.shape();
    for (i32 y = shape.min_y; y <= shape.max_y(); y += 4) {
        for (usize z = 0; z < 16; z += 4) {
            for (usize x = 0; x < 16; x += 4) {
                const u16 index = chunk.get_biome(x, y, z);
                if (index < impl_->to_codec.size()) {
                    chunk.set_biome(x, y, z, impl_->to_codec[index]);
                }
            }
        }
    }
    // Bound what the pipeline holds. Eight chunks is more than the two the
    // feature stage's radius needs, so an ordinary streaming pattern — chunk
    // after neighbouring chunk — keeps its support and pays nothing; a jump
    // across the world drops a cache that was about to be useless anyway.
    constexpr i32 kKeep = 8;
    impl_->pipeline->trim(chunk_x, chunk_z, kKeep);

    return chunk;
}

i64 GeneratedWorld::seed() const noexcept { return impl_->seed; }

}  // namespace ov::server
