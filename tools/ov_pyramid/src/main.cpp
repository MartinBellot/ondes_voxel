// ov_pyramid — measurements of the Great Pyramid, an original Ondes VOXEL
// structure (src/ov_worldgen/include/ov/worldgen/great_pyramid.hpp).
//
//   --mode=scan     seeds whose pyramid stands near the origin (for a visit)
//   --mode=density  starts per 10 000 chunks², and why the others were refused
//   --mode=cost     generation time per chunk, pyramid chunks against the same
//                   chunks with the switch off, and ordinary desert chunks
//   --mode=order    one area generated in two chunk orders, compared block by
//                   block (structures only, no features: the pyramid alone)
//
// Every number it prints comes from the real generator: our noise, our
// biomes, the placer's vanilla gates.
#define OV_LOG_CATEGORY "pyramid"

#include "ov/base/log.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/great_pyramid.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "ov/worldgen/structure_stage.hpp"
#include "ov/worldgen/structure_template.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path jar{"tools/vanilla/server.jar"};
    std::string           mode{"scan"};
    i64                   seed{1234567890};
    i64                   seeds{1};      // scan: this many seeds from `seed`
    i32                   radius{64};    // scan: chunks from the origin
    i32                   cells{12};     // density: grid cells per side
    i32                   side{3};       // cost: chunks per side around the centre
    bool                  features{false};
    i32                   spacing{0};     // density: another grid than the set's (0 = the set's)
    i32                   separation{0};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        std::string            rest;
        const auto             flag = [&](std::string_view name) {
            if (!arg.starts_with(name)) {
                return false;
            }
            rest = std::string{arg.substr(name.size())};
            return true;
        };
        if (flag("--mode=")) {
            options.mode = rest;
        } else if (flag("--seed=")) {
            options.seed = std::stoll(rest);
        } else if (flag("--seeds=")) {
            options.seeds = std::stoll(rest);
        } else if (flag("--radius=")) {
            options.radius = std::stoi(rest);
        } else if (flag("--cells=")) {
            options.cells = std::stoi(rest);
        } else if (flag("--side=")) {
            options.side = std::stoi(rest);
        } else if (flag("--spacing=")) {
            options.spacing = std::stoi(rest);
        } else if (flag("--separation=")) {
            options.separation = std::stoi(rest);
        } else if (arg == "--features") {
            options.features = true;
        } else if (flag("--data=")) {
            const std::filesystem::path root{rest};
            options.pack    = root / "vanilla/1.20.1/registry.ovpack";
            options.data    = root / "vanilla/1.20.1/generated/data/minecraft";
            options.reports = root / "vanilla/1.20.1/generated";
        } else {
            fmt::print(
                "usage: ov_pyramid [--mode=scan|density|cost|order] [--seed=N] [--seeds=N]\n"
                "                  [--radius=chunks] [--cells=N] [--side=N] [--features]\n");
            std::exit(arg == "--help" ? 0 : 2);
        }
    }
    return options;
}

/// The generator as the structure layer sees it — the server's rules
/// (world_structures.cpp), with the base column for the pyramid's heights.
class Sampler final : public worldgen::StructureWorldSampler {
public:
    explicit Sampler(const worldgen::ChunkGenerator& g)
        : g_(&g), low_(g.gen_min_y()), high_(g.gen_min_y() + g.gen_depth() - 1) {}
    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return g_->biome_name_at(x, y, z);
    }
    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        for (i32 y = high_; y >= low_; --y) {
            if (y < g_->sea_level() || g_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return low_;
    }
    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        for (i32 y = high_; y >= low_; --y) {
            if (g_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return low_;
    }
    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return g_->is_solid(x, y, z);
    }

private:
    const worldgen::ChunkGenerator* g_;
    i32                             low_;
    i32                             high_;
};

/// One seed's generator, placer and builder.
struct World {
    std::optional<worldgen::NoiseRouter>     router;
    std::optional<worldgen::BiomeSource>     biomes;
    std::optional<worldgen::SurfaceSystem>   surface;
    std::optional<worldgen::FeatureRegistry> features;
    std::optional<worldgen::Decorator>       decorator;
    std::optional<worldgen::CarvingContext>  carving;
    std::optional<worldgen::CarverStage>     carvers;
    std::optional<worldgen::ChunkGenerator>  generator;
    std::optional<Sampler>                   sampler;
};

[[nodiscard]] bool build(World& w, const Options& o, i64 seed, const registry::BlockRegistry& blocks,
                         const registry::Registries& registries, bool with_features) {
    auto router  = worldgen::NoiseRouter::load(o.data, "overworld", seed);
    auto biomes  = worldgen::BiomeSource::load(o.reports, "overworld");
    auto surface = worldgen::SurfaceSystem::load(o.data, "overworld", seed, blocks);
    if (!router || !biomes || !surface) {
        return false;
    }
    w.router.emplace(std::move(*router));
    w.biomes.emplace(std::move(*biomes));
    w.surface.emplace(std::move(*surface));
    if (with_features) {
        auto features = worldgen::FeatureRegistry::load(o.data, blocks);
        if (!features) {
            return false;
        }
        w.features.emplace(std::move(*features));
        auto decorator = worldgen::Decorator::load(o.data, blocks, *w.features, *w.biomes);
        if (!decorator) {
            return false;
        }
        w.decorator.emplace(std::move(*decorator));
    }
    w.generator.emplace(*w.router, *w.biomes, blocks);
    w.generator->set_surface_system(&*w.surface);
    w.carving.emplace(w.router->min_y(), w.router->height());
    w.carvers.emplace(seed, *w.carving);
    if (!w.generator->set_carvers(&*w.carvers, registries)) {
        return false;
    }
    w.sampler.emplace(*w.generator);
    return true;
}

[[nodiscard]] std::string_view facing_name(u8 facing) {
    constexpr std::array<std::string_view, 4> kNames{"north", "east", "south", "west"};
    return kNames[facing & 3U];
}

void print_layout(i64 seed, const worldgen::GreatPyramidLayout& l) {
    const BlockPos entrance = l.world(0, 0, -52);
    const BlockPos chamber  = l.world(0, 26, 8);
    fmt::print(
        "seed {} chunk ({}, {}) centre x {} z {} floor y {} entrance faces {} — "
        "entrance at ({}, {}, {}), Pharaoh's chamber ({}, {}, {}), {:.0f} blocks from 0,0\n",
        seed, l.chunk_x, l.chunk_z, l.centre_x, l.centre_z, l.base_y, facing_name(l.facing),
        entrance.x, entrance.y, entrance.z, chamber.x, chamber.y, chamber.z,
        std::hypot(static_cast<f64>(l.centre_x), static_cast<f64>(l.centre_z)));
}

struct Shared {
    std::optional<worldgen::StructureSetRegistry> sets;
    std::optional<worldgen::StructurePlacer>      placer;
    std::optional<worldgen::BlockTags>            tags;
    std::optional<worldgen::StructureBuilder>     builder;
};

[[nodiscard]] bool load_shared(Shared& s, const Options& o, const registry::BlockRegistry& blocks,
                               const worldgen::BiomeSource& biomes, bool with_builder) {
    auto sets = worldgen::StructureSetRegistry::load(o.data);
    if (!sets) {
        return false;
    }
    s.sets.emplace(std::move(*sets));
    auto placer = worldgen::StructurePlacer::load(o.data, *s.sets);
    if (!placer) {
        return false;
    }
    s.placer.emplace(std::move(*placer));
    s.placer->restrict_to_biomes(biomes.biomes());
    if (!with_builder) {
        return true;
    }
    auto tags = worldgen::BlockTags::load(o.data, blocks);
    if (!tags) {
        return false;
    }
    s.tags.emplace(std::move(*tags));
    std::string detail;
    auto        builder = worldgen::StructureBuilder::load(o.jar, o.data, blocks, *s.tags, &detail);
    if (!builder) {
        OV_LOG_ERROR("templates: {}", detail);
        return false;
    }
    s.builder.emplace(std::move(*builder));
    return true;
}

[[nodiscard]] i32 floor_div(i32 a, i32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int scan(const Options& o, const registry::BlockRegistry& blocks,
         const registry::Registries& registries) {
    const auto pyramid = worldgen::GreatPyramid::create(blocks);
    if (!pyramid) {
        return 1;
    }
    const auto grid  = worldgen::great_pyramid_placement();
    i32        found = 0;
    for (i64 seed = o.seed; seed < o.seed + o.seeds; ++seed) {
        World w;
        if (!build(w, o, seed, blocks, registries, false)) {
            return 1;
        }
        Shared s;
        if (!load_shared(s, o, blocks, *w.biomes, false)) {
            return 1;
        }
        const i32 lo = floor_div(-o.radius, grid.spacing);
        const i32 hi = floor_div(o.radius, grid.spacing);
        for (i32 gz = lo; gz <= hi; ++gz) {
            for (i32 gx = lo; gx <= hi; ++gx) {
                const ChunkPos c = grid.candidate(seed, gx, gz);
                if (std::abs(c.x) > o.radius || std::abs(c.z) > o.radius) {
                    continue;
                }
                worldgen::GreatPyramidLayout layout;
                const auto decision =
                    pyramid->decide(seed, c.x, c.z, &*w.sampler, &*s.placer, &layout);
                if (decision == worldgen::PyramidDecision::Placed) {
                    print_layout(seed, layout);
                    ++found;
                } else if (o.seeds == 1) {
                    const i32 x = c.x * 16 + 8;
                    const i32 z = c.z * 16 + 8;
                    fmt::print("seed {} chunk ({}, {}): {} — biome {} at y 64, surface {}\n", seed,
                               c.x, c.z, worldgen::to_string(decision),
                               w.sampler->biome_at(x, 64, z), w.sampler->surface_height(x, z));
                }
            }
        }
        std::fflush(stdout);
    }
    fmt::print("{} pyramids within {} chunks of the origin over {} seeds\n", found, o.radius,
               o.seeds);
    return 0;
}

int density(const Options& o, const registry::BlockRegistry& blocks,
            const registry::Registries& registries) {
    const auto pyramid = worldgen::GreatPyramid::create(blocks);
    World      w;
    Shared     s;
    if (!pyramid || !build(w, o, o.seed, blocks, registries, false) ||
        !load_shared(s, o, blocks, *w.biomes, false)) {
        return 1;
    }
    // The set's grid, or another one with the same salt, to compare spacings
    // over the same world.
    auto grid = worldgen::great_pyramid_placement();
    if (o.spacing > 0) {
        grid.spacing    = o.spacing;
        grid.separation = o.separation > 0 ? o.separation : o.spacing / 2;
    }
    std::map<worldgen::PyramidDecision, i32> counts;
    std::map<i32, i32>                       spreads;  // TooSteep, by spread
    std::vector<ChunkPos>                    placed_at;
    const i32  half  = o.cells / 2;
    const auto start = std::chrono::steady_clock::now();
    for (i32 gz = -half; gz < o.cells - half; ++gz) {
        for (i32 gx = -half; gx < o.cells - half; ++gx) {
            const ChunkPos               c = grid.candidate(o.seed, gx, gz);
            worldgen::GreatPyramidLayout layout;
            const auto decision =
                pyramid->decide_site(o.seed, c.x, c.z, &*w.sampler, &*s.placer, &layout);
            ++counts[decision];
            if (decision == worldgen::PyramidDecision::Placed) {
                placed_at.push_back(c);
            } else if (decision == worldgen::PyramidDecision::TooSteep) {
                const auto [lo, hi] = std::minmax_element(layout.samples.begin(), layout.samples.end());
                ++spreads[*hi - *lo];
            }
        }
    }
    const f64 seconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();

    // The desert's share of the same area: the biome at every fourth chunk's
    // middle, at y 64.
    const i32 x0 = -half * grid.spacing;
    const i32 z0 = -half * grid.spacing;
    const i32 side_chunks = o.cells * grid.spacing;
    i64       sampled     = 0;
    i64       desert      = 0;
    for (i32 z = 0; z < side_chunks; z += 4) {
        for (i32 x = 0; x < side_chunks; x += 4) {
            ++sampled;
            desert += w.sampler->biome_at((x0 + x) * 16 + 8, 64, (z0 + z) * 16 + 8) ==
                              "minecraft:desert"
                          ? 1
                          : 0;
        }
    }
    // Closest pair of placed starts, in chunks.
    f64 closest = 0.0;
    for (usize i = 0; i < placed_at.size(); ++i) {
        for (usize j = i + 1; j < placed_at.size(); ++j) {
            const f64 d = std::hypot(static_cast<f64>(placed_at[i].x - placed_at[j].x),
                                     static_cast<f64>(placed_at[i].z - placed_at[j].z));
            closest = closest == 0.0 ? d : std::min(closest, d);
        }
    }
    const i64 cells       = static_cast<i64>(o.cells) * o.cells;
    const f64 area        = static_cast<f64>(side_chunks) * side_chunks;
    const f64 desert_area = area * static_cast<f64>(desert) / static_cast<f64>(sampled);
    const i32 placed      = counts[worldgen::PyramidDecision::Placed];
    fmt::print("seed {} grid {}/{}: {} cells, {:.0f} chunks², desert {:.2f} % ({:.0f} chunks²), "
               "decided in {:.1f} s\n",
               o.seed, grid.spacing, grid.separation, cells, area,
               100.0 * static_cast<f64>(desert) / static_cast<f64>(sampled), desert_area, seconds);
    for (const auto& [decision, count] : counts) {
        fmt::print("  {:<14} {}\n", worldgen::to_string(decision), count);
    }
    std::string steep;
    for (const auto& [spread, count] : spreads) {
        steep += fmt::format(" {}:{}", spread, count);
    }
    fmt::print("  too_steep spreads (blocks:count):{}\n", steep);
    fmt::print("density: {} placed — {:.3f} per 10 000 chunks², {:.1f} per 10 000 desert chunks², "
               "closest pair {:.0f} chunks\n",
               placed, placed * 10000.0 / area,
               desert_area > 0 ? placed * 10000.0 / desert_area : 0.0, closest);
    return 0;
}

/// The first placed pyramid near the origin, for cost and order.
[[nodiscard]] std::optional<worldgen::GreatPyramidLayout> first_pyramid(
    const Options& o, const worldgen::GreatPyramid& pyramid, const World& w, const Shared& s) {
    const auto grid = worldgen::great_pyramid_placement();
    for (i32 ring = 0; ring < 20; ++ring) {
        for (i32 gz = -ring; gz <= ring; ++gz) {
            for (i32 gx = -ring; gx <= ring; ++gx) {
                if (std::max(std::abs(gx), std::abs(gz)) != ring) {
                    continue;
                }
                const ChunkPos               c = grid.candidate(o.seed, gx, gz);
                worldgen::GreatPyramidLayout layout;
                if (pyramid.decide(o.seed, c.x, c.z, &*w.sampler, &*s.placer, &layout) ==
                    worldgen::PyramidDecision::Placed) {
                    return layout;
                }
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] f64 percentile(std::vector<f64> v, f64 p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto index = static_cast<usize>(std::min<f64>(static_cast<f64>(v.size() - 1),
                                                        std::floor(p * static_cast<f64>(v.size()))));
    return v[index];
}

/// Promote `chunks` to Full one by one, timing each call (it includes the
/// neighbours it drags up), on a fresh pipeline.
[[nodiscard]] std::vector<f64> time_chunks(const World& w, const Shared& s,
                                           const registry::BlockRegistry& blocks,
                                           const registry::Registries& registries, i64 seed,
                                           const std::vector<ChunkPos>& chunks, bool pyramid_on,
                                           bool features, u64* pyramid_blocks = nullptr) {
    worldgen::ChunkPipeline  pipeline{*w.generator, features ? &*w.decorator : nullptr, blocks,
                                     world::WorldShape::overworld(), seed};
    worldgen::StructureStage stage{*s.placer,  *s.builder, &*w.sampler, blocks, &registries, seed,
                                   pyramid_on ? worldgen::OriginalStructures::all()
                                              : worldgen::OriginalStructures::vanilla_parity()};
    pipeline.set_structure_stage(&stage);
    std::vector<f64> out;
    for (const ChunkPos& c : chunks) {
        const auto start = std::chrono::steady_clock::now();
        (void)pipeline.promote(c.x, c.z, worldgen::ChunkStatus::Full);
        out.push_back(
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    if (pyramid_blocks != nullptr && stage.great_pyramid() != nullptr) {
        *pyramid_blocks = stage.great_pyramid()->stats().blocks_written;
    }
    return out;
}

int cost(const Options& o, const registry::BlockRegistry& blocks,
         const registry::Registries& registries) {
    const auto pyramid = worldgen::GreatPyramid::create(blocks);
    World      w;
    Shared     s;
    if (!pyramid || !build(w, o, o.seed, blocks, registries, o.features) ||
        !load_shared(s, o, blocks, *w.biomes, true)) {
        return 1;
    }
    const auto layout = first_pyramid(o, *pyramid, w, s);
    if (!layout) {
        fmt::print("no pyramid near the origin for seed {}\n", o.seed);
        return 1;
    }
    print_layout(o.seed, *layout);

    // The pyramid's own chunks, a square around its centre; and an ordinary
    // desert square far enough that no pyramid reaches it.
    std::vector<ChunkPos> here;
    std::vector<ChunkPos> desert;
    const i32             half = o.side / 2;
    for (i32 dz = -half; dz < o.side - half; ++dz) {
        for (i32 dx = -half; dx < o.side - half; ++dx) {
            here.push_back({layout->chunk_x + dx, layout->chunk_z + dz});
            desert.push_back({layout->chunk_x + 12 + dx, layout->chunk_z + dz});
        }
    }
    u64        written = 0;
    const auto on      = time_chunks(w, s, blocks, registries, o.seed, here, true, o.features, &written);
    const auto off     = time_chunks(w, s, blocks, registries, o.seed, here, false, o.features);
    const auto plain   = time_chunks(w, s, blocks, registries, o.seed, desert, true, o.features);
    const auto report  = [](std::string_view what, const std::vector<f64>& v) {
        f64 sum = 0.0;
        for (const f64 x : v) {
            sum += x;
        }
        fmt::print("  {:<34} n {:>3}  mean {:>8.1f} ms  p50 {:>8.1f}  p99 {:>8.1f}\n", what, v.size(),
                   v.empty() ? 0.0 : sum / static_cast<f64>(v.size()), percentile(v, 0.5),
                   percentile(v, 0.99));
    };
    fmt::print("promote to Full, per chunk ({} build, features {}):\n",
#ifdef NDEBUG
               "release",
#else
               "debug",
#endif
               o.features ? "on" : "off");
    report("pyramid chunks, switch on", on);
    report("same chunks, switch off", off);
    report("desert chunks 12 chunks away", plain);
    fmt::print("  pyramid blocks written: {}\n", written);
    return 0;
}

int order(const Options& o, const registry::BlockRegistry& blocks,
          const registry::Registries& registries) {
    const auto pyramid = worldgen::GreatPyramid::create(blocks);
    World      w;
    Shared     s;
    if (!pyramid || !build(w, o, o.seed, blocks, registries, o.features) ||
        !load_shared(s, o, blocks, *w.biomes, true)) {
        return 1;
    }
    const auto layout = first_pyramid(o, *pyramid, w, s);
    if (!layout) {
        return 1;
    }
    print_layout(o.seed, *layout);
    std::vector<ChunkPos> area;
    for (i32 z = layout->box.min_z >> 4; z <= layout->box.max_z >> 4; ++z) {
        for (i32 x = layout->box.min_x >> 4; x <= layout->box.max_x >> 4; ++x) {
            area.push_back({x, z});
        }
    }
    const auto run = [&](const std::vector<ChunkPos>& sequence) {
        worldgen::ChunkPipeline  pipeline{*w.generator, o.features ? &*w.decorator : nullptr,
                                         blocks, world::WorldShape::overworld(), o.seed};
        worldgen::StructureStage stage{*s.placer, *s.builder, &*w.sampler, blocks, &registries,
                                       o.seed};
        pipeline.set_structure_stage(&stage);
        for (const ChunkPos& c : sequence) {
            (void)pipeline.promote(c.x, c.z, worldgen::ChunkStatus::Full);
        }
        std::map<std::pair<i32, i32>, std::vector<registry::BlockStateId>> out;
        for (const ChunkPos& c : area) {
            const world::Chunk& chunk = pipeline.promote(c.x, c.z, worldgen::ChunkStatus::Full);
            auto&               cells = out[{c.x, c.z}];
            for (i32 y = layout->base_y - 64; y <= layout->base_y + 52; ++y) {
                for (usize z = 0; z < 16; ++z) {
                    for (usize x = 0; x < 16; ++x) {
                        cells.push_back(chunk.get_block(x, y, z));
                    }
                }
            }
        }
        return out;
    };
    std::vector<ChunkPos> backward = area;
    std::reverse(backward.begin(), backward.end());
    // Columns first, then rows: a third order, neither of the other two.
    std::vector<ChunkPos> columns = area;
    std::stable_sort(columns.begin(), columns.end(),
                     [](const ChunkPos& a, const ChunkPos& b) { return a.x < b.x; });
    const auto a = run(area);
    const auto b = run(backward);
    const auto c = run(columns);
    u64        compared = 0;
    u64        differ_b = 0;
    u64        differ_c = 0;
    for (const auto& [key, cells] : a) {
        const auto& other_b = b.at(key);
        const auto& other_c = c.at(key);
        for (usize i = 0; i < cells.size(); ++i) {
            ++compared;
            differ_b += cells[i] != other_b[i] ? 1 : 0;
            differ_c += cells[i] != other_c[i] ? 1 : 0;
        }
    }
    fmt::print("{} chunks, {} cells compared (features {}): reversed order {} differ, "
               "column order {} differ\n",
               area.size(), compared, o.features ? "on" : "off", differ_b, differ_c);
    return differ_b == 0 && differ_c == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o          = parse(argc, argv);
    auto          blocks     = registry::BlockRegistry::load(o.pack);
    auto          registries = registry::Registries::load(o.pack);
    if (!blocks || !registries) {
        fmt::print("cannot read {}\n", o.pack.string());
        return 1;
    }
    if (o.mode == "scan") {
        return scan(o, *blocks, *registries);
    }
    if (o.mode == "density") {
        return density(o, *blocks, *registries);
    }
    if (o.mode == "cost") {
        return cost(o, *blocks, *registries);
    }
    if (o.mode == "order") {
        return order(o, *blocks, *registries);
    }
    fmt::print("unknown mode {}\n", o.mode);
    return 2;
}
