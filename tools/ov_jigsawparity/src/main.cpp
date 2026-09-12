// Jigsaw parity: our pieces, grown from the seed, against the game's.
//
// Every chunk the game has taken to `structure_starts` stores the pieces of the
// jigsaw starts it owns, long before any block exists: element, position,
// rotation, box, ground delta, junctions. `StructureBuilder::generate` grows the
// same start from the seed and our noise; this compares the two piece by piece
// and, for the NBT, tag by tag (types included).
//
// `--control` grows every start from the seed plus one — the shifted witness a
// comparison must fail (trap 14 of the project): a harness that still agrees
// with the wrong seed measures nothing.
//
// The placement is asked too: does our placer, with the jigsaw anchor, start
// this structure in this chunk.
#define OV_LOG_CATEGORY "jigsawparity"

#include "ov/base/log.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/jigsaw.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_nbt.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    std::filesystem::path jar{"tools/vanilla/server.jar"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    i64                   seed{1234567890};
    std::string           dimension{"overworld"};
    std::string           only;
    bool                  control{false};
    /// Answer WORLD_SURFACE_WG through the aquifer rather than the global sea.
    bool                  aquifer_surface{false};
    i32                   show{4};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto             value = [&](std::string_view flag) {
            return std::string{argument.substr(flag.size())};
        };
        if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--jar=")) {
            options.jar = value("--jar=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--dimension=")) {
            options.dimension = value("--dimension=");
        } else if (argument.starts_with("--only=")) {
            options.only = value("--only=");
        } else if (argument == "--control") {
            options.control = true;
        } else if (argument == "--aquifer-surface") {
            options.aquifer_surface = true;
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        }
    }
    return options;
}

/// The generator as the structure layer sees it — the server's own sampler.
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
        if (use_aquifer_ && generator_->aquifer_active()) {
            // WORLD_SURFACE_WG through the aquifer: a dry pocket below the
            // sea level is air, a lake above it is water — the block the
            // noise stage would write, not the global sea.
            worldgen::AquiferSampler aquifer{*generator_->aquifer()};
            for (i32 y = high_; y >= low_; --y) {
                const auto answer = aquifer.compute(x, y, z, generator_->density_at(x, y, z));
                if (answer.substance != worldgen::Substance::Air) {
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

    void use_aquifer(bool on) noexcept { use_aquifer_ = on; }

    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        for (i32 y = high_; y >= low_; --y) {
            if (generator_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return low_;
    }

private:
    const worldgen::ChunkGenerator* generator_;
    i32                             low_;
    i32                             high_;
    bool                            use_aquifer_{false};
};

struct StoredStart {
    std::string name;
    i32         chunk_x{0};
    i32         chunk_z{0};
    bool        full{false};
    nbt::Tag    children;
};

[[nodiscard]] std::vector<StoredStart> read_starts(const std::filesystem::path& dir,
                                                   std::string_view             only) {
    std::vector<StoredStart>           out;
    std::vector<std::filesystem::path> regions;
    std::error_code                    error;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (entry.path().extension() == ".mca") {
            regions.push_back(entry.path());
        }
    }
    std::sort(regions.begin(), regions.end());
    for (const auto& path : regions) {
        i32 region_x = 0;
        i32 region_z = 0;
        if (std::sscanf(path.stem().string().c_str(), "r.%d.%d", &region_x, &region_z) != 2) {
            continue;
        }
        auto region = nbt::RegionFile::open(path);
        if (!region) {
            continue;
        }
        for (u32 local_z = 0; local_z < 32; ++local_z) {
            for (u32 local_x = 0; local_x < 32; ++local_x) {
                if (!region->has_chunk(local_x, local_z)) {
                    continue;
                }
                auto document = region->read_chunk(local_x, local_z);
                if (!document) {
                    continue;
                }
                const nbt::Tag* structures = document->root.find("structures");
                const nbt::Tag* starts =
                    structures != nullptr ? structures->find("starts") : nullptr;
                if (starts == nullptr || starts->compound() == nullptr) {
                    continue;
                }
                const nbt::Tag* status = document->root.find("Status");
                for (const nbt::CompoundEntry& entry : *starts->compound()) {
                    const nbt::Tag* children = entry.value.find("Children");
                    if (children == nullptr || children->list() == nullptr ||
                        children->list()->empty()) {
                        continue;
                    }
                    const nbt::Tag* id = children->list()->front().find("id");
                    if (id == nullptr || id->as_string() != "minecraft:jigsaw") {
                        continue;
                    }
                    if (!only.empty() && entry.name.find(only) == std::string::npos) {
                        continue;
                    }
                    out.push_back({entry.name, region_x * 32 + static_cast<i32>(local_x),
                                   region_z * 32 + static_cast<i32>(local_z),
                                   status != nullptr && status->as_string() == "minecraft:full",
                                   *children});
                }
            }
        }
    }
    return out;
}

/// Two tags equal in type and value, compounds regardless of key order.
[[nodiscard]] bool same_tag(const nbt::Tag& a, const nbt::Tag& b, std::string& where) {
    if (a.type() != b.type()) {
        where += fmt::format(" type {} vs {}", nbt::to_string(a.type()), nbt::to_string(b.type()));
        return false;
    }
    if (const auto* entries = a.compound()) {
        if (entries->size() != b.compound()->size()) {
            where += " key count";
            return false;
        }
        for (const nbt::CompoundEntry& entry : *entries) {
            const nbt::Tag* other = b.find(entry.name);
            if (other == nullptr) {
                where += " missing " + entry.name;
                return false;
            }
            if (!same_tag(entry.value, *other, where)) {
                where = "." + entry.name + where;
                return false;
            }
        }
        return true;
    }
    if (const auto* items = a.list()) {
        const auto* others = b.list();
        if (items->size() != others->size()) {
            where += fmt::format(" list size {} vs {}", items->size(), others->size());
            return false;
        }
        if (!items->empty() && a.list_element_type() != b.list_element_type()) {
            where += " list element type";
            return false;
        }
        for (usize index = 0; index < items->size(); ++index) {
            if (!same_tag((*items)[index], (*others)[index], where)) {
                where = fmt::format("[{}]", index) + where;
                return false;
            }
        }
        return true;
    }
    if (a.type() == nbt::TagType::String) {
        if (a.as_string() != b.as_string()) {
            where += fmt::format(" '{}' vs '{}'", a.as_string(), b.as_string());
            return false;
        }
        return true;
    }
    if (const auto* ints = a.get_if<nbt::Tag::IntArray>()) {
        return *ints == *b.get_if<nbt::Tag::IntArray>();
    }
    if (a.type() == nbt::TagType::Float || a.type() == nbt::TagType::Double) {
        return a.as_f64() == b.as_f64();
    }
    if (a.as_i64() != b.as_i64()) {
        where += fmt::format(" {} vs {}", a.as_i64(), b.as_i64());
        return false;
    }
    return true;
}

[[nodiscard]] std::string describe(const worldgen::StructurePiece& piece) {
    const std::string element =
        piece.element == nullptr ? std::string{"?"}
        : !piece.element->location.empty() ? piece.element->location
        : !piece.element->feature.empty()  ? "feature " + piece.element->feature
        : piece.element->type == worldgen::PoolElementType::List
            ? "list " + piece.element->elements.front().location
            : std::string{to_string(piece.element->type)};
    return fmt::format("{} at {},{},{} {} box [{},{},{} {},{},{}] delta {} junctions {}", element,
                       piece.origin.x, piece.origin.y, piece.origin.z,
                       worldgen::to_string(piece.rotation), piece.box.min_x, piece.box.min_y,
                       piece.box.min_z, piece.box.max_x, piece.box.max_y, piece.box.max_z,
                       piece.ground_level_delta, piece.junctions.size());
}

[[nodiscard]] bool same_junctions(const std::vector<worldgen::JigsawJunction>& a,
                                  const std::vector<worldgen::JigsawJunction>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index].source_x != b[index].source_x ||
            a[index].source_ground_y != b[index].source_ground_y ||
            a[index].source_z != b[index].source_z || a[index].delta_y != b[index].delta_y ||
            a[index].dest_projection != b[index].dest_projection) {
            return false;
        }
    }
    return true;
}

struct Tally {
    i32 starts{0};
    i32 identical{0};
    i32 nbt_identical{0};
    i32 placed{0};
    i64 pieces_game{0};
    i64 pieces_same{0};
};

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    const bool    nether  = options.dimension == "nether" || options.dimension == "the_nether";
    const auto    region_dir = options.world / (nether ? "DIM-1/region" : "region");

    auto blocks = registry::BlockRegistry::load(options.pack);
    if (!blocks) {
        OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
        return 1;
    }
    const std::string dimension = nether ? "nether" : "overworld";
    auto router = worldgen::NoiseRouter::load(options.data, dimension, options.seed);
    auto source = worldgen::BiomeSource::load(options.reports, dimension);
    if (!router || !source) {
        OV_LOG_ERROR("noise router or biome source for {} did not load", dimension);
        return 1;
    }
    worldgen::ChunkGenerator generator{*router, *source, *blocks};
    GeneratorSampler         sampler{generator};
    sampler.use_aquifer(options.aquifer_surface);

    auto tags = worldgen::BlockTags::load(options.data, *blocks);
    auto sets = worldgen::StructureSetRegistry::load(options.data);
    if (!tags || !sets) {
        OV_LOG_ERROR("block tags or structure sets did not load");
        return 1;
    }
    auto placer = worldgen::StructurePlacer::load(options.data, *sets);
    if (!placer) {
        OV_LOG_ERROR("structures: {}", worldgen::to_string(placer.error()));
        return 1;
    }
    placer->restrict_to_biomes(source->biomes());
    std::string detail;
    auto builder = worldgen::StructureBuilder::load(options.jar, options.data, *blocks, *tags, &detail);
    if (!builder || builder->jigsaw() == nullptr) {
        OV_LOG_ERROR("templates: {}", detail);
        return 1;
    }
    placer->set_jigsaw(builder->jigsaw());
    const worldgen::JigsawLibrary& library = *builder->jigsaw();

    const auto stored = read_starts(region_dir, options.only);
    fmt::print("{} jigsaw starts stored in {}\n", stored.size(), region_dir.string());

    std::map<std::string, Tally> tallies;
    std::map<std::string, i32>   shown;
    for (const StoredStart& start : stored) {
        Tally& tally = tallies[start.name];
        ++tally.starts;
        const worldgen::StructureDefinition* definition = placer->find(start.name);
        if (definition == nullptr) {
            fmt::print("  {} ({}, {}): unknown structure\n", start.name, start.chunk_x, start.chunk_z);
            continue;
        }
        // The placement: does our placer start it here, with the jigsaw anchor.
        for (const auto& verdict : placer->decide(options.seed, start.chunk_x, start.chunk_z, &sampler)) {
            if (verdict.structure == start.name &&
                verdict.decision == worldgen::PlacementDecision::PlacedByPlacement) {
                ++tally.placed;
            }
        }

        const i64 seed  = options.control ? options.seed + 1 : options.seed;
        auto      grown = builder->generate(*definition, seed, start.chunk_x, start.chunk_z, &sampler);
        const auto& children = *start.children.list();
        tally.pieces_game += static_cast<i64>(children.size());
        if (!grown) {
            fmt::print("  {} ({}, {}): refused — {}\n", start.name, start.chunk_x, start.chunk_z,
                       grown.error());
            continue;
        }
        bool        same = grown->pieces.size() == children.size();
        std::string first;
        for (usize index = 0; index < children.size(); ++index) {
            auto game = worldgen::jigsaw_piece_from_nbt(library, children[index]);
            if (!game) {
                same = false;
                if (first.empty()) {
                    first = fmt::format("piece {}: {}", index, game.error());
                }
                continue;
            }
            if (index >= grown->pieces.size()) {
                same = false;
                if (first.empty()) {
                    first = fmt::format("piece {}: ours ended; game {}", index, describe(*game));
                }
                continue;
            }
            const worldgen::StructurePiece& ours = grown->pieces[index];
            const bool piece_same = ours.element == game->element && ours.origin == game->origin &&
                                    ours.rotation == game->rotation &&
                                    ours.box.min_x == game->box.min_x &&
                                    ours.box.min_y == game->box.min_y &&
                                    ours.box.min_z == game->box.min_z &&
                                    ours.box.max_x == game->box.max_x &&
                                    ours.box.max_y == game->box.max_y &&
                                    ours.box.max_z == game->box.max_z &&
                                    ours.ground_level_delta == game->ground_level_delta &&
                                    same_junctions(ours.junctions, game->junctions);
            if (piece_same) {
                ++tally.pieces_same;
            } else {
                same = false;
                if (first.empty()) {
                    first = fmt::format("piece {}:\n      game {}\n      ours {}", index,
                                        describe(*game), describe(ours));
                }
            }
        }
        if (same) {
            ++tally.identical;
        }
        // The NBT the server would write, against the game's, tag by tag.
        const nbt::Tag ours_nbt = worldgen::start_to_nbt(*grown);
        std::string    where;
        if (const nbt::Tag* ours_children = ours_nbt.find("Children");
            ours_children != nullptr && same_tag(*ours_children, start.children, where)) {
            ++tally.nbt_identical;
        }
        const bool print = !same && shown[start.name]++ < options.show;
        fmt::print("  {} {} ({}, {}) {} pieces {}/{}{}{}\n", same ? "OK " : "BAD", start.name,
                   start.chunk_x, start.chunk_z, start.full ? "full" : "partial",
                   grown->pieces.size(), children.size(),
                   where.empty() ? "" : " nbt:" + where.substr(0, 160),
                   print ? "\n    " + first : "");
    }

    Tally total;
    fmt::print("\n{:<30} {:>7} {:>10} {:>10} {:>9} {:>16}\n", "structure", "starts", "identical",
               "nbt", "placed", "pieces");
    for (const auto& [name, tally] : tallies) {
        fmt::print("{:<30} {:>7} {:>10} {:>10} {:>9} {:>9}/{:<6}\n", name, tally.starts,
                   tally.identical, tally.nbt_identical, tally.placed, tally.pieces_same,
                   tally.pieces_game);
        total.starts += tally.starts;
        total.identical += tally.identical;
        total.nbt_identical += tally.nbt_identical;
        total.placed += tally.placed;
        total.pieces_game += tally.pieces_game;
        total.pieces_same += tally.pieces_same;
    }
    fmt::print("{:<30} {:>7} {:>10} {:>10} {:>9} {:>9}/{:<6}{}\n", "total", total.starts,
               total.identical, total.nbt_identical, total.placed, total.pieces_same,
               total.pieces_game, options.control ? "  (control: seed + 1)" : "");
    return 0;
}
