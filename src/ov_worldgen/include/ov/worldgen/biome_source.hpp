// Which biome a place is, from six climate values.
//
// There is no map and no seeded diagram. The world has six climate noises —
// temperature, humidity, continentalness, erosion, depth and weirdness — and a
// table of about seven and a half thousand boxes in that six-dimensional space,
// each labelled with a biome. A place gets the biome whose box is nearest.
//
// The table is *not* in the datapack: `multi_noise_biome_source_parameter_list`
// only names a preset, and the preset is Java. It is however exported by the
// official data generator into `reports/biome_parameters`, which is where this
// reads it — generated locally like every other piece of vanilla data, and
// committed nowhere.
//
// The nearest box is found through the same R-tree vanilla builds over those
// boxes. That used to be a plain scan here, on the argument that a tree search
// returns the true minimum and so has to agree with a scan. It does agree — on
// the *distance*. It does not agree on *which* box, because the search only
// replaces its running best when a candidate is strictly nearer, so a tie goes
// to whichever box the traversal reaches first, and the traversal order is the
// tree's rather than the file's. The scan measured 7819115 of 7821312 biome
// cells right against the real game at seed 1234567890, and every one of the
// 2197 that were left was an exact tie.
//
// The search also carries a one-entry cache: the box the previous query
// returned is tried first, and being first it wins any tie it takes part in.
// So the answer depends on the order the queries are asked in. That order is
// the caller's business and not this class's, so the cache is a value the
// caller owns (`BiomeSearchCache`) rather than hidden state — which also keeps
// `biome_at` callable from several threads at once.
#pragma once

#include "ov/base/types.hpp"
#include "ov/worldgen/density.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <array>
#include <vector>

namespace ov::worldgen {

/// A climate reading, in the fixed-point form the comparison uses.
///
/// Quantised by ten thousand and held as integers, exactly as the game does:
/// the distance between two boxes is an integer sum of squares, so there is no
/// rounding anywhere in the search and two builds cannot disagree about a
/// boundary.
struct ClimatePoint {
    std::array<i64, 7> coordinates{};
};

/// The one-entry cache vanilla's climate search keeps.
///
/// Vanilla holds it in a thread local inside the tree, which makes the biome a
/// place gets depend on what the same thread asked about before it. Held here
/// as a plain value so that dependence is written down instead of hidden, and
/// so two threads cannot silently share one.
struct BiomeSearchCache {
    /// Index of the box the previous query returned, or -1 for none.
    i32 last{-1};

    void clear() noexcept { last = -1; }
};

class BiomeSource {
public:
    /// Read the exported parameter table.
    [[nodiscard]] static std::expected<BiomeSource, DensityError> load(
        const std::filesystem::path& data_root, std::string_view dimension);

    /// The climate at a *quart* position — the 4x4x4 cell biomes are stored
    /// in, not a block position.
    [[nodiscard]] ClimatePoint sample(const NoiseRouter& router, i32 quart_x, i32 quart_y,
                                      i32 quart_z) const;

    /// The biome whose box is nearest, by name.
    ///
    /// Without a cache this is a pure function of the climate: the tie goes to
    /// whichever box the tree reaches first.
    [[nodiscard]] std::string_view biome_at(const ClimatePoint& climate) const;

    /// The same, remembering the answer so that the next query tries it first.
    ///
    /// The cache is not an optimisation that happens to be visible; it decides
    /// ties, so a caller that wants the game's answers has to ask its
    /// questions in the game's order.
    [[nodiscard]] std::string_view biome_at(const ClimatePoint& climate,
                                            BiomeSearchCache&   cache) const;

    /// The index of the nearest box, and -1 when the table is empty. The index
    /// rather than the name, because a cache is a box and not a biome.
    [[nodiscard]] i32 entry_at(const ClimatePoint& climate, BiomeSearchCache& cache) const;

    /// How far a climate is from the nearest box of a named biome, per axis.
    ///
    /// The whole point of a parity harness is to say *which* axis is wrong, not
    /// merely that something is. When the game names a biome we did not, the
    /// axis carrying the distance to that biome's nearest box is the one to
    /// look at.
    [[nodiscard]] std::array<i64, 7> gap_to(const ClimatePoint& climate,
                                            std::string_view    biome) const;

    /// The squared distance to the nearest box of a named biome. Equal
    /// distances mean a tie, which is decided by the order of the table rather
    /// than by the climate — a different kind of disagreement from a numeric
    /// one, and worth telling apart.
    [[nodiscard]] i64 distance_to(const ClimatePoint& climate, std::string_view biome) const;

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }

    /// The biome one box belongs to.
    [[nodiscard]] std::string_view entry_biome(usize index) const;

    /// The middle of one box.
    ///
    /// Exposed so a test can ask the question that matters: is every biome in
    /// the table actually *reachable*? A box that another box swallows would
    /// name a biome the world can never contain, and nothing about the
    /// generated terrain would reveal it.
    [[nodiscard]] ClimatePoint entry_centre(usize index) const;

    /// Every distinct biome, sorted.
    [[nodiscard]] std::vector<std::string_view> biomes() const;
    [[nodiscard]] usize biome_count() const noexcept;

    /// Quantise a climate value the way the game does.
    [[nodiscard]] static i64 quantise(f32 value) noexcept {
        return static_cast<i64>(value * 10000.0F);
    }

private:
    struct Range {
        i64 low{0};
        i64 high{0};

        /// Zero inside the range, and the gap to the nearer edge outside it.
        [[nodiscard]] i64 distance(i64 value) const noexcept {
            const i64 above = value - high;
            const i64 below = low - value;
            return above > 0 ? above : (below > 0 ? below : 0);
        }
    };

    struct Entry {
        std::array<Range, 7> box{};
        std::string          biome;
    };

    /// One node of the search tree, flattened.
    ///
    /// A leaf names an entry; a branch names a contiguous run of children. The
    /// tree is built once and never changes, so a flat array beats a graph of
    /// pointers both for cache behaviour and for the guarantee that nothing
    /// here allocates during a query.
    struct Node {
        std::array<Range, 7> box{};
        /// The entry this leaf stands for, or -1 for a branch.
        i32 entry{-1};
        u32 first_child{0};
        u32 child_count{0};
    };

    /// The node a search should start from, and -1 when the tree is empty.
    [[nodiscard]] i32 search(const ClimatePoint& climate, i32 start) const;

    /// The same by a plain scan of the table, ignoring the tree.
    ///
    /// Kept, and reachable through OV_BIOME_SCAN=1, for the same reason
    /// density.cpp keeps OV_NO_INTERPOLATION: the claim that the tree only
    /// changes ties is a claim about the real world, and one run of the parity
    /// harness with the flag either way settles it instead of arguing it.
    [[nodiscard]] i32 scan(const ClimatePoint& climate) const;

    /// Build the tree over `entries_`. Called once, by load().
    void build_tree();

    std::vector<Entry> entries_;
    std::vector<Node>  nodes_;
    /// OV_BIOME_SCAN, read once at load rather than per query.
    bool scan_only_{false};
    /// Index of the root in `nodes_`, or -1 when the table is empty.
    i32 root_{-1};
};

}  // namespace ov::worldgen
