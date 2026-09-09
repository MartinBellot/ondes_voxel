// ov-lab — writes the test world.
//
// A bench you cannot regenerate is a bench that drifts. This writes the same
// Anvil world every time from the catalogue in plots.cpp, so a measurement
// taken in it can be repeated a month later against the same blocks — and so
// that fixing a plot is a diff rather than an afternoon in creative mode.
//
// The world is a real save: our server opens it, and so does Minecraft 1.20.1.
// That is deliberate. A bench only our own code can read would let a
// disagreement hide.
#include "canvas.hpp"
#include "plots.hpp"

#include "ov/base/log.hpp"
#include "ov/io/file.hpp"
#include "ov/nbt/region_writer.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/world/level_dat.hpp"

#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::filesystem::path out{"run/lab"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    /// Print the catalogue and build nothing.
    bool list{false};
    /// Overwrite an existing world. Off by default: the lab is meant to be
    /// persistent, and someone will have left a probe rig in it.
    bool force{false};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument.starts_with("--out=")) {
            options.out = argument.substr(6);
        } else if (argument.starts_with("--pack=")) {
            options.pack = argument.substr(7);
        } else if (argument == "--list") {
            options.list = true;
        } else if (argument == "--force") {
            options.force = true;
        } else if (argument == "--help" || argument == "-h") {
            fmt::print(
                "ov-lab — write the Ondes VOXEL test world\n\n"
                "  --out=DIR     where to write it (default run/lab)\n"
                "  --pack=FILE   the block registry (default data/vanilla/1.20.1/registry.ovpack)\n"
                "  --list        print the catalogue and build nothing\n"
                "  --force       overwrite an existing world\n");
            std::exit(0);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ov;

    const Options options = parse(argc, argv);

    if (options.list) {
        fmt::print("{:<10} {:<20} {:>6} {:>6}  {}\n", "zone", "plot", "x", "z", "purpose");
        for (const lab::Plot& plot : lab::catalogue()) {
            fmt::print("{:<10} {:<20} {:>6} {:>6}  {}\n", plot.zone, plot.name, plot.x, plot.z,
                       plot.purpose);
        }
        return 0;
    }

    const std::filesystem::path region_dir = options.out / "region";
    if (std::filesystem::exists(options.out) && !options.force) {
        OV_LOG_ERROR("{} already exists — pass --force to overwrite it",
                     options.out.string());
        return 1;
    }

    auto blocks = registry::BlockRegistry::load(options.pack);
    if (!blocks) {
        OV_LOG_ERROR("could not read {}: {}. Run tools/ov_datagen first.", options.pack.string(),
                     registry::to_string(blocks.error()));
        return 1;
    }
    auto registries = registry::Registries::load(options.pack);

    lab::Canvas canvas{*blocks, registries ? &*registries : nullptr};
    lab::build_world(canvas);

    if (!canvas.unknown().empty()) {
        // Named, not swallowed: a plot built from a block this version does not
        // have is a silent hole in the bench, and every measurement taken in it
        // would inherit the hole without knowing.
        OV_LOG_ERROR("{} name(s) did not resolve:", canvas.unknown().size());
        for (const std::string& name : canvas.unknown()) {
            OV_LOG_ERROR("  {}", name);
        }
        return 1;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(region_dir, directory_error);
    if (directory_error) {
        OV_LOG_ERROR("could not create {}: {}", region_dir.string(), directory_error.message());
        return 1;
    }

    // Biome ids are ours to choose here because the disk format is name-based:
    // the canvas writes id 0 everywhere and this says what 0 is called.
    const std::vector<std::string_view> biome_names{"minecraft:plains"};
    const world::ChunkCodecContext      context{&*blocks, biome_names,
                                                registries ? &*registries : nullptr,
                                                world::AirStates::from(*blocks)};

    std::map<std::pair<i32, i32>, nbt::RegionWriter> writers;
    for (const auto& [position, chunk] : canvas.chunks()) {
        const auto [chunk_x, chunk_z] = position;
        const std::pair region{chunk_x >> 5, chunk_z >> 5};
        if (!writers.contains(region)) {
            const auto path =
                region_dir / fmt::format("r.{}.{}.mca", region.first, region.second);
            writers.emplace(region, nbt::RegionWriter::open_or_empty(path));
        }
        writers.at(region).set_chunk(static_cast<u32>(chunk_x & 31), static_cast<u32>(chunk_z & 31),
                                     world::to_nbt(chunk, context), 0);
    }

    for (const auto& [region, writer] : writers) {
        const auto path = region_dir / fmt::format("r.{}.{}.mca", region.first, region.second);
        if (!writer.write(path)) {
            OV_LOG_ERROR("could not write {}", path.string());
            return 1;
        }
    }

    // The generator matters as much as the blocks: a world whose regions were
    // made flat but whose level.dat says "noise" grows normal terrain the
    // moment someone walks past the last saved chunk.
    world::LevelSettings settings;
    settings.name    = "Ondes VOXEL — banc de test";
    settings.spawn_x = -8;
    settings.spawn_y = lab::Canvas::kGroundY + 1;
    settings.spawn_z = -8;
    settings.layers  = {
        {"minecraft:bedrock", 1},
        {"minecraft:dirt", 2},
        {"minecraft:grass_block", 1},
    };
    if (!io::write_file_atomic(options.out / "level.dat", world::encode_level_dat(settings))) {
        OV_LOG_ERROR("could not write level.dat");
        return 1;
    }

    OV_LOG_INFO("wrote {} chunks across {} regions, {} blocks, {} plots", canvas.chunks().size(),
                writers.size(), canvas.blocks_written(), lab::catalogue().size());
    OV_LOG_INFO("spawn at {},{},{} — serve it with: ov_dedicated --world={}", settings.spawn_x,
                settings.spawn_y, settings.spawn_z, options.out.string());
    return 0;
}
