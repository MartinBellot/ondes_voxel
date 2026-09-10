// Structure parity: where the game started a structure, and where we would.
//
// Of everything in the world generator this is the one question with a *clean*
// oracle. A chunk the game has finished carries, in its NBT,
//
//     structures: { starts: { "minecraft:mineshaft": {…}, … }, References: {…} }
//
// so the truth is a boolean per chunk and per structure type, written by the
// game itself, over thousands of chunks. There is no threshold, no tolerance
// and no percentage of a noisy field: a chunk either starts a mineshaft or it
// does not, and either we agree or we do not.
//
// The two ways of being wrong are counted apart, because they are fixed in
// completely different places:
//
//   * **false positive** — we place, the game did not. Almost always the biome
//     filter, or a structure whose own generation refused after the placement
//     said yes (an ocean monument that found no deep water).
//   * **false negative** — the game placed, we did not. That is the grid or the
//     salt, and it is the serious one: a false negative means the arithmetic is
//     wrong, and no amount of geometry work afterwards can recover it.
//
// Two cautions the numbers here depend on.
//
// Trap 4 of the project's list: only `Status == minecraft:full` chunks may be
// read, because an unfinished chunk has not decided its structures and reads as
// "none" — which is indistinguishable from a real disagreement and would
// manufacture thousands of false negatives.
//
// And the reference worlds are force-loaded in patches, so a rim of chunks
// around each patch reached `full` by being a neighbour rather than by being
// asked for. `--patches-only` restricts the comparison to the interior of the
// force-loaded squares; the difference between the two numbers is reported
// rather than hidden, because it is one chunk out of 5 092 and pretending it
// does not exist would be worse than naming it.
#define OV_LOG_CATEGORY "structparity"

#include "ov/base/log.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};

    /// Cap on chunks read. The whole reference world is about five thousand
    /// full chunks and reading all of them takes seconds, so the default is no
    /// cap at all — a placement oracle is only as sharp as the number of
    /// chunks it covers.
    i32 chunks{1000000};

    /// Apply the biome filter. Off measures the *grid* alone, which is the part
    /// that is claimed to be exact; on adds our noise biomes, which are
    /// themselves only 100 % on the sampled comparison and so can only make the
    /// number worse for a reason that is not the placement's.
    bool biomes{true};

    /// Restrict to the interior of the force-loaded patches.
    bool patches_only{false};

    /// Print the first few disagreements per structure.
    i32 show{4};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options     options;
    const auto  value = [&](std::string_view flag, std::string_view argument) {
        return std::string{argument.substr(flag.size())};
    };
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument.starts_with("--world=")) {
            options.world = value("--world=", argument);
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=", argument);
        } else if (argument.starts_with("--reports=")) {
            options.reports = value("--reports=", argument);
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=", argument);
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=", argument).c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=", argument).c_str());
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=", argument).c_str());
        } else if (argument == "--no-biomes") {
            options.biomes = false;
        } else if (argument == "--patches-only") {
            options.patches_only = true;
        }
    }
    return options;
}

/// The eight force-loaded squares of `scripts/reference_world.sh`, in chunks.
///
/// Hard-coded and not read from anywhere, because the script hard-codes them
/// too and the only alternative would be to parse a shell variable. If the
/// script's patch list changes this list has to follow, and the symptom would
/// be `--patches-only` covering fewer chunks rather than a wrong answer.
struct Patch {
    i32 x;
    i32 z;
};

constexpr Patch kPatches[] = {
    {0, 0},          {48000, 0},      {-37000, 15000}, {22000, -41000}, {-19000, -28000},
    {61000, 33000},  {-55000, -51000},{8000, 72000},   {-70000, 4000},  {35000, 58000},
    {-12000, -64000},{44000, 44000},  {-44000, 44000}, {67000, -17000}, {-26000, 39000},
    {15000, 26000},  {-83000, 29000}, {52000, -68000}, {-31000, 77000}, {90000, 21000},
    {-95000, -38000},{73000, 63000},  {-63000, -83000},{27000, 95000},  {-105000, 7000},
    {41000, -95000}, {-17000, -105000},{110000, -45000},
};

[[nodiscard]] bool inside_patch(i32 chunk_x, i32 chunk_z) {
    for (const Patch& patch : kPatches) {
        const i32 x0 = floor_div(patch.x, 16);
        const i32 x1 = floor_div(patch.x + 128, 16);
        const i32 z0 = floor_div(patch.z, 16);
        const i32 z1 = floor_div(patch.z + 128, 16);
        if (chunk_x >= x0 && chunk_x <= x1 && chunk_z >= z0 && chunk_z <= z1) {
            return true;
        }
    }
    return false;
}

/// The world as the structure placer needs to see it: a biome at a point, and
/// two column heights.
///
/// Built on our own noise rather than on the reference chunk's stored biome
/// array on purpose. Reading the game's biomes back would measure the placement
/// with the biome question already answered — an oracle grading its own
/// homework — and the point of the harness is to know what *our* generator
/// would do with a fresh world at this seed.
class NoiseSampler final : public worldgen::StructureWorldSampler {
public:
    NoiseSampler(const worldgen::ChunkGenerator& generator, const worldgen::BiomeSource& biomes,
                 const worldgen::NoiseRouter& router, i32 min_y, i32 max_y)
        : generator_{&generator}, biomes_{&biomes}, router_{&router}, min_y_{min_y},
          max_y_{max_y} {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        const auto climate = biomes_->sample(*router_, floor_div(x, 4), floor_div(y, 4),
                                             floor_div(z, 4));
        // The uncached lookup. The cache decides ties by the order the
        // questions were asked, and a harness that walks chunks in region order
        // asks them in an order the game never used — trap: a cache is not an
        // optimisation here, it is part of the answer.
        return biomes_->biome_at(climate);
    }

    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        // The first free y from the top. Our noise stage fills every non-solid
        // cell below the sea level with water, so "free" and "not solid" are
        // the same question at this stage — which is exactly why the ocean
        // floor below needs a different rule.
        for (i32 y = max_y_ - 1; y >= min_y_; --y) {
            if (generator_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return min_y_;
    }

    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        // Identical at this stage: the noise says solid or not, and water is
        // not solid. The two are kept apart anyway because the day an aquifer
        // lands they stop being the same, and a single function would then be
        // wrong for one of the two callers with nothing to show it.
        return surface_height(x, z);
    }

private:
    const worldgen::ChunkGenerator* generator_;
    const worldgen::BiomeSource*    biomes_;
    const worldgen::NoiseRouter*    router_;
    i32                             min_y_;
    i32                             max_y_;
};

/// One structure type's tally.
struct Tally {
    i32 agree_present{0};
    i32 agree_absent{0};
    i32 false_positive{0};
    i32 false_negative{0};

    std::vector<ChunkPos> false_positive_at;
    std::vector<ChunkPos> false_negative_at;

    /// Why we said no where the game said yes, and vice versa.
    std::map<std::string, i32> false_positive_reason;
};

[[nodiscard]] std::string strip(std::string_view name) {
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? std::string{name}
                                           : std::string{name.substr(colon + 1)};
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);

    if (!std::filesystem::is_directory(options.world / "region")) {
        OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_world.sh.",
                     options.world.string());
        return 1;
    }

    auto sets = worldgen::StructureSetRegistry::load(options.data);
    if (!sets) {
        OV_LOG_ERROR("structure sets: {}", worldgen::to_string(sets.error()));
        return 1;
    }
    auto placer = worldgen::StructurePlacer::load(options.data, *sets);
    if (!placer) {
        OV_LOG_ERROR("structures: {}", worldgen::to_string(placer.error()));
        return 1;
    }

    std::optional<registry::BlockRegistry>  blocks;
    std::optional<worldgen::NoiseRouter>    router;
    std::optional<worldgen::BiomeSource>    biomes;
    std::optional<worldgen::ChunkGenerator> generator;
    std::optional<NoiseSampler>             sampler;

    if (options.biomes) {
        auto loaded_blocks = registry::BlockRegistry::load(options.pack);
        if (!loaded_blocks) {
            OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
            return 1;
        }
        blocks = std::move(*loaded_blocks);
        auto loaded_router = worldgen::NoiseRouter::load(options.data, "overworld", options.seed);
        if (!loaded_router) {
            OV_LOG_ERROR("router: {}", worldgen::to_string(loaded_router.error()));
            return 1;
        }
        router = std::move(*loaded_router);
        auto loaded_biomes = worldgen::BiomeSource::load(options.reports, "overworld");
        if (!loaded_biomes) {
            OV_LOG_ERROR("biome table: {}", worldgen::to_string(loaded_biomes.error()));
            return 1;
        }
        biomes = std::move(*loaded_biomes);
        generator.emplace(*router, *biomes, *blocks);
        sampler.emplace(*generator, *biomes, *router, -64, 320);

        // Only the sets this dimension can produce. Without this the overworld
        // "places" nether fossils in a quarter of its chunks, because their set
        // has spacing 2 and nothing else would ever say no.
        placer->restrict_to_biomes(biomes->biomes());
    }

    const worldgen::StructureWorldSampler* world =
        sampler ? &sampler.value() : static_cast<const worldgen::StructureWorldSampler*>(nullptr);

    // Every structure the pack has, so that a type that never appears in either
    // world still shows up as a row of zeroes rather than vanishing.
    std::map<std::string, Tally> tallies;
    for (const worldgen::StructureDefinition& definition : placer->structures()) {
        tallies.emplace(definition.name, Tally{});
    }

    i32 chunks_read = 0;
    i32 chunks_skipped_status = 0;
    i32 chunks_skipped_patch  = 0;

    std::vector<std::filesystem::path> regions;
    for (const auto& entry : std::filesystem::directory_iterator(options.world / "region")) {
        if (entry.is_regular_file() && entry.path().extension() == ".mca") {
            regions.push_back(entry.path());
        }
    }
    std::sort(regions.begin(), regions.end());

    for (const auto& path : regions) {
        if (chunks_read >= options.chunks) {
            break;
        }
        // `r.<x>.<z>.mca`.
        const std::string stem = path.stem().string();
        i32               region_x = 0;
        i32               region_z = 0;
        if (std::sscanf(stem.c_str(), "r.%d.%d", &region_x, &region_z) != 2) {
            continue;
        }
        auto region = nbt::RegionFile::open(path);
        if (!region) {
            continue;
        }
        for (u32 local_z = 0; local_z < 32 && chunks_read < options.chunks; ++local_z) {
            for (u32 local_x = 0; local_x < 32 && chunks_read < options.chunks; ++local_x) {
                if (!region->has_chunk(local_x, local_z)) {
                    continue;
                }
                auto document = region->read_chunk(local_x, local_z);
                if (!document) {
                    continue;
                }
                const nbt::Tag& root = document->root;

                const nbt::Tag* status = root.find("Status");
                if (status == nullptr || status->as_string() != "minecraft:full") {
                    ++chunks_skipped_status;
                    continue;
                }
                const i32 chunk_x = region_x * 32 + static_cast<i32>(local_x);
                const i32 chunk_z = region_z * 32 + static_cast<i32>(local_z);
                if (options.patches_only && !inside_patch(chunk_x, chunk_z)) {
                    ++chunks_skipped_patch;
                    continue;
                }

                // What the game says.
                std::set<std::string> theirs;
                if (const nbt::Tag* structures = root.find("structures")) {
                    if (const nbt::Tag* starts = structures->find("starts")) {
                        if (const auto* entries = starts->compound()) {
                            for (const nbt::CompoundEntry& entry : *entries) {
                                // A start the game considered and rejected is
                                // written as `id: INVALID`. Counting one as a
                                // start would invent structures on both sides.
                                const nbt::Tag* id = entry.value.find("id");
                                if (id != nullptr && id->as_string() == "INVALID") {
                                    continue;
                                }
                                theirs.insert(entry.name);
                            }
                        }
                    }
                }

                // What we say.
                std::map<std::string, std::string> ours;  // structure -> reason
                for (const auto& result :
                     placer->decide(options.seed, chunk_x, chunk_z, world)) {
                    if (result.structure.empty()) {
                        continue;
                    }
                    ours.emplace(std::string{result.structure},
                                 std::string{worldgen::to_string(result.decision)});
                }

                for (auto& [name, tally] : tallies) {
                    const bool they = theirs.contains(name);
                    const auto entry = ours.find(name);
                    // With the biome filter off the placer stops at
                    // `biome-unknown`, which is the honest verdict for a
                    // sampler that was never given: counting it as a placement
                    // here is what makes `--no-biomes` measure the grid alone.
                    const bool we =
                        entry != ours.end() &&
                        (entry->second == "placed" ||
                         (!options.biomes && entry->second == "biome-unknown"));
                    if (they && we) {
                        ++tally.agree_present;
                    } else if (!they && !we) {
                        ++tally.agree_absent;
                    } else if (we) {
                        ++tally.false_positive;
                        if (tally.false_positive_at.size() < static_cast<usize>(options.show)) {
                            tally.false_positive_at.push_back({chunk_x, chunk_z});
                        }
                    } else {
                        ++tally.false_negative;
                        if (tally.false_negative_at.size() < static_cast<usize>(options.show)) {
                            tally.false_negative_at.push_back({chunk_x, chunk_z});
                        }
                        if (entry != ours.end()) {
                            ++tally.false_positive_reason[entry->second];
                        } else {
                            ++tally.false_positive_reason["not-chosen"];
                        }
                    }
                }
                ++chunks_read;
            }
        }
    }

    fmt::print("world {}  seed {}\n", options.world.string(), options.seed);
    fmt::print("chunks compared {}  (skipped: {} not full, {} outside patches)\n", chunks_read,
               chunks_skipped_status, chunks_skipped_patch);
    fmt::print("biome filter: {}\n\n", options.biomes ? "on" : "off (grid only)");

    fmt::print("{:<28} {:>6} {:>6} {:>6} {:>6}\n", "structure", "theirs", "ours", "FP", "FN");
    i32 total_theirs = 0;
    i32 total_ours   = 0;
    i32 total_fp     = 0;
    i32 total_fn     = 0;
    for (const auto& [name, tally] : tallies) {
        const i32 theirs = tally.agree_present + tally.false_negative;
        const i32 mine   = tally.agree_present + tally.false_positive;
        total_theirs += theirs;
        total_ours += mine;
        total_fp += tally.false_positive;
        total_fn += tally.false_negative;
        if (theirs == 0 && mine == 0) {
            continue;
        }
        fmt::print("{:<28} {:>6} {:>6} {:>6} {:>6}\n", strip(name), theirs, mine,
                   tally.false_positive, tally.false_negative);
    }
    fmt::print("{:<28} {:>6} {:>6} {:>6} {:>6}\n", "— total —", total_theirs, total_ours, total_fp,
               total_fn);

    // The recall is the number that matters most: a false negative is a wrong
    // grid, and no later work recovers it.
    if (total_theirs > 0) {
        fmt::print("\nrecall  {}/{} = {:.2f} %  (their starts we also place)\n",
                   total_theirs - total_fn, total_theirs,
                   100.0 * static_cast<f64>(total_theirs - total_fn) /
                       static_cast<f64>(total_theirs));
    }
    if (total_ours > 0) {
        fmt::print("precision {}/{} = {:.2f} %  (our starts they also place)\n",
                   total_ours - total_fp, total_ours,
                   100.0 * static_cast<f64>(total_ours - total_fp) / static_cast<f64>(total_ours));
    }

    if (options.show > 0) {
        fmt::print("\ndisagreements:\n");
        for (const auto& [name, tally] : tallies) {
            if (tally.false_negative == 0 && tally.false_positive == 0) {
                continue;
            }
            fmt::print("  {}\n", strip(name));
            for (const ChunkPos& at : tally.false_negative_at) {
                fmt::print("    theirs-not-ours  chunk {} {}\n", at.x, at.z);
            }
            for (const auto& [reason, count] : tally.false_positive_reason) {
                fmt::print("    theirs-not-ours  our reason {} x{}\n", reason, count);
            }
            for (const ChunkPos& at : tally.false_positive_at) {
                fmt::print("    ours-not-theirs  chunk {} {}\n", at.x, at.z);
            }
        }
    }

    return 0;
}
