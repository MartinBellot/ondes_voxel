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
#include "ov/worldgen/structure_stage.hpp"  // ── structures ──
#include "ov/worldgen/structure_template.hpp"  // ── worldgen-3 ── the fossils
#include "ov/worldgen/surface_system.hpp"
#include "world_structures.hpp"  // ── structures ──

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ov::server {

/// One complete, self-contained worldgen stack.
///
/// Held in declaration order, because that is destruction order reversed and
/// the generator must go before the router it points at.
///
/// There is one of these **per thread that generates**, and that is the whole
/// of the concurrency design for worldgen. It is not a memory-for-simplicity
/// trade made on taste: `NoiseRouter`'s interpolated density nodes and the
/// surface system keep `mutable` memo caches (`src/ov_worldgen/src/density.cpp`
/// and `surface_system.cpp`), so one router driven from two threads is a data
/// race on an `unordered_map` — the kind that corrupts rather than the kind
/// that returns a stale number. Making those caches per-thread would mean
/// editing `src/ov_worldgen/`, which this work is not allowed to touch and
/// which another line of work is editing at the same time. Whole stacks are
/// the honest answer: nothing is shared, so nothing needs a lock.
struct GeneratedWorld::Stack {
    worldgen::NoiseRouter     router;
    worldgen::BiomeSource     biomes;
    worldgen::SurfaceSystem   surface;
    worldgen::FeatureRegistry features;

    /// Built *after* `features` is in its final home, never before.
    ///
    /// A Decorator keeps a raw pointer to the FeatureRegistry it was loaded
    /// from. Loading one from a local and then moving that local in here
    /// leaves the pointer aimed at a destroyed object — and it does not crash
    /// on the way in, it crashes the first time a chunk reaches the feature
    /// stage, which is on the first join and nowhere in the unit tests.
    std::optional<worldgen::Decorator> decorator;

    /// ── worldgen-3 ── The fossils' templates, which `decorator` borrows.
    /// Declared after it, so destroyed before it — the decorator never uses
    /// them while being destroyed.
    std::optional<worldgen::TemplateLibrary> templates;

    worldgen::CarvingContext carving;
    worldgen::CarverStage    carvers;

    std::optional<worldgen::ChunkGenerator> generator;
    std::optional<worldgen::ChunkPipeline>  pipeline;

    /// ── structures ── This stack's sampler and structure stage, attached to
    /// `pipeline`; null when the dimension has no structures to place.
    std::unique_ptr<WorldStructures::StackStage> structures;

    /// ── nether ── The chunk's shape: the overworld's, or the Nether's 256.
    world::WorldShape shape{world::WorldShape::overworld()};

    Stack(i64 world_seed, worldgen::NoiseRouter&& r, worldgen::BiomeSource&& b,
          worldgen::SurfaceSystem&& s, worldgen::FeatureRegistry&& f, bool nether,
          bool end = false)
        : router(std::move(r)),
          biomes(std::move(b)),
          surface(std::move(s)),
          features(std::move(f)),
          carving{router.min_y(), router.height()},
          carvers{nether ? worldgen::CarverStage::nether(world_seed)
                         : worldgen::CarverStage{world_seed, carving}},
          shape{nether ? world::WorldShape::nether()
                       : (end ? world::WorldShape::the_end() : world::WorldShape::overworld())} {}
};

struct GeneratedWorld::Impl {
    i64 seed{0};

    /// ── structures ── Shared by every stack, so declared before them: the
    /// stacks' stages borrow its placer and builder.
    std::string                      settings;
    std::unique_ptr<WorldStructures> structures;

    /// One per generating thread. `stacks[0]` belongs to whoever calls
    /// `generate` directly — the tick thread's fallback path.
    std::vector<std::unique_ptr<Stack>> stacks;

    /// Block-registry biome index -> the id the chunk packet uses. Built once,
    /// by name; see the header for why the two numberings are not assumed to
    /// be the same one. Immutable after `load`, which is what makes it the one
    /// thing every stack may share.
    std::vector<u16> to_codec;
};

namespace {

/// Build one stack, or say which piece is missing.
[[nodiscard]] std::unique_ptr<GeneratedWorld::Stack> build_stack(
    const std::filesystem::path& data, const std::filesystem::path& reports,
    const registry::BlockRegistry& blocks, const registry::Registries& registries, i64 seed,
    bool quiet, std::string_view settings) {
    const std::string name{settings};
    auto              router = worldgen::NoiseRouter::load(data, name, seed);
    if (!router) {
        OV_LOG_ERROR("worldgen: router: {} — run tools/ov_datagen first",
                     worldgen::to_string(router.error()));
        return nullptr;
    }
    auto biomes = worldgen::BiomeSource::load(reports, name);
    if (!biomes) {
        OV_LOG_ERROR("worldgen: biome source: {}", worldgen::to_string(biomes.error()));
        return nullptr;
    }
    auto surface = worldgen::SurfaceSystem::load(data, name, seed, blocks);
    if (!surface) {
        OV_LOG_ERROR("worldgen: surface rules: {}", worldgen::to_string(surface.error()));
        return nullptr;
    }
    auto features = worldgen::FeatureRegistry::load(data, blocks);
    if (!features) {
        OV_LOG_ERROR("worldgen: features: {}", worldgen::to_string(features.error()));
        return nullptr;
    }

    auto stack = std::make_unique<GeneratedWorld::Stack>(seed, std::move(*router),
                                                         std::move(*biomes), std::move(*surface),
                                                         std::move(*features), name == "nether",
                                                         name == "end");

    // From `stack->features` and `stack->biomes`, not from the locals that were
    // just moved out of them: see the comment on Stack::decorator.
    auto decorator = worldgen::Decorator::load(data, blocks, stack->features, stack->biomes);
    if (!decorator) {
        OV_LOG_ERROR("worldgen: decorator: {}", worldgen::to_string(decorator.error()));
        return nullptr;
    }
    stack->decorator = std::move(*decorator);
    // ── worldgen-3 ── The fossils are templates read from the server jar.
    // Without it they place nothing, and the log says so once.
    if (name == "overworld") {
        static constexpr std::array<std::string_view, 1> kFamilies{"fossil/"};
        std::string detail;
        // `reports` is <data root>/vanilla/1.20.1/generated; the jar is found
        // from the data root, as the structures find it.
        const auto data_root = reports.parent_path().parent_path().parent_path();
        auto       templates = worldgen::TemplateLibrary::open(
            WorldStructures::default_jar(data_root), blocks, kFamilies, &detail);
        if (templates) {
            stack->templates.emplace(std::move(*templates));
            stack->decorator->set_templates(&*stack->templates);
        } else {
            OV_LOG_WARN("worldgen: no fossil templates ({}): fossils will not be placed", detail);
        }
    }

    stack->generator.emplace(stack->router, stack->biomes, blocks);
    stack->generator->set_surface_system(&stack->surface);
    // ── end ── The End's biomes list no carver: nothing is cut there.
    if (name == "end") {
        stack->pipeline.emplace(*stack->generator, &*stack->decorator, blocks, stack->shape, seed);
        return stack;
    }
    if (auto attached = stack->generator->set_carvers(&stack->carvers, registries); !attached) {
        // Refused rather than carried on without: carving without the tag
        // leaves a crust of dirt over every cave, and a world that looks nearly
        // right is worse than one that refuses to start.
        OV_LOG_ERROR("worldgen: carvers: {}", worldgen::to_string(attached.error()));
        return nullptr;
    }

    stack->pipeline.emplace(*stack->generator, &*stack->decorator, blocks, stack->shape, seed);
    (void)quiet;
    return stack;
}

}  // namespace

GeneratedWorld::GeneratedWorld(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GeneratedWorld::~GeneratedWorld() = default;

std::unique_ptr<GeneratedWorld> GeneratedWorld::load(
    const std::filesystem::path& data_root, const registry::BlockRegistry& blocks,
    const registry::Registries& registries, std::span<const std::string_view> codec_biomes,
    i64 seed, usize stacks, std::string_view settings) {
    const auto data    = data_root / "vanilla" / "1.20.1" / "generated" / "data" / "minecraft";
    const auto reports = data_root / "vanilla" / "1.20.1" / "generated";

    auto impl  = std::make_unique<Impl>();
    impl->seed = seed;

    const usize wanted = stacks == 0 ? usize{1} : stacks;
    impl->stacks.reserve(wanted);
    for (usize index = 0; index < wanted; ++index) {
        auto stack = build_stack(data, reports, blocks, registries, seed, index != 0, settings);
        if (!stack) {
            return nullptr;
        }
        impl->stacks.push_back(std::move(stack));
    }

    // ── structures ── Loaded once, a stage per stack. Without the jar the
    // world still generates, and says loudly that it carries no structures.
    impl->settings = std::string{settings};
    if (const char* off = std::getenv("OV_STRUCTURES"); off != nullptr && std::string_view{off} == "0") {
        OV_LOG_WARN("worldgen: OV_STRUCTURES=0; {} places no structure — an instrument", settings);
    } else {
        impl->structures = WorldStructures::load(data, WorldStructures::default_jar(data_root),
                                                 blocks, impl->stacks.front()->biomes.biomes());
        if (!impl->structures) {
            OV_LOG_ERROR("worldgen: {} will carry no structures (see above)", settings);
        }
        // ── great pyramid ── Our own structures are part of the generator.
        // `OV_ORIGINAL_STRUCTURES=0` is the parity instruments' switch, not a
        // player option: a world measured against the game must be vanilla.
        worldgen::OriginalStructures originals{};
        if (const char* flag = std::getenv("OV_ORIGINAL_STRUCTURES");
            flag != nullptr && std::string_view{flag} == "0") {
            originals = worldgen::OriginalStructures::vanilla_parity();
            OV_LOG_WARN("worldgen: OV_ORIGINAL_STRUCTURES=0; {} carries no {} — a parity instrument",
                        settings, worldgen::kGreatPyramidId);
        }
        for (auto& stack : impl->stacks) {
            if (impl->structures) {
                stack->structures = impl->structures->make_stage(*stack->generator, blocks,
                                                                 registries, seed, originals);
                stack->pipeline->set_structure_stage(stack->structures->stage.get());
            }
        }
    }

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

    OV_LOG_INFO(
        "worldgen: {} ready at seed {} — noise, biomes, surface, carvers, features; "
        "{} independent stacks",
        settings, seed, impl->stacks.size());
    return std::unique_ptr<GeneratedWorld>(new GeneratedWorld(std::move(impl)));
}

usize GeneratedWorld::stack_count() const noexcept { return impl_->stacks.size(); }

void GeneratedWorld::to_codec_biomes(world::Chunk& chunk) const {
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
}

world::Chunk GeneratedWorld::generate(i32 chunk_x, i32 chunk_z) {
    Stack& stack = *impl_->stacks.front();

    // `take` rather than a copy: `Full` means all eight neighbours have run
    // their features, so nothing can write into this chunk again and the
    // pipeline has no reason to keep it. The server's own cache owns it now.
    world::Chunk chunk = stack.pipeline->take(chunk_x, chunk_z);
    if (stack.structures) {  // ── structures ──
        stack.structures->record(chunk);
    }
    to_codec_biomes(chunk);

    // Bound what the pipeline holds. Eight chunks is more than the two the
    // feature stage's radius needs, so an ordinary streaming pattern — chunk
    // after neighbouring chunk — keeps its support and pays nothing; a jump
    // across the world drops a cache that was about to be useless anyway.
    constexpr i32 kKeep = 8;
    stack.pipeline->trim(chunk_x, chunk_z, kKeep);
    if (stack.structures) {  // ── structures ──
        stack.structures->stage->trim(chunk_x, chunk_z, kKeep + worldgen::StructureStage::kReach);
        stack.structures->report(impl_->settings);
    }

    return chunk;
}

void GeneratedWorld::generate_square(usize stack_index, i32 origin_x, i32 origin_z, i32 side,
                                     std::vector<std::pair<ChunkPos, world::Chunk>>& out,
                                     std::vector<BlockPos>* fluid_wakeups) {
    Stack& stack = *impl_->stacks[stack_index];

    // The cold start is the determinism. `ChunkPipeline::promote` decorates a
    // chunk's eight neighbours on the way to `Full` and skips any that have
    // already been decorated, so what a chunk ends up containing depends on
    // what the pipeline was asked for before. Clearing here — and again at the
    // end — makes this square a pure function of the seed and its origin, which
    // is what lets N workers produce the world one worker would have.
    stack.pipeline->clear();
    if (stack.structures) {  // ── structures ── cold, like the pipeline
        stack.structures->stage->clear();
    }

    // Two passes, and the order of the first one is part of the contract.
    // Promoting every chunk before taking any means no chunk is ever removed
    // and then rebuilt as somebody else's neighbour, which would decorate it a
    // second time into a fresh copy.
    for (i32 dz = 0; dz < side; ++dz) {
        for (i32 dx = 0; dx < side; ++dx) {
            (void)stack.pipeline->promote(origin_x + dx, origin_z + dz, worldgen::ChunkStatus::Full);
        }
    }
    for (i32 dz = 0; dz < side; ++dz) {
        for (i32 dx = 0; dx < side; ++dx) {
            const i32    x     = origin_x + dx;
            const i32    z     = origin_z + dz;
            world::Chunk chunk = stack.pipeline->take(x, z, fluid_wakeups);  // ── worldgen-3 ──
            if (stack.structures) {  // ── structures ──
                stack.structures->record(chunk);
            }
            to_codec_biomes(chunk);
            out.emplace_back(ChunkPos{x, z}, std::move(chunk));
        }
    }

    stack.pipeline->clear();
    if (stack.structures) {  // ── structures ──
        stack.structures->report(impl_->settings);
        stack.structures->stage->clear();
    }
}

i64 GeneratedWorld::seed() const noexcept { return impl_->seed; }

}  // namespace ov::server
