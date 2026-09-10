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
#include <limits>
#include <tuple>
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
    /// Compare *trees* instead of ores: strip the vegetation the game grew,
    /// grow ours over the same terrain, and compare trunk by trunk.
    bool trees{false};
    /// Print, for the first N trees that came out the wrong shape, our shape
    /// and theirs layer by layer.
    i32 show{0};
    /// Only print the ones whose placed feature contains this.
    std::string only;
    /// Only print trees whose trunk landed where the game's did — a pure shape
    /// difference, with the placement already known to be right.
    bool matched{false};
    /// Only print the first try of a feature in its chunk.
    bool first{false};
    /// Only print trees whose *logs* all match the game's, block for block —
    /// the trunk placer is then known right and only the foliage is wrong.
    bool logs_exact{false};
    /// Only print trees that wrote at least this many logs — the cheap way to
    /// pick the branching placers out of a forest of blob oaks.
    i32 min_logs{0};
    /// One chunk in this many is taken from each region of the reference
    /// world. Smaller means a bigger, slower sample.
    i32 stride{97};
    /// Measure against a *probe* world instead of the reference world.
    ///
    /// A probe world (scripts/probe_tree.sh) has a biome whose feature lists
    /// are all empty but one: `count(1) → in_square → heightmap → <feature>`
    /// at the vegetal step. Every chunk therefore carries exactly one tree of
    /// one known species, isolated, on plain terrain. That turns "40 % of the
    /// trunks are in the right place" — a number in which the selector, the
    /// biome, the neighbours and the placer are all mixed — into "this placer
    /// is right in 1291 of 1329 trees", which is a fact about the placer.
    bool probe{false};
    /// The configured feature the probe world was built with.
    std::string feature;
    /// Its index at the step, as the probe datapack wrote it.
    i32 index{0};
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
        } else if (argument == "--trees") {
            options.trees = true;
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        } else if (argument.starts_with("--only=")) {
            options.only = value("--only=");
        } else if (argument == "--matched") {
            options.matched = true;
        } else if (argument == "--probe") {
            options.probe = true;
            options.step  = 9;
        } else if (argument.starts_with("--feature=")) {
            options.feature = value("--feature=");
        } else if (argument.starts_with("--index=")) {
            options.index = std::atoi(value("--index=").c_str());
        } else if (argument.starts_with("--min-logs=")) {
            options.min_logs = std::atoi(value("--min-logs=").c_str());
        } else if (argument == "--logs-exact") {
            options.logs_exact = true;
        } else if (argument == "--first") {
            options.first = true;
        } else if (argument.starts_with("--stride=")) {
            options.stride = std::max(1, std::atoi(value("--stride=").c_str()));
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
        if (current_group_ >= 0) {
            // Appended, not assigned. Two trees overlap constantly, and the
            // world keeps the last writer — but for judging one tree's shape
            // what matters is what *it* tried to write, so every write is kept
            // with the attempt that made it.
            group_writes_.emplace_back(current_group_, pack(x, y, z), state);
        }
        // Assign, not emplace. Features overwrite each other constantly — a
        // granite blob lands on stone and an iron vein lands on the granite —
        // and `emplace` keeps the *first* write while the block array keeps the
        // last, so the record and the world disagree. That disagreement cost
        // 2006 blocks of the comparison: they were ours and correct, and the
        // record still called them granite.
        written_[pack(x, y, z)] = state;
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

    /// Which attempt wrote each position.
    ///
    /// One "attempt" is one call of one placed feature at one position — one
    /// tree. Without it a forest is a single sea of logs and leaves and there
    /// is no way to say that *this* tree is the wrong shape.
    void set_group(i32 group) { current_group_ = group; }

    /// Every write, in order, with the attempt that made it.
    [[nodiscard]] const std::vector<std::tuple<i32, i64, registry::BlockStateId>>&
    group_writes() const {
        return group_writes_;
    }

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
    std::vector<std::tuple<i32, i64, registry::BlockStateId>> group_writes_;
    i32                                                       current_group_{-1};
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

[[nodiscard]] i32 unpack_x(i64 packed) {
    return static_cast<i32>(packed >> 40);
}

/// z lives in twenty-eight bits and has to be sign-extended out of them.
///
/// Reading it as unsigned turns every negative z into a number near 2^28, which
/// puts the position in no chunk at all. Half the reference world's patches are
/// at negative z, so a comparison that got this wrong silently dropped their
/// side of the tally and read as a shortfall in our own generation.
[[nodiscard]] i32 unpack_z(i64 packed) {
    const i64 raw = (packed >> 12) & 0xFFFFFFF;
    return static_cast<i32>((raw ^ 0x8000000) - 0x8000000);
}

[[nodiscard]] bool inside_chunk(i64 packed, i32 chunk_x, i32 chunk_z) {
    return floor_div(unpack_x(packed), 16) == chunk_x &&
           floor_div(unpack_z(packed), 16) == chunk_z;
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
    // The decorator needs the dimension's biomes in the biome source's own
    // order: that order is the sorter's tie-break and therefore part of the
    // seed. The reference world is the overworld.
    auto biomes = worldgen::BiomeSource::load(options.reports, "overworld");
    if (!biomes) {
        OV_LOG_ERROR("biome source: {}", worldgen::to_string(biomes.error()));
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, *pack, *features, *biomes);
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
        if (!options.trees && candidates.size() >= static_cast<usize>(options.chunks) * 4) {
            break;
        }
    }

    // Trees are measured across the whole reference world rather than out of
    // its first region.
    //
    // The ores are everywhere and one region is a fair sample of them; trees
    // are not. The reference world is a scatter of small patches, and the first
    // one in coordinate order can easily be ocean — which is exactly what
    // happened, and read as "no trees anywhere" rather than as "no trees here".
    // So this takes a handful of interior chunks from every patch instead.
    if (options.trees) {
        std::vector<std::pair<i32, i32>> spread;
        std::map<std::pair<i32, i32>, i32> per_region;
        for (const auto& [chunk_x, chunk_z] : candidates) {
            const std::pair<i32, i32> region{floor_div(chunk_x, 32), floor_div(chunk_z, 32)};
            // Interior only: a chunk on a region's rim has neighbours in a
            // region the reference world may not have generated.
            if (floor_mod(chunk_x, 32) == 0 || floor_mod(chunk_x, 32) == 31 ||
                floor_mod(chunk_z, 32) == 0 || floor_mod(chunk_z, 32) == 31) {
                continue;
            }
            if (per_region[region]++ % options.stride != 0) {
                continue;
            }
            spread.emplace_back(chunk_x, chunk_z);
        }
        candidates = std::move(spread);
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

    if (options.probe) {
        // One species, one tree per chunk, nothing else in the world.
        //
        // The reference world answers "is our forest the game's forest", which
        // is a dozen questions at once. A probe world answers one: given the
        // generator state the game had — and the seeding is measured at 100 %
        // elsewhere — does *this placer* write the same blocks? A wrong answer
        // here is a wrong placer and cannot be anything else.
        // Several features, comma-separated, go to indices 0, 1, 2… exactly as
        // the probe datapack wrote them. Running them all is not a convenience:
        // a probe world holding three species has all three on the disk, and
        // scoring one of them against a world that contains the other two calls
        // a neighbour's leaves a difference.
        std::vector<std::string>              names;
        std::vector<const worldgen::Feature*> probes;
        for (usize start = 0; start <= options.feature.size();) {
            const auto stop = options.feature.find(',', start);
            names.push_back(options.feature.substr(
                start, stop == std::string::npos ? std::string::npos : stop - start));
            if (stop == std::string::npos) {
                break;
            }
            start = stop + 1;
        }
        for (const auto& name : names) {
            const auto* found = features->configured(name);
            if (found == nullptr) {
                fmt::print("{} is not a configured feature we built; --missing says why\n", name);
                return 1;
            }
            probes.push_back(found);
        }
        const auto& tags   = features->tags();
        const auto  in_tag = [&](std::string_view tag, registry::BlockStateId state) {
            return tags.contains(tag, pack->block_of(state));
        };
        const auto is_log  = [&](registry::BlockStateId state) {
            return in_tag("minecraft:logs", state);
        };
        const auto is_leaf = [&](registry::BlockStateId state) {
            return in_tag("minecraft:leaves", state);
        };
        const auto is_wood = [&](registry::BlockStateId state) {
            return is_log(state) || is_leaf(state);
        };

        // A probe world has no other decoration at all, so the whole of what a
        // tree left behind is the wood plus the few blocks its decorators add.
        std::set<u16> stripped;
        for (const std::string_view name :
             {"minecraft:vine", "minecraft:cocoa", "minecraft:bee_nest",
              "minecraft:moss_carpet", "minecraft:shroomlight", "minecraft:mangrove_roots"}) {
            if (const auto block = pack->find_block(name)) {
                stripped.insert(block->value());
            }
        }
        for (usize index = 0; index < pack->block_count(); ++index) {
            const registry::BlockId block{static_cast<u16>(index)};
            if (tags.contains("minecraft:logs", block) ||
                tags.contains("minecraft:leaves", block)) {
                stripped.insert(block.value());
            }
        }
        const auto air = pack->find_block("minecraft:air");
        if (!air) {
            return 1;
        }
        const auto air_state = pack->default_state(*air);

        std::map<std::string, std::pair<i64, i64>> by_feature;
        i64   trees      = 0;
        i64   exact      = 0;
        i64   logs_right = 0;
        i64   theirs_all = 0;
        i64   ours_all   = 0;
        i64   agreed     = 0;
        i32   shown      = 0;
        usize done       = 0;
        for (const auto& [chunk_x, chunk_z] : usable) {
            if (done >= static_cast<usize>(options.chunks)) {
                break;
            }
            auto held = build_level(chunk_x, chunk_z);
            if (held == nullptr) {
                continue;
            }
            ReferenceLevel& level = *held;
            ++done;

            std::map<i64, registry::BlockStateId> their_wood;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (i32 y = level.min_y(); y <= level.max_y(); ++y) {
                        for (i32 z = 0; z < 16; ++z) {
                            for (i32 x = 0; x < 16; ++x) {
                                const i32  wx    = (chunk_x + dx) * 16 + x;
                                const i32  wz    = (chunk_z + dz) * 16 + z;
                                const auto state = level.block_at(wx, y, wz);
                                if (is_wood(state)) {
                                    their_wood.emplace(ReferenceLevel::pack(wx, y, wz), state);
                                }
                            }
                        }
                    }
                }
            }
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (i32 y = level.min_y(); y <= level.max_y(); ++y) {
                        for (i32 z = 0; z < 16; ++z) {
                            for (i32 x = 0; x < 16; ++x) {
                                const i32  wx    = (chunk_x + dx) * 16 + x;
                                const i32  wz    = (chunk_z + dz) * 16 + z;
                                const auto state = level.block_at(wx, y, wz);
                                if (stripped.contains(pack->block_of(state).value())) {
                                    level.restore(wx, y, wz, air_state);
                                }
                            }
                        }
                    }
                }
            }

            // The nine trees, so that a neighbour's canopy reaching into this
            // chunk is our neighbour's canopy and not a difference.
            const auto kind = worldgen::configured_feature_random();
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 origin_x = (chunk_x + dx) * 16;
                    const i32 origin_z = (chunk_z + dz) * 16;
                    const i64 seed =
                        worldgen::decoration_seed(options.seed, origin_x, origin_z, kind);
                    for (usize slot = 0; slot < probes.size(); ++slot) {
                        worldgen::FeatureRandom random{
                            kind, worldgen::feature_seed(
                                      seed, options.index + static_cast<i32>(slot), options.step)};
                        // The probe's pipeline, spelled out: `count(1)` draws
                        // nothing, `in_square` is two `nextInt(16)`, `heightmap`
                        // is none. Everything after belongs to the tree.
                        const i32 at_x = origin_x + random.next_int(16);
                        const i32 at_z = origin_z + random.next_int(16);
                        const i32 at_y =
                            level.height(world::HeightmapType::OceanFloorWG, at_x, at_z);
                        worldgen::FeatureContext context;
                        context.blocks       = &*pack;
                        context.feature_name = names[slot];
                        context.biomes       = &decorator->biome_features();
                        level.set_group((dz * 3 + dx + 4) * 16 + static_cast<i32>(slot));
                        (void)probes[slot]->place(context, level, random, {at_x, at_y, at_z});
                    }
                }
            }
            level.set_group(-1);

            std::map<i32, std::map<i64, registry::BlockStateId>> ours;
            for (const auto& [group, where, state] : level.group_writes()) {
                if (is_wood(state)) {
                    ours[group][where] = state;
                }
            }
            std::map<i64, registry::BlockStateId> our_wood;
            for (const auto& [group, cells] : ours) {
                (void)group;
                for (const auto& [where, state] : cells) {
                    our_wood[where] = state;
                }
            }
            for (const auto& [where, state] : their_wood) {
                if (inside_chunk(where, chunk_x, chunk_z)) {
                    ++theirs_all;
                    const auto mine = our_wood.find(where);
                    if (mine != our_wood.end() &&
                        pack->block_of(mine->second) == pack->block_of(state)) {
                        ++agreed;
                    }
                }
            }
            for (const auto& [where, state] : our_wood) {
                (void)state;
                if (inside_chunk(where, chunk_x, chunk_z)) {
                    ++ours_all;
                }
            }

            for (usize slot = 0; slot < probes.size(); ++slot) {
            const auto centre = ours.find(4 * 16 + static_cast<i32>(slot));
            if (centre == ours.end() || centre->second.empty()) {
                continue;
            }
            const auto& cells = centre->second;
            ++trees;
            ++by_feature[names[slot]].second;

            i32 low_x = std::numeric_limits<i32>::max();
            i32 low_y = low_x;
            i32 low_z = low_x;
            i32 high_x = std::numeric_limits<i32>::min();
            i32 high_y = high_x;
            i32 high_z = high_x;
            bool shape = true;
            bool trunk = true;
            for (const auto& [where, state] : cells) {
                const i32 x = unpack_x(where);
                const i32 y = unpack_y(where);
                const i32 z = unpack_z(where);
                low_x = std::min(low_x, x); high_x = std::max(high_x, x);
                low_y = std::min(low_y, y); high_y = std::max(high_y, y);
                low_z = std::min(low_z, z); high_z = std::max(high_z, z);
                const auto theirs = their_wood.find(where);
                const bool same   = theirs != their_wood.end() &&
                                  pack->block_of(theirs->second) == pack->block_of(state);
                if (!same) {
                    shape = false;
                    if (is_log(state)) {
                        trunk = false;
                    }
                }
            }
            for (i32 y = low_y; y <= high_y; ++y) {
                for (i32 z = low_z; z <= high_z; ++z) {
                    for (i32 x = low_x; x <= high_x; ++x) {
                        const i64  where  = ReferenceLevel::pack(x, y, z);
                        const auto theirs = their_wood.find(where);
                        if (theirs == their_wood.end()) {
                            continue;
                        }
                        if (!our_wood.contains(where)) {
                            shape = false;
                            if (is_log(theirs->second)) {
                                trunk = false;
                            }
                        }
                    }
                }
            }
            if (shape) {
                ++exact;
                ++by_feature[names[slot]].first;
            }
            if (trunk) ++logs_right;
            if (!shape && shown < options.show) {
                ++shown;
                fmt::print("\n{} at chunk {},{} — ours then theirs, layer by layer\n",
                           names[slot], chunk_x, chunk_z);
                for (i32 y = high_y; y >= low_y; --y) {
                    fmt::print("  y {:>4}\n", y);
                    for (i32 z = low_z; z <= high_z; ++z) {
                        std::string mine;
                        std::string them;
                        for (i32 x = low_x; x <= high_x; ++x) {
                            const i64  where = ReferenceLevel::pack(x, y, z);
                            const auto our   = cells.find(where);
                            mine += our == cells.end()      ? '.'
                                    : is_log(our->second)   ? '#'
                                                            : 'o';
                            const auto other = their_wood.find(where);
                            them += other == their_wood.end() ? '.'
                                    : is_log(other->second)   ? '#'
                                                              : 'o';
                        }
                        fmt::print("    {}   {}\n", mine, them);
                    }
                }
            }
            }
        }

        fmt::print("\nprobe world {}, seed {}, feature {} at step {} index {}\n",
                   options.world.string(), options.seed, options.feature, options.step,
                   options.index);
        fmt::print("{} chunks, {} trees we grew in the middle chunk\n", done, trees);
        const auto percent = [](i64 part, i64 whole) {
            return whole == 0 ? 0.0 : 100.0 * static_cast<f64>(part) / static_cast<f64>(whole);
        };
        fmt::print("  logs identical      {:>6}  ({:.3f} %)\n", logs_right,
                   percent(logs_right, trees));
        fmt::print("  whole tree identical{:>6}  ({:.3f} %)\n", exact, percent(exact, trees));
        fmt::print("  wood blocks: theirs {}, ours {}, same block {} ({:.3f} %)\n", theirs_all,
                   ours_all, agreed, percent(agreed, theirs_all));
        if (by_feature.size() > 1) {
            fmt::print("\nby feature:\n");
            for (const auto& [name, score] : by_feature) {
                fmt::print("  {:<44} {:>5} / {:<5} {:.1f} %\n", name, score.first, score.second,
                           percent(score.first, score.second));
            }
        }
        return 0;
    }

    if (options.trees) {
        // Trees, not ores, and the question is different enough to need its own
        // protocol.
        //
        // An ore can be put back: it replaced exactly one stone block and the
        // stone is known. A tree cannot — it grew into air that the rest of
        // decoration then filled with grass, flowers and snow. So instead of
        // restoring, this strips: every log, leaf, sapling, vine and small plant
        // in the neighbourhood is set to air, which is very close to what the
        // world looked like when the game reached the vegetal step, and our
        // decoration is run over the result.
        //
        // Two failures are counted apart because they have different causes:
        //
        //   * a trunk in the wrong *place* means the placement pipeline or the
        //     seed is wrong, and the tree code is not even reached;
        //   * a trunk in the right place with the wrong *shape* means the
        //     placement is right and a placer draws differently.
        //
        // A percentage over all leaf blocks would average the two together and
        // say nothing about either.
        const auto& tags = features->tags();

        const auto in_tag = [&](std::string_view tag, registry::BlockStateId state) {
            return tags.contains(tag, pack->block_of(state));
        };
        const auto is_log = [&](registry::BlockStateId state) {
            return in_tag("minecraft:logs", state);
        };
        const auto is_leaf = [&](registry::BlockStateId state) {
            return in_tag("minecraft:leaves", state);
        };
        const auto is_dirt = [&](registry::BlockStateId state) {
            return in_tag("minecraft:dirt", state);
        };

        // What a tree or the vegetation around it may have left behind. Water,
        // seagrass and kelp are deliberately *not* here: they are terrain, and
        // stripping them would drain every swamp.
        static constexpr std::array<std::string_view, 30> kStrippedNames{
            "minecraft:vine",          "minecraft:cocoa",
            "minecraft:bee_nest",      "minecraft:mangrove_propagule",
            "minecraft:moss_carpet",   "minecraft:snow",
            "minecraft:sweet_berry_bush", "minecraft:pumpkin",
            "minecraft:melon",         "minecraft:cactus",
            "minecraft:sugar_cane",    "minecraft:lily_pad",
            "minecraft:brown_mushroom", "minecraft:red_mushroom",
            "minecraft:pink_petals",   "minecraft:azalea",
            "minecraft:flowering_azalea", "minecraft:spore_blossom",
            "minecraft:cave_vines",    "minecraft:cave_vines_plant",
            "minecraft:glow_lichen",   "minecraft:hanging_roots",
            "minecraft:grass",         "minecraft:tall_grass",
            "minecraft:fern",          "minecraft:large_fern",
            "minecraft:dead_bush",     "minecraft:mangrove_roots",
            "minecraft:muddy_mangrove_roots", "minecraft:fire",
        };
        std::set<u16> stripped;
        for (const std::string_view name : kStrippedNames) {
            if (const auto block = pack->find_block(name)) {
                stripped.insert(block->value());
            }
        }
        for (usize index = 0; index < pack->block_count(); ++index) {
            const registry::BlockId block{static_cast<u16>(index)};
            if (tags.contains("minecraft:logs", block) ||
                tags.contains("minecraft:leaves", block) ||
                tags.contains("minecraft:saplings", block) ||
                tags.contains("minecraft:flowers", block)) {
                stripped.insert(block.value());
            }
        }

        const auto air = pack->find_block("minecraft:air");
        if (!air) {
            return 1;
        }
        const auto air_state = pack->default_state(*air);

        i64 their_trunks   = 0;
        i64 matched_trunks = 0;
        i64 our_trunks     = 0;
        i64 our_groups     = 0;
        i64 same_shape     = 0;
        i64 groups_at_real_trunk     = 0;
        i64 same_shape_at_real_trunk = 0;
        i64 wrong_shape_at_real_trunk = 0;
        i64 first_tries       = 0;
        i64 first_tries_exact = 0;
        std::map<std::string, std::pair<i64, i64>> first_by_feature;
        i64 their_blocks   = 0;
        i64 our_blocks     = 0;
        i64 agreed_blocks  = 0;
        std::map<std::string, i64> wrong_shape_by_feature;
        std::map<std::string, std::pair<i64, i64>> shape_at_trunk_by_feature;
        std::map<std::string, i64> missing_by_feature;
        i32                        shown       = 0;
        usize                      done        = 0;
        std::map<i32, i64>         sweep_hits;
        std::map<i32, i64>         sweep_seen;

        for (const auto& [chunk_x, chunk_z] : usable) {
            if (done >= static_cast<usize>(options.chunks)) {
                break;
            }
            auto held = build_level(chunk_x, chunk_z);
            if (held == nullptr) {
                continue;
            }
            ReferenceLevel& level = *held;

            // What the game grew, over the whole neighbourhood: the middle
            // chunk is what is scored, the ring is what stops a tree that
            // leans over the border from reading as a difference.
            std::map<i64, registry::BlockStateId> their_wood;
            std::vector<BlockPos>                 their_bases;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (i32 y = level.min_y(); y <= level.max_y(); ++y) {
                        for (i32 z = 0; z < 16; ++z) {
                            for (i32 x = 0; x < 16; ++x) {
                                const i32  wx    = (chunk_x + dx) * 16 + x;
                                const i32  wz    = (chunk_z + dz) * 16 + z;
                                const auto state = level.block_at(wx, y, wz);
                                if (!is_log(state) && !is_leaf(state)) {
                                    continue;
                                }
                                their_wood.emplace(ReferenceLevel::pack(wx, y, wz), state);
                                if (dx != 0 || dz != 0 || !is_log(state)) {
                                    continue;
                                }
                                const auto below = level.block_at(wx, y - 1, wz);
                                if (!is_log(below) && is_dirt(below)) {
                                    their_bases.push_back({wx, y, wz});
                                }
                            }
                        }
                    }
                }
            }

            // Strip. Air, not the original terrain: a tree grew into air.
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (i32 y = level.min_y(); y <= level.max_y(); ++y) {
                        for (i32 z = 0; z < 16; ++z) {
                            for (i32 x = 0; x < 16; ++x) {
                                const i32  wx    = (chunk_x + dx) * 16 + x;
                                const i32  wz    = (chunk_z + dz) * 16 + z;
                                const auto state = level.block_at(wx, y, wz);
                                if (stripped.contains(pack->block_of(state).value())) {
                                    level.restore(wx, y, wz, air_state);
                                }
                            }
                        }
                    }
                }
            }

            // The index sweep, when one was asked for.
            //
            // A feature's seed is `decoration + index + 10000 * step`, and the
            // index is its rank in the shared per-step ordering. That ordering
            // was measured at the ores' step and only there. This asks the same
            // question at the vegetal step, with a far better probe than the
            // ores had: a trunk is one column, the world has a handful of them
            // per chunk, and the *first* position a pipeline produces is the
            // one whose generator state cannot have drifted.
            if (!options.calibrate.empty()) {
                const auto* feature = features->placed(options.calibrate);
                if (feature == nullptr) {
                    fmt::print("{} was not built; --missing says why\n", options.calibrate);
                    return 1;
                }
                std::set<std::pair<i32, i32>> trunk_columns;
                for (const BlockPos& base : their_bases) {
                    trunk_columns.emplace(base.x, base.z);
                }
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 origin_x = (chunk_x + dx) * 16;
                        const i32 origin_z = (chunk_z + dz) * 16;
                        if (dx != 0 || dz != 0) {
                            continue;
                        }
                        const auto kind = worldgen::configured_feature_random();
                        const i64  seed =
                            worldgen::decoration_seed(options.seed, origin_x, origin_z, kind);
                        for (i32 index = 0; index < options.span; ++index) {
                            worldgen::FeatureRandom random{
                                kind, worldgen::feature_seed(seed, index, options.step)};
                            worldgen::FeatureContext context;
                            context.blocks       = &*pack;
                            context.feature_name = feature->name;
                            context.biomes       = &decorator->biome_features();
                            bool first = true;
                            worldgen::expand(feature->placement, context, level, random,
                                             {origin_x, level.min_y(), origin_z},
                                             [&](BlockPos at) {
                                                 if (!first) {
                                                     return;
                                                 }
                                                 first = false;
                                                 sweep_seen[index] += 1;
                                                 if (trunk_columns.contains({at.x, at.z})) {
                                                     sweep_hits[index] += 1;
                                                 }
                                             });
                        }
                    }
                }
                ++done;
                continue;
            }

            // Our decoration, one attempt at a time. This walks the same order
            // Decorator::decorate does — nearby biomes, then each step's shared
            // index in ascending order — but marks the level before every
            // attempt so that each tree's blocks can be told from its
            // neighbour's.
            struct Attempt {
                std::string name;
                BlockPos    origin;
                /// Which try of this feature in this chunk. Zero is the only
                /// one whose generator state is known-good: the pipeline is
                /// consumed depth-first, so try one begins where try zero's
                /// feature left off, and a feature that draws the wrong number
                /// of times moves every later try.
                i32 ordinal{0};
            };
            std::vector<Attempt> attempts;

            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 cx = chunk_x + dx;
                    const i32 cz = chunk_z + dz;
                    const i32 origin_x = cx * 16;
                    const i32 origin_z = cz * 16;

                    std::set<std::string_view> nearby;
                    for (i32 nz = -1; nz <= 1; ++nz) {
                        for (i32 nx = -1; nx <= 1; ++nx) {
                            for (i32 y = level.min_y(); y <= level.max_y(); y += 4) {
                                for (i32 z = 0; z < 16; z += 4) {
                                    for (i32 x = 0; x < 16; x += 4) {
                                        nearby.insert(level.biome_at(origin_x + nx * 16 + x, y,
                                                                     origin_z + nz * 16 + z));
                                    }
                                }
                            }
                        }
                    }

                    const auto kind = worldgen::configured_feature_random();
                    const i64  seed =
                        worldgen::decoration_seed(options.seed, origin_x, origin_z, kind);
                    for (usize step = 0; step < worldgen::kDecorationStepCount; ++step) {
                        const auto which = static_cast<worldgen::DecorationStep>(step);
                        const auto order = decorator->order_at(which);
                        for (usize index = 0; index < order.size(); ++index) {
                            const std::string_view name = order[index];
                            const bool listed = std::ranges::any_of(
                                nearby, [&](std::string_view biome) {
                                    return decorator->biome_features().lists(biome, name);
                                });
                            if (!listed) {
                                continue;
                            }
                            const auto* feature = features->placed(name);
                            if (feature == nullptr) {
                                continue;
                            }
                            worldgen::FeatureRandom random{
                                kind, worldgen::feature_seed(seed, static_cast<i32>(index),
                                                             static_cast<i32>(step))};
                            worldgen::FeatureContext context;
                            context.blocks       = &*pack;
                            context.feature_name = feature->name;
                            context.biomes       = &decorator->biome_features();
                            i32 ordinal_in_chunk = 0;
                            worldgen::expand(
                                feature->placement, context, level, random,
                                {origin_x, level.min_y(), origin_z}, [&](BlockPos at) {
                                    level.set_group(static_cast<i32>(attempts.size()));
                                    attempts.push_back(
                                        {std::string(name), at, ordinal_in_chunk++});
                                    (void)feature->feature->place(context, level, random, at);
                                });
                        }
                    }
                }
            }

            // Our writes, grouped by attempt, keeping only wood.
            std::map<i32, std::map<i64, registry::BlockStateId>> ours;
            for (const auto& [group, where, state] : level.group_writes()) {
                if (!is_log(state) && !is_leaf(state)) {
                    continue;
                }
                ours[group][where] = state;
            }

            // Block-level agreement, middle chunk only.
            for (const auto& [where, state] : their_wood) {
                if (!inside_chunk(where, chunk_x, chunk_z)) {
                    continue;
                }
                ++their_blocks;
            }
            std::map<i64, registry::BlockStateId> our_wood;
            for (const auto& [group, cells] : ours) {
                (void)group;
                for (const auto& [where, state] : cells) {
                    our_wood[where] = state;
                }
            }
            for (const auto& [where, state] : our_wood) {
                if (!inside_chunk(where, chunk_x, chunk_z)) {
                    continue;
                }
                ++our_blocks;
                const auto theirs = their_wood.find(where);
                // Compared by *block*, not by state. The game runs one last
                // pass over a finished tree that rewrites every leaf's
                // `distance` from its nearest log; that pass draws nothing and
                // is not part of the shape, and comparing states would call
                // every correct leaf a difference.
                if (theirs != their_wood.end() &&
                    pack->block_of(theirs->second) == pack->block_of(state)) {
                    ++agreed_blocks;
                }
            }

            // Trunk bases: same rule on both sides.
            std::set<i64> their_base_keys;
            for (const BlockPos& base : their_bases) {
                their_base_keys.insert(ReferenceLevel::pack(base.x, base.y, base.z));
            }
            their_trunks += static_cast<i64>(their_bases.size());

            std::set<i64> our_base_keys;
            for (const auto& [where, state] : our_wood) {
                if (!is_log(state) || !inside_chunk(where, chunk_x, chunk_z)) {
                    continue;
                }
                const i32  x     = unpack_x(where);
                const i32  y     = unpack_y(where);
                const i32  z     = unpack_z(where);
                const auto below = level.block_at(x, y - 1, z);
                if (!is_log(below) && is_dirt(below)) {
                    our_base_keys.insert(where);
                }
            }
            our_trunks += static_cast<i64>(our_base_keys.size());
            for (const i64 key : their_base_keys) {
                if (our_base_keys.contains(key)) {
                    ++matched_trunks;
                }
            }

            // Shape, per attempt: every wood block we wrote must be the same
            // block the game has there, and the game must have nothing extra
            // inside the box we filled.
            for (const auto& [group, cells] : ours) {
                if (group < 0 || cells.empty()) {
                    continue;
                }
                const Attempt& attempt = attempts[static_cast<usize>(group)];
                if (!inside_chunk(ReferenceLevel::pack(attempt.origin.x, attempt.origin.y,
                                                       attempt.origin.z),
                                  chunk_x, chunk_z)) {
                    continue;
                }
                const bool has_log = std::ranges::any_of(
                    cells, [&](const auto& cell) { return is_log(cell.second); });
                if (!has_log) {
                    continue;
                }
                ++our_groups;

                // Whether this tree's own trunk landed where one of the
                // game's did. A wrong shape here is a wrong *placer*; a wrong
                // shape anywhere else may only be this tree standing somewhere
                // the game never grew one.
                bool at_a_real_trunk = false;
                for (const auto& [where, state] : cells) {
                    if (is_log(state) && their_base_keys.contains(where)) {
                        at_a_real_trunk = true;
                        break;
                    }
                }

                i32 low_x = std::numeric_limits<i32>::max();
                i32 low_y = low_x;
                i32 low_z = low_x;
                i32 high_x = std::numeric_limits<i32>::min();
                i32 high_y = high_x;
                i32 high_z = high_x;
                bool exact = true;
                for (const auto& [where, state] : cells) {
                    const i32 x = unpack_x(where);
                    const i32 y = unpack_y(where);
                    const i32 z = unpack_z(where);
                    low_x = std::min(low_x, x); high_x = std::max(high_x, x);
                    low_y = std::min(low_y, y); high_y = std::max(high_y, y);
                    low_z = std::min(low_z, z); high_z = std::max(high_z, z);
                    const auto theirs = their_wood.find(where);
                    if (theirs == their_wood.end() ||
                        pack->block_of(theirs->second) != pack->block_of(state)) {
                        exact = false;
                    }
                }
                if (exact) {
                    // And nothing of theirs left over inside the box we filled
                    // — except where another of our own trees produced it. Two
                    // canopies overlap constantly, and a neighbour's leaves
                    // reaching into this tree's box are not this tree's shape
                    // being wrong.
                    for (i32 y = low_y; y <= high_y && exact; ++y) {
                        for (i32 z = low_z; z <= high_z && exact; ++z) {
                            for (i32 x = low_x; x <= high_x && exact; ++x) {
                                const i64 where = ReferenceLevel::pack(x, y, z);
                                if (their_wood.contains(where) && !cells.contains(where) &&
                                    !our_wood.contains(where)) {
                                    exact = false;
                                }
                            }
                        }
                    }
                }
                if (at_a_real_trunk) {
                    ++groups_at_real_trunk;
                    shape_at_trunk_by_feature[attempt.name].second += 1;
                    if (exact) {
                        shape_at_trunk_by_feature[attempt.name].first += 1;
                    }
                }
                if (attempt.ordinal == 0) {
                    ++first_tries;
                    first_by_feature[attempt.name].second += 1;
                    if (exact) {
                        ++first_tries_exact;
                        first_by_feature[attempt.name].first += 1;
                    }
                }
                if (exact) {
                    ++same_shape;
                    if (at_a_real_trunk) {
                        ++same_shape_at_real_trunk;
                    }
                    continue;
                }
                wrong_shape_by_feature[attempt.name] += 1;
                if (at_a_real_trunk) {
                    ++wrong_shape_at_real_trunk;
                }
                // Whether every log we wrote is a log of theirs and every log
                // of theirs inside our box is one of ours. When that holds the
                // trunk placer is right and the difference is the foliage.
                bool logs_agree = true;
                for (const auto& [where, state] : cells) {
                    if (!is_log(state)) {
                        continue;
                    }
                    const auto theirs = their_wood.find(where);
                    if (theirs == their_wood.end() || !is_log(theirs->second)) {
                        logs_agree = false;
                        break;
                    }
                }
                for (i32 y = low_y; y <= high_y && logs_agree; ++y) {
                    for (i32 z = low_z; z <= high_z && logs_agree; ++z) {
                        for (i32 x = low_x; x <= high_x && logs_agree; ++x) {
                            const i64  where  = ReferenceLevel::pack(x, y, z);
                            const auto theirs = their_wood.find(where);
                            if (theirs == their_wood.end() || !is_log(theirs->second)) {
                                continue;
                            }
                            const auto our = cells.find(where);
                            if (our == cells.end() || !is_log(our->second)) {
                                logs_agree = false;
                            }
                        }
                    }
                }
                i32 log_count = 0;
                for (const auto& [where, state] : cells) {
                    (void)where;
                    log_count += is_log(state) ? 1 : 0;
                }
                if (shown < options.show && (!options.first || attempt.ordinal == 0) &&
                    (!options.matched || at_a_real_trunk) &&
                    (!options.logs_exact || logs_agree) && log_count >= options.min_logs &&
                    (options.only.empty() ||
                     attempt.name.find(options.only) != std::string::npos)) {
                    ++shown;
                    fmt::print("\n{} at {},{},{} — ours then theirs, layer by layer\n",
                               attempt.name, attempt.origin.x, attempt.origin.y,
                               attempt.origin.z);
                    for (i32 y = high_y; y >= low_y; --y) {
                        fmt::print("  y {:>4}\n", y);
                        for (i32 z = low_z; z <= high_z; ++z) {
                            std::string mine;
                            std::string theirs;
                            for (i32 x = low_x; x <= high_x; ++x) {
                                const i64  where = ReferenceLevel::pack(x, y, z);
                                const auto our   = cells.find(where);
                                mine += our == cells.end() ? '.'
                                        : is_log(our->second) ? '#'
                                                              : 'o';
                                const auto them = their_wood.find(where);
                                theirs += them == their_wood.end() ? '.'
                                          : is_log(them->second)   ? '#'
                                                                   : 'o';
                            }
                            fmt::print("    {}   {}\n", mine, theirs);
                        }
                    }
                }
            }

            // Their trunks that nothing of ours reached, by the feature that
            // should have made them — read off the biome rather than guessed.
            for (const i64 key : their_base_keys) {
                if (!our_base_keys.contains(key)) {
                    missing_by_feature[std::string(
                        level.biome_at(unpack_x(key), unpack_y(key), unpack_z(key)))] += 1;
                }
            }
            ++done;
        }

        if (!options.calibrate.empty()) {
            fmt::print("\n{} at step {}, {} chunks: the first position the pipeline makes,\n"
                       "against the columns the game actually grew a trunk in\n",
                       options.calibrate, options.step, done);
            fmt::print("{:>6} {:>8} {:>8}\n", "index", "tries", "on a trunk");
            i32 best  = -1;
            i64 top   = -1;
            for (const auto& [index, seen] : sweep_seen) {
                const i64 hits = sweep_hits.contains(index) ? sweep_hits.at(index) : 0;
                if (hits > top) {
                    top  = hits;
                    best = index;
                }
                if (hits > 0) {
                    fmt::print("{:>6} {:>8} {:>8}\n", index, seen, hits);
                }
            }
            fmt::print("\nbest index {} with {} hits; our sorter says {}\n", best, top,
                       decorator->index_of(static_cast<worldgen::DecorationStep>(options.step),
                                           options.calibrate));
            return 0;
        }

        fmt::print("\nseed {}, {} chunks compared\n", options.seed, done);
        fmt::print("{} of {} configured features built, {} placed features\n",
                   features->configured_count(), 194, features->placed_count());
        fmt::print("\ntrunk bases in the game's world   {}\n", their_trunks);
        fmt::print("trunk bases in ours                {}\n", our_trunks);
        fmt::print("in the same place                  {}  ({:.3f} %)\n", matched_trunks,
                   their_trunks == 0 ? 0.0
                                     : 100.0 * static_cast<f64>(matched_trunks) /
                                           static_cast<f64>(their_trunks));
        fmt::print("\ntrees we grew (attempts with a log) {}\n", our_groups);
        fmt::print("of those, identical shape           {}  ({:.3f} %)\n", same_shape,
                   our_groups == 0 ? 0.0
                                   : 100.0 * static_cast<f64>(same_shape) /
                                         static_cast<f64>(our_groups));
        fmt::print("\nof the trees standing on one of the game's trunks   {}\n",
                   groups_at_real_trunk);
        fmt::print("of those, identical shape                          {}  ({:.3f} %)\n",
                   same_shape_at_real_trunk,
                   groups_at_real_trunk == 0
                       ? 0.0
                       : 100.0 * static_cast<f64>(same_shape_at_real_trunk) /
                             static_cast<f64>(groups_at_real_trunk));
        (void)wrong_shape_at_real_trunk;
        fmt::print("\nlog and leaf blocks: theirs {}, ours {}, same block {}  ({:.3f} %)\n",
                   their_blocks, our_blocks, agreed_blocks,
                   their_blocks == 0 ? 0.0
                                     : 100.0 * static_cast<f64>(agreed_blocks) /
                                           static_cast<f64>(their_blocks));

        fmt::print(
            "\nfirst try of a feature in its chunk — the one whose generator state is\n"
            "known-good, so a wrong shape here is a wrong placer and nothing else:\n");
        fmt::print("  {} of {} identical  ({:.3f} %)\n", first_tries_exact, first_tries,
                   first_tries == 0 ? 0.0
                                    : 100.0 * static_cast<f64>(first_tries_exact) /
                                          static_cast<f64>(first_tries));
        for (const auto& [name, counts] : first_by_feature) {
            fmt::print("  {:<44} {:>4} / {:<4}\n", name, counts.first, counts.second);
        }
        if (!shape_at_trunk_by_feature.empty()) {
            fmt::print("\nshape, among the trees standing on one of the game's trunks:\n");
            for (const auto& [name, counts] : shape_at_trunk_by_feature) {
                fmt::print("  {:<44} {:>4} / {:<4}  {:.1f} %\n", name, counts.first,
                           counts.second,
                           counts.second == 0 ? 0.0
                                              : 100.0 * static_cast<f64>(counts.first) /
                                                    static_cast<f64>(counts.second));
            }
        }
        if (!wrong_shape_by_feature.empty()) {
            fmt::print("\nwrong shape, by the placed feature that grew it:\n");
            std::vector<std::pair<std::string, i64>> ranked(wrong_shape_by_feature.begin(),
                                                            wrong_shape_by_feature.end());
            std::ranges::sort(ranked,
                              [](const auto& a, const auto& b) { return a.second > b.second; });
            for (const auto& [name, count] : ranked) {
                fmt::print("  {:<44} {:>7}\n", name, count);
            }
        }
        if (!missing_by_feature.empty()) {
            fmt::print("\ntheir trunks with nothing of ours there, by biome:\n");
            std::vector<std::pair<std::string, i64>> ranked(missing_by_feature.begin(),
                                                            missing_by_feature.end());
            std::ranges::sort(ranked,
                              [](const auto& a, const auto& b) { return a.second > b.second; });
            for (const auto& [name, count] : ranked) {
                fmt::print("  {:<44} {:>7}\n", name, count);
            }
        }
        return 0;
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
    /// For every ore of theirs we did not reproduce, the block our replay holds
    /// at that position.
    std::map<std::string, i64>   blocked_by;
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
        // Every write, ore or not. The ore map answers "did we place this
        // one"; the whole map answers "what did we do there instead", and a
        // shortfall needs the second question.
        std::map<i64, std::string> ours_all;
        for (const auto& [where, state] : level.written()) {
            const auto name = pack->block_name(pack->block_of(state));
            ours_all[where] = std::string(name);
            if (host_of(name).empty()) {
                continue;
            }
            ours[where] = std::string(name);
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
                // What *we wrote* there instead, from our own writes and not
                // from the level — asking the level would answer with the
                // reference world's own block and say nothing at all. A
                // shortfall is only a number until this separates "another of
                // our veins got there first" from "no vein of ours reached it".
                const auto instead = ours_all.find(where);
                blocked_by[instead == ours_all.end() ? "(nothing of ours reached it)"
                                                     : instead->second] += 1;
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

    if (!blocked_by.empty()) {
        fmt::print("\nwhat our replay holds where one of their ores is missing:\n");
        std::vector<std::pair<std::string, i64>> ranked(blocked_by.begin(), blocked_by.end());
        std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.second > b.second; });
        i64 shown = 0;
        for (const auto& [name, count] : ranked) {
            if (shown++ >= 12) {
                break;
            }
            fmt::print("  {:<38} {:>8}\n", name, count);
        }
    }

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
