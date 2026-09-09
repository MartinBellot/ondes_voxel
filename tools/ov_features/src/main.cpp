// Feature parity: do our ores land on the same blocks as the game's?
//
// The measurement takes the game's own terrain as given. It reads a chunk the
// real server generated, puts every ore block back to the stone it replaced,
// runs our decoration over the result, and compares what we wrote against what
// was there. That separates two failures which otherwise look identical: an
// ore in the wrong place, and an ore in the right place inside terrain that is
// the wrong shape. Our terrain has no deepslate yet — the surface rules are
// somebody else's work in progress — so measuring ores against our own stone
// would say nothing about the ores.
//
// It reports three things per ore, and all three are needed:
//
//   * the count per slice of y, ours and theirs. A count that matches at the
//     wrong altitude is a height provider read wrongly, and only the histogram
//     shows it;
//   * exact position agreement. If the decoration seeding is right the veins
//     are the same blocks, not merely the same number of blocks;
//   * what each side had that the other did not.
//
// Only chunks at status minecraft:full are read. A force-loaded region holds
// chunks that stopped earlier, and their ores are simply not there yet.
#define OV_LOG_CATEGORY "features"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/feature.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};
    /// How many chunks to compare. Each one needs its eight neighbours read as
    /// well, so this is not free.
    i32 chunks{64};
    /// Print the shared feature ordering at one step and stop.
    std::string order;
    /// List the placed features that could not be built, and stop.
    bool missing{false};
    /// Run one placed feature at every candidate index and report which index
    /// reproduces the game's blocks.
    ///
    /// The index a feature has at its step is what seeds it, and an index one
    /// out gives a world that is statistically perfect and shares no block with
    /// the real one. Sweeping it turns a guess about the ordering into a
    /// measurement, the way ov_parity's --sweep turns a guess about a climate
    /// axis into one.
    std::string calibrate;
    i32         step{6};
    /// How many indices to try. Wide by default: the index a feature has at
    /// its step is not obviously reset per step, and a sweep that stops at the
    /// number of features in one step would miss a running count.
    i32 span{48};
    /// Treat --span as a raw offset from the decoration seed rather than as a
    /// feature index.
    bool raw{false};
    /// Only count the game's ores whose name contains this, so the score is
    /// about the feature under test rather than about how much ore is around.
    std::string ore;
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        const auto        value = [&](std::string_view prefix) {
            return argument.substr(prefix.size());
        };
        if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=");
        } else if (argument.starts_with("--reports=")) {
            options.reports = value("--reports=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=").c_str());
        } else if (argument.starts_with("--order=")) {
            options.order = value("--order=");
        } else if (argument == "--missing") {
            options.missing = true;
        } else if (argument.starts_with("--calibrate=")) {
            options.calibrate = value("--calibrate=");
        } else if (argument.starts_with("--step=")) {
            options.step = std::atoi(value("--step=").c_str());
        } else if (argument.starts_with("--span=")) {
            options.span = std::atoi(value("--span=").c_str());
        } else if (argument == "--raw") {
            options.raw = true;
        } else if (argument.starts_with("--ore=")) {
            options.ore = value("--ore=");
        }
    }
    return options;
}

/// One chunk read out of a region file: its blocks and its biomes.
struct StoredChunk {
    i32 chunk_x{0};
    i32 chunk_z{0};
    /// Section states, indexed [section][y*256 + z*16 + x], bottom section
    /// first. Empty sections are empty vectors.
    std::vector<std::vector<registry::BlockStateId>> sections;
    /// Biome names per section, 64 cells each.
    std::vector<std::array<std::string, 64>> biomes;
    i32                                      min_section{-4};
};

/// The Anvil palette decoder.
///
/// Written here rather than reused from world::from_nbt because this must not
/// go through our own registry's idea of what a block is: it reads the names
/// the game wrote and resolves them itself, so that a block we do not know is
/// a loud failure instead of quiet air.
[[nodiscard]] bool decode_section(const nbt::Tag& section, const registry::BlockRegistry& blocks,
                                  std::vector<registry::BlockStateId>& out,
                                  std::array<std::string, 64>&         biome_out) {
    const nbt::Tag* states = section.find("block_states");
    if (states == nullptr) {
        return false;
    }
    const nbt::Tag* palette = states->find("palette");
    if (palette == nullptr || palette->type() != nbt::TagType::List || palette->list()->empty()) {
        return false;
    }

    std::vector<registry::BlockStateId> resolved;
    for (const nbt::Tag& entry : *palette->list()) {
        const nbt::Tag* name = entry.find("Name");
        if (name == nullptr) {
            return false;
        }
        const auto block = blocks.find_block(name->as_string());
        if (!block) {
            OV_LOG_ERROR("the reference world names block {}, which our registry does not have",
                         name->as_string());
            return false;
        }
        registry::BlockStateId state = blocks.default_state(*block);
        if (const nbt::Tag* properties = entry.find("Properties")) {
            std::vector<std::pair<std::string_view, std::string_view>> pairs;
            for (const auto& [key, value] : *properties->compound()) {
                pairs.emplace_back(key, value.as_string());
            }
            if (const auto exact = blocks.state_for(*block, pairs)) {
                state = *exact;
            }
        }
        resolved.push_back(state);
    }

    out.assign(4096, resolved.front());
    if (resolved.size() > 1) {
        const nbt::Tag* data = states->find("data");
        if (data == nullptr) {
            return false;
        }
        const auto* words = data->get_if<nbt::Tag::LongArray>();
        if (words == nullptr) {
            return false;
        }
        // At least four bits an entry, and entries never straddle a long: the
        // spare bits at the top of each word are simply unused.
        const auto  bits     = std::max<usize>(4, static_cast<usize>(std::bit_width(
                                                     resolved.size() - 1)));
        const usize per_word = 64 / bits;
        for (usize index = 0; index < 4096; ++index) {
            const usize word = index / per_word;
            if (word >= words->size()) {
                return false;
            }
            const usize offset = (index % per_word) * bits;
            const auto  slot   = static_cast<usize>(
                (static_cast<u64>((*words)[word]) >> offset) & ((1ULL << bits) - 1));
            if (slot >= resolved.size()) {
                return false;
            }
            out[index] = resolved[slot];
        }
    }

    // Biomes, on their own 4x4x4 grid in the same section.
    biome_out.fill("minecraft:plains");
    const nbt::Tag* biomes = section.find("biomes");
    if (biomes == nullptr) {
        return true;
    }
    const nbt::Tag* biome_palette = biomes->find("palette");
    if (biome_palette == nullptr || biome_palette->type() != nbt::TagType::List ||
        biome_palette->list()->empty()) {
        return true;
    }
    std::vector<std::string> names;
    for (const nbt::Tag& entry : *biome_palette->list()) {
        names.emplace_back(entry.as_string());
    }
    if (names.size() == 1) {
        biome_out.fill(names.front());
        return true;
    }
    const nbt::Tag* data = biomes->find("data");
    if (data == nullptr) {
        return true;
    }
    const auto* words = data->get_if<nbt::Tag::LongArray>();
    if (words == nullptr) {
        return true;
    }
    const auto  bits     = static_cast<usize>(std::bit_width(names.size() - 1));
    const usize per_word = 64 / bits;
    for (usize cell = 0; cell < 64; ++cell) {
        const usize word = cell / per_word;
        if (word >= words->size()) {
            break;
        }
        const usize offset = (cell % per_word) * bits;
        const auto  slot   = static_cast<usize>(
            (static_cast<u64>((*words)[word]) >> offset) & ((1ULL << bits) - 1));
        if (slot < names.size()) {
            biome_out[cell] = names[slot];
        }
    }
    return true;
}

/// A three-by-three block of stored chunks, presented as a level a feature can
/// be placed into.
///
/// Writes outside the nine chunks are dropped rather than being an error: a
/// vein at the corner of the region reaches into a chunk nobody read, exactly
/// as it does when the game generates one neighbour at a time.
class ReferenceLevel final : public worldgen::FeatureLevel {
public:
    ReferenceLevel(const registry::BlockRegistry& blocks, i32 centre_x, i32 centre_z)
        : blocks_(&blocks), centre_x_(centre_x), centre_z_(centre_z) {
        heights_.fill(std::numeric_limits<i32>::min());
    }

    void install(StoredChunk chunk) {
        const i32 dx = chunk.chunk_x - centre_x_ + 1;
        const i32 dz = chunk.chunk_z - centre_z_ + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) {
            return;
        }
        chunks_[static_cast<usize>(dz * 3 + dx)] = std::move(chunk);
    }

    [[nodiscard]] bool complete() const {
        return std::ranges::all_of(chunks_,
                                   [](const auto& chunk) { return !chunk.sections.empty(); });
    }

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        const StoredChunk* chunk = find(x, z);
        if (chunk == nullptr || outside_build_height(y)) {
            return registry::kAirState;
        }
        const i32 section = (y >> 4) - chunk->min_section;
        if (section < 0 || static_cast<usize>(section) >= chunk->sections.size() ||
            chunk->sections[static_cast<usize>(section)].empty()) {
            return registry::kAirState;
        }
        const usize index = static_cast<usize>(y & 15) * 256 +
                            static_cast<usize>(floor_mod(z, 16)) * 16 +
                            static_cast<usize>(floor_mod(x, 16));
        return chunk->sections[static_cast<usize>(section)][index];
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        StoredChunk* chunk = find_mutable(x, z);
        if (chunk == nullptr || outside_build_height(y)) {
            return false;
        }
        const i32 section = (y >> 4) - chunk->min_section;
        if (section < 0 || static_cast<usize>(section) >= chunk->sections.size()) {
            return false;
        }
        auto& blocks = chunk->sections[static_cast<usize>(section)];
        if (blocks.empty()) {
            blocks.assign(4096, registry::kAirState);
        }
        const usize index = static_cast<usize>(y & 15) * 256 +
                            static_cast<usize>(floor_mod(z, 16)) * 16 +
                            static_cast<usize>(floor_mod(x, 16));
        blocks[index] = state;
        written_.emplace(pack(x, y, z), state);
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType type, i32 x, i32 z) const override {
        // Computed from the blocks rather than read from the file. The two
        // heightmaps a feature asks about are the *_WG pair, which is what the
        // maps looked like partway through generation and is not what a saved
        // chunk carries. Recomputing gives the finished chunk's version, which
        // for the ores' "is this underground" test is the same answer
        // everywhere it matters.
        const i32 local_x = x - (centre_x_ - 1) * 16;
        const i32 local_z = z - (centre_z_ - 1) * 16;
        if (local_x < 0 || local_x >= 48 || local_z < 0 || local_z >= 48) {
            return min_y();
        }
        const bool motion = type == world::HeightmapType::OceanFloor ||
                            type == world::HeightmapType::OceanFloorWG ||
                            type == world::HeightmapType::MotionBlocking;
        const usize slot = static_cast<usize>(local_z * 48 + local_x) * 2 + (motion ? 1U : 0U);
        if (heights_[slot] != std::numeric_limits<i32>::min()) {
            return heights_[slot];
        }
        i32 found = min_y();
        for (i32 y = max_y(); y >= min_y(); --y) {
            const auto state = block_at(x, y, z);
            const auto block = blocks_->block_of(state);
            const bool counts = motion ? blocks_->blocks_motion(block) : !blocks_->is_air(block);
            if (counts) {
                found = y + 1;
                break;
            }
        }
        heights_[slot] = found;
        return found;
    }

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        const StoredChunk* chunk = find(x, z);
        if (chunk == nullptr) {
            return "minecraft:plains";
        }
        const i32 section = (std::clamp(y, min_y(), max_y()) >> 4) - chunk->min_section;
        if (section < 0 || static_cast<usize>(section) >= chunk->biomes.size()) {
            return "minecraft:plains";
        }
        const usize cell = static_cast<usize>((y >> 2) & 3) * 16 +
                           static_cast<usize>((floor_mod(z, 16)) >> 2) * 4 +
                           static_cast<usize>((floor_mod(x, 16)) >> 2);
        return chunk->biomes[static_cast<usize>(section)][cell];
    }

    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    /// Everything the decoration wrote, by packed position.
    [[nodiscard]] const std::map<i64, registry::BlockStateId>& written() const { return written_; }

    /// Rewrite one block without recording it as ours. Used to put the game's
    /// ores back to stone before anything is placed.
    void restore(i32 x, i32 y, i32 z, registry::BlockStateId state) {
        StoredChunk* chunk = find_mutable(x, z);
        if (chunk == nullptr) {
            return;
        }
        const i32 section = (y >> 4) - chunk->min_section;
        if (section < 0 || static_cast<usize>(section) >= chunk->sections.size() ||
            chunk->sections[static_cast<usize>(section)].empty()) {
            return;
        }
        const usize index = static_cast<usize>(y & 15) * 256 +
                            static_cast<usize>(floor_mod(z, 16)) * 16 +
                            static_cast<usize>(floor_mod(x, 16));
        chunk->sections[static_cast<usize>(section)][index] = state;
    }

    /// A position as one integer, reversibly: x in the top bits, z in the
    /// middle, y in the low twelve with the world's floor added back so it is
    /// never negative. Reversible matters — the comparison has to get the y and
    /// the chunk back out of a key.
    [[nodiscard]] static i64 pack(i32 x, i32 y, i32 z) {
        return (static_cast<i64>(x) << 40) | ((static_cast<i64>(z) & 0xFFFFFFF) << 12) |
               static_cast<i64>(y + 64);
    }

    [[nodiscard]] std::span<StoredChunk> chunks() { return chunks_; }

private:
    [[nodiscard]] const StoredChunk* find(i32 x, i32 z) const {
        const i32 dx = floor_div(x, 16) - centre_x_ + 1;
        const i32 dz = floor_div(z, 16) - centre_z_ + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) {
            return nullptr;
        }
        const StoredChunk& chunk = chunks_[static_cast<usize>(dz * 3 + dx)];
        return chunk.sections.empty() ? nullptr : &chunk;
    }

    [[nodiscard]] StoredChunk* find_mutable(i32 x, i32 z) {
        return const_cast<StoredChunk*>(find(x, z));
    }

    const registry::BlockRegistry*  blocks_;
    i32                             centre_x_;
    i32                             centre_z_;
    std::array<StoredChunk, 9>      chunks_;
    std::map<i64, registry::BlockStateId> written_;
    /// Two heightmaps per column of the 48 by 48 block, memoised.
    mutable std::array<i32, 48 * 48 * 2> heights_{};
};

/// What an ore block replaced.
///
/// Only the ores whose block occurs nowhere else in the world. Dirt, gravel,
/// granite, diorite, andesite and tuff are also placed by `ore` features, but
/// they are terrain as well, so putting them back would be a guess and they are
/// left out of the comparison rather than measured wrongly.
[[nodiscard]] std::string host_of(std::string_view ore) {
    if (ore.starts_with("minecraft:deepslate_") && ore.ends_with("_ore")) {
        return "minecraft:deepslate";
    }
    if (ore == "minecraft:infested_deepslate") {
        return "minecraft:deepslate";
    }
    if (ore == "minecraft:infested_stone") {
        return "minecraft:stone";
    }
    if (ore == "minecraft:nether_gold_ore" || ore == "minecraft:nether_quartz_ore" ||
        ore == "minecraft:ancient_debris") {
        return "minecraft:netherrack";
    }
    if (ore.ends_with("_ore")) {
        return "minecraft:stone";
    }
    return {};
}

/// The y out of a packed position, and whether it belongs to a chunk.
///
/// The pack is x in the top bits, z in the middle, y in the low twelve — a
/// world 384 blocks tall fits in twelve bits once the floor is added back.
[[nodiscard]] i32 unpack_y(i64 packed) {
    return static_cast<i32>(packed & 0xFFF) - 64;
}

[[nodiscard]] bool inside_chunk(i64 packed, i32 chunk_x, i32 chunk_z) {
    const i32 x = static_cast<i32>(packed >> 40);
    const i32 z = static_cast<i32>((packed >> 12) & 0xFFFFFFF);
    return floor_div(x, 16) == chunk_x && floor_div(z, 16) == chunk_z;
}

struct Tally {
    /// Blocks per slice of sixteen y, from -64 upwards.
    std::array<i64, 24> ours{};
    std::array<i64, 24> theirs{};
    i64                 agreed{0};
    i64                 ours_only{0};
    i64                 theirs_only{0};

    [[nodiscard]] i64 total_ours() const {
        i64 sum = 0;
        for (const i64 count : ours) sum += count;
        return sum;
    }
    [[nodiscard]] i64 total_theirs() const {
        i64 sum = 0;
        for (const i64 count : theirs) sum += count;
        return sum;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);

    auto pack = registry::BlockRegistry::load(options.pack);
    if (!pack) {
        OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
        return 1;
    }
    auto features = worldgen::FeatureRegistry::load(options.data, *pack);
    if (!features) {
        OV_LOG_ERROR("features: {}", worldgen::to_string(features.error()));
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, *pack, *features);
    if (!decorator) {
        OV_LOG_ERROR("decorator: {}", worldgen::to_string(decorator.error()));
        return 1;
    }

    if (!options.order.empty()) {
        for (usize step = 0; step < worldgen::kDecorationStepCount; ++step) {
            const auto which = static_cast<worldgen::DecorationStep>(step);
            if (worldgen::to_string(which) != options.order) {
                continue;
            }
            const auto order = decorator->order_at(which);
            fmt::print("{} — {} placed features, in the order they are seeded\n",
                       worldgen::to_string(which), order.size());
            for (usize index = 0; index < order.size(); ++index) {
                fmt::print("  {:>3}  {}\n", index, order[index]);
            }
            return 0;
        }
        fmt::print("no step called {}\n", options.order);
        return 1;
    }

    if (options.missing) {
        fmt::print("{} placed features built, {} named by a biome and not built\n",
                   features->placed_count(), decorator->missing_count());
        for (const auto& name : decorator->missing()) {
            fmt::print("  {}\n", name);
        }
        fmt::print("\nwhy each thing failed:\n");
        std::map<std::string, usize> reasons;
        for (const auto& [name, error] : features->unavailable()) {
            reasons[std::string(worldgen::to_string(error))] += 1;
        }
        for (const auto& [reason, count] : reasons) {
            fmt::print("  {:>5}  {}\n", count, reason);
        }
        return 0;
    }

    if (!std::filesystem::is_directory(options.world / "region")) {
        OV_LOG_ERROR("{} has no region/", options.world.string());
        return 1;
    }

    // Read the whole reference world's chunk index first, so that a chunk's
    // eight neighbours can be found wherever they live.
    std::map<std::pair<i32, i32>, std::filesystem::path> regions;
    for (const auto& entry : std::filesystem::directory_iterator(options.world / "region")) {
        if (entry.path().extension() != ".mca") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        // r.<x>.<z>
        const auto first  = stem.find('.');
        const auto second = stem.find('.', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }
        regions.emplace(std::pair<i32, i32>{std::atoi(stem.substr(first + 1, second - first - 1).c_str()),
                                            std::atoi(stem.substr(second + 1).c_str())},
                        entry.path());
    }

    std::unordered_map<std::string, nbt::RegionFile> open_regions;
    const auto load_chunk = [&](i32 chunk_x, i32 chunk_z) -> std::optional<StoredChunk> {
        const auto found =
            regions.find({floor_div(chunk_x, 32), floor_div(chunk_z, 32)});
        if (found == regions.end()) {
            return std::nullopt;
        }
        auto region = nbt::RegionFile::open(found->second);
        if (!region) {
            return std::nullopt;
        }
        const auto local_x = static_cast<u32>(floor_mod(chunk_x, 32));
        const auto local_z = static_cast<u32>(floor_mod(chunk_z, 32));
        if (!region->has_chunk(local_x, local_z)) {
            return std::nullopt;
        }
        auto document = region->read_chunk(local_x, local_z);
        if (!document) {
            return std::nullopt;
        }
        const nbt::Tag* status = document->root.find("Status");
        if (status == nullptr || status->as_string() != "minecraft:full") {
            return std::nullopt;
        }
        const nbt::Tag* list = document->root.find("sections");
        if (list == nullptr || list->type() != nbt::TagType::List) {
            return std::nullopt;
        }
        StoredChunk chunk;
        chunk.chunk_x = chunk_x;
        chunk.chunk_z = chunk_z;
        chunk.sections.resize(24);
        chunk.biomes.resize(24);
        for (const nbt::Tag& section : *list->list()) {
            const nbt::Tag* y_tag = section.find("Y");
            if (y_tag == nullptr) {
                continue;
            }
            const auto index = static_cast<i32>(y_tag->as_i64()) - chunk.min_section;
            if (index < 0 || index >= 24) {
                continue;
            }
            (void)decode_section(section, *pack, chunk.sections[static_cast<usize>(index)],
                                 chunk.biomes[static_cast<usize>(index)]);
        }
        return chunk;
    };

    // Only the chunks whose eight neighbours are also finished, so that no
    // disagreement is an artefact of a vein reaching into a chunk nobody read.
    std::vector<std::pair<i32, i32>> candidates;
    for (const auto& [where, path] : regions) {
        auto region = nbt::RegionFile::open(path);
        if (!region) {
            continue;
        }
        for (u32 local = 0; local < 1024; ++local) {
            if (region->has_chunk(local % 32, local / 32)) {
                candidates.emplace_back(where.first * 32 + static_cast<i32>(local % 32),
                                        where.second * 32 + static_cast<i32>(local / 32));
            }
        }
        // Four times as many candidates as chunks wanted: a chunk on the edge
        // of the generated area has neighbours that were never finished and is
        // skipped.
        if (candidates.size() >= static_cast<usize>(options.chunks) * 4) {
            break;
        }
    }

    // The 3x3 neighbourhoods, read once and reused: the sweep runs the same
    // feature over them forty times and rereading the region files each time
    // would dominate.
    const auto build_level = [&](i32 chunk_x, i32 chunk_z) -> std::shared_ptr<ReferenceLevel> {
        auto level = std::make_shared<ReferenceLevel>(*pack, chunk_x, chunk_z);
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                auto chunk = load_chunk(chunk_x + dx, chunk_z + dz);
                if (!chunk) {
                    return nullptr;
                }
                level->install(std::move(*chunk));
            }
        }
        return level->complete() ? level : nullptr;
    };

    /// What the game has in the centre chunk, and then the same neighbourhood
    /// with every ore put back to the stone it replaced.
    const auto strip_ores = [&](ReferenceLevel& level, i32 chunk_x, i32 chunk_z) {
        std::map<i64, std::string> theirs;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                for (i32 y = level.min_y(); y <= level.max_y(); ++y) {
                    for (i32 z = 0; z < 16; ++z) {
                        for (i32 x = 0; x < 16; ++x) {
                            const i32  world_x = (chunk_x + dx) * 16 + x;
                            const i32  world_z = (chunk_z + dz) * 16 + z;
                            const auto state   = level.block_at(world_x, y, world_z);
                            const auto name    = pack->block_name(pack->block_of(state));
                            const auto host    = host_of(name);
                            if (host.empty()) {
                                continue;
                            }
                            if (dx == 0 && dz == 0) {
                                theirs.emplace(ReferenceLevel::pack(world_x, y, world_z),
                                               std::string(name));
                            }
                            if (const auto block = pack->find_block(host)) {
                                level.restore(world_x, y, world_z, pack->default_state(*block));
                            }
                        }
                    }
                }
            }
        }
        return theirs;
    };

    std::vector<std::pair<i32, i32>> usable;
    for (const auto& [chunk_x, chunk_z] : candidates) {
        if (usable.size() >= static_cast<usize>(options.chunks)) {
            break;
        }
        if (build_level(chunk_x, chunk_z) != nullptr) {
            usable.emplace_back(chunk_x, chunk_z);
        }
    }

    if (!options.calibrate.empty()) {
        const auto* feature = features->placed(options.calibrate);
        if (feature == nullptr) {
            fmt::print("{} was not built; --missing says why\n", options.calibrate);
            return 1;
        }

        // Score the *origins* the pipeline produces, not the blocks it writes.
        //
        // A block-level score cannot tell a right answer from a lucky one: ore
        // is dense enough underground that a position picked at random is often
        // within a few blocks of some. An origin is one point per attempt and
        // the vein is built around it, so "is there ore of this kind within two
        // blocks of the origin" is close to a yes/no question about whether the
        // seed is right. Ten origins a chunk at roughly six percent by chance
        // means a correct seed stands out by an order of magnitude.
        struct Prepared {
            std::shared_ptr<ReferenceLevel> level;
            std::map<i64, std::string>      theirs;
            i32                             chunk_x{0};
            i32                             chunk_z{0};
        };
        std::vector<Prepared> prepared;
        for (const auto& [chunk_x, chunk_z] : usable) {
            Prepared entry;
            entry.level = build_level(chunk_x, chunk_z);
            if (entry.level == nullptr) {
                continue;
            }
            entry.theirs = strip_ores(*entry.level, chunk_x, chunk_z);
            if (!options.ore.empty()) {
                std::erase_if(entry.theirs, [&](const auto& pair) {
                    return pair.second.find(options.ore) == std::string::npos;
                });
            }
            entry.chunk_x = chunk_x;
            entry.chunk_z = chunk_z;
            prepared.push_back(std::move(entry));
        }
        fmt::print("\n{} — our sorter puts it at index {} of step {}, over {} chunks\n",
                   options.calibrate,
                   decorator->index_of(static_cast<worldgen::DecorationStep>(options.step),
                                       options.calibrate),
                   options.step, prepared.size());

        // The box a vein of this feature can cover, given its origin.
        //
        // Not a symmetric ball: an ore vein's spheres are centred nought to two
        // blocks *below* the origin and have a radius of about one, so the
        // origin block itself is ore only about one time in five even when the
        // seed is right. Looking one block out sideways and three down turns a
        // marginal signal into an unmistakable one.
        const auto in_vein_box = [&](const Prepared& entry, BlockPos at) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dy = -3; dy <= 1; ++dy) {
                        if (entry.theirs.contains(
                                ReferenceLevel::pack(at.x + dx, at.y + dy, at.z + dz))) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        const auto near_ore = [&](const Prepared& entry, BlockPos at, i32 slack, bool ignore_y) {
            for (i32 dx = -slack; dx <= slack; ++dx) {
                for (i32 dz = -slack; dz <= slack; ++dz) {
                    if (ignore_y) {
                        for (i32 y = entry.level->min_y(); y <= entry.level->max_y(); ++y) {
                            if (entry.theirs.contains(
                                    ReferenceLevel::pack(at.x + dx, y, at.z + dz))) {
                                return true;
                            }
                        }
                        continue;
                    }
                    for (i32 dy = -slack; dy <= slack; ++dy) {
                        if (entry.theirs.contains(
                                ReferenceLevel::pack(at.x + dx, at.y + dy, at.z + dz))) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        fmt::print("{:>5} {:>6} {:>9} {:>9} {:>9}\n", "step", "index", "origins", "on ore", "within 2");
        i32 best_step  = -1;
        i32 best_index = -1;
        i64 best_near  = -1;
        const i32 first_step = options.step < 0 ? 0 : options.step;
        const i32 last_step  = options.step < 0 ? 10 : options.step;
        for (i32 step = first_step; step <= last_step; ++step) {
            for (i32 index = 0; index < options.span; ++index) {
                i64 origins = 0;
                i64 near    = 0;
                i64 column  = 0;
                for (const Prepared& entry : prepared) {
                    const auto kind = worldgen::configured_feature_random();
                    const i64  deco = worldgen::decoration_seed(options.seed, entry.chunk_x * 16,
                                                                entry.chunk_z * 16, kind);
                    // The raw offset from the chunk's decoration seed, not
                    // index and step separately. Every (index, step) pair is
                    // some offset, so sweeping the offset tests the decoration
                    // seed itself without assuming how the two are combined.
                    worldgen::FeatureRandom random{
                        kind, options.raw ? deco + index
                                          : worldgen::feature_seed(deco, index, step)};
                    worldgen::FeatureContext context;
                    context.blocks       = &*pack;
                    context.feature_name = feature->name;
                    context.biomes       = &decorator->biome_features();
                    worldgen::expand(
                        feature->placement, context, *entry.level, random,
                        {entry.chunk_x * 16, entry.level->min_y(), entry.chunk_z * 16},
                        [&](BlockPos at) {
                            ++origins;
                            // Slack zero. A vein covers its own origin nearly
                            // always, and ore is rare enough that a wrong seed
                            // hits it about one time in a hundred — so this
                            // separates a right seed from a wrong one by two
                            // orders of magnitude, where a slack of two only
                            // measures how much ore is about.
                            if (in_vein_box(entry, at)) {
                                ++near;
                            }
                            if (near_ore(entry, at, 2, false)) {
                                ++column;
                            }
                        });
                }
                if (near > best_near) {
                    best_near  = near;
                    best_step  = step;
                    best_index = index;
                }
                if (!options.raw || near * 4 >= origins) {
                    fmt::print("{:>5} {:>6} {:>9} {:>9} {:>9}\n", step, index, origins, near,
                               column);
                }
            }
        }
        fmt::print("\nbest: step {}, index {} — {} origins of {} landed on ore\n", best_step,
                   best_index, best_near, prepared.empty() ? 0 : 0);
        return 0;
    }

    std::map<std::string, Tally> tallies;
    usize                        chunks_done = 0;
    usize                        skipped     = 0;
    for (const auto& [chunk_x, chunk_z] : usable) {
        if (chunks_done >= static_cast<usize>(options.chunks)) {
            break;
        }
        auto held = build_level(chunk_x, chunk_z);
        if (held == nullptr) {
            ++skipped;
            continue;
        }
        ReferenceLevel& level = *held;

        const auto theirs = strip_ores(level, chunk_x, chunk_z);

        // All nine chunks, not only the middle one. A vein whose origin is in
        // a neighbour finishes inside this chunk, so a comparison that
        // decorated one chunk would be missing about a fifth of the ore that
        // the game put there and would read as a shortfall in our own.
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                (void)decorator->decorate(level, chunk_x + dx, chunk_z + dz, options.seed);
            }
        }

        std::map<i64, std::string> ours;
        for (const auto& [where, state] : level.written()) {
            const auto name = pack->block_name(pack->block_of(state));
            if (host_of(name).empty()) {
                continue;
            }
            // Only the centre chunk: a vein of ours that reaches into a
            // neighbour has no counterpart to compare against, because the
            // neighbour's own decoration pass is not being run.
            const i32 world_x = static_cast<i32>(where >> 40);
            const i32 world_z = static_cast<i32>((where >> 12) & 0xFFFFF) - 0;
            (void)world_x;
            (void)world_z;
            ours.emplace(where, std::string(name));
        }

        const auto slice = [&](i32 y) {
            return static_cast<usize>(std::clamp((y + 64) / 16, 0, 23));
        };
        const auto y_of = [](i64 where) { return static_cast<i32>(where & 0xFFF) - 0; };
        (void)y_of;

        // Positions are compared through the recorded maps rather than by
        // scanning the world again: a vein we placed and a vein the game placed
        // may sit on the same block, and only the two records can tell an
        // agreement from a coincidence of terrain.
        for (const auto& [where, name] : theirs) {
            Tally& tally = tallies[name];
            tally.theirs[slice(unpack_y(where))] += 1;
            const auto mine = ours.find(where);
            if (mine != ours.end() && mine->second == name) {
                ++tally.agreed;
            } else {
                ++tally.theirs_only;
            }
        }
        for (const auto& [where, name] : ours) {
            if (!inside_chunk(where, chunk_x, chunk_z)) {
                continue;
            }
            Tally& tally = tallies[name];
            tally.ours[slice(unpack_y(where))] += 1;
            const auto other = theirs.find(where);
            if (other == theirs.end() || other->second != name) {
                ++tally.ours_only;
            }
        }
        ++chunks_done;
    }

    fmt::print("\nseed {}, {} chunks compared ({} skipped for an unfinished neighbour)\n",
               options.seed, chunks_done, skipped);
    fmt::print("{} placed features built of {} configured; {} named by a biome and missing\n",
               features->placed_count(), features->configured_count(),
               decorator->missing_count());

    i64 all_agreed = 0;
    i64 all_theirs = 0;
    i64 all_ours   = 0;
    fmt::print("\n{:<34} {:>9} {:>9} {:>9} {:>9} {:>8}\n", "ore", "theirs", "ours", "same block",
               "ours only", "");
    for (const auto& [name, tally] : tallies) {
        all_agreed += tally.agreed;
        all_theirs += tally.total_theirs();
        all_ours += tally.total_ours();
        const f64 rate = tally.total_theirs() == 0
                             ? 0.0
                             : 100.0 * static_cast<f64>(tally.agreed) /
                                   static_cast<f64>(tally.total_theirs());
        fmt::print("{:<34} {:>9} {:>9} {:>9} {:>9} {:>7.3f} %\n", name, tally.total_theirs(),
                   tally.total_ours(), tally.agreed, tally.ours_only, rate);
    }
    fmt::print("{:<34} {:>9} {:>9} {:>9} {:>9} {:>7.3f} %\n", "all", all_theirs, all_ours,
               all_agreed,
               all_ours - all_agreed,
               all_theirs == 0 ? 0.0
                               : 100.0 * static_cast<f64>(all_agreed) /
                                     static_cast<f64>(all_theirs));

    fmt::print("\nby height, theirs / ours, in slices of sixteen:\n");
    for (const auto& [name, tally] : tallies) {
        fmt::print("\n  {}\n", name);
        for (usize band = 0; band < tally.ours.size(); ++band) {
            if (tally.ours[band] == 0 && tally.theirs[band] == 0) {
                continue;
            }
            fmt::print("    y {:>4} .. {:>4}   {:>8} / {:>8}{}\n",
                       static_cast<i32>(band) * 16 - 64, static_cast<i32>(band) * 16 - 49,
                       tally.theirs[band], tally.ours[band],
                       tally.theirs[band] == tally.ours[band] ? "" : "   <-");
        }
    }
    return all_agreed == all_theirs && all_ours == all_agreed ? 0 : 2;
}
