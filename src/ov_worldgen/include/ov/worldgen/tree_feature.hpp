// `tree`: the feature the overworld is made of, and the longest single run of
// random draws anywhere in worldgen.
//
// A tree is not one algorithm but six that share a generator, in a fixed order
// that nothing in the output reveals:
//
//   1. the **trunk placer** says how tall (`getTreeHeight`);
//   2. the **foliage placer** says how much of that height is leaves
//      (`foliageHeight`) and how wide (`foliageRadius`);
//   3. the **root placer**, when there is one, moves the trunk's origin and
//      then grows roots down from it;
//   4. the trunk placer builds the trunk and hands back a list of
//      *attachments* — the points where foliage hangs;
//   5. the foliage placer runs once per attachment;
//   6. the **decorators** run last, over the sets of logs and leaves that came
//      out.
//
// Every one of those draws, and a single draw out of place moves the whole
// tree. Three traps are paid for in this file:
//
//   * `random.nextInt(a + 1) + random.nextInt(b + 1)` is two draws whose order
//     Java fixes and C++ does not. Every such expression here is split into
//     named locals, in order. `nextInt(1)` still draws.
//   * the fancy trunk uses `Math.sin`/`Math.cos` on doubles, while the mega
//     jungle trunk uses `Mth.sin`/`Mth.cos` — the 65536-entry float table. They
//     are not interchangeable, and using libm for the second bends every
//     branch of a jungle giant by a block.
//   * the decorators are handed the log and leaf positions *in the iteration
//     order of a `java.util.HashSet`*, stably sorted by y. That order is part
//     of the seed: a beehive is placed on a shuffled list built from it. See
//     `java_hash_order` below.
#pragma once

#include "ov/worldgen/feature.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// The block sets a tree asks the world about.
///
/// Resolved from the block tags once, at load, into sorted id lists: the
/// questions ("is this free?", "is this dirt?") are asked tens of thousands of
/// times per chunk and each one would otherwise be a string comparison.
struct TreeTags {
    /// `#minecraft:logs`
    std::vector<u16> logs;
    /// `#minecraft:leaves`
    std::vector<u16> leaves;
    /// `#minecraft:dirt`
    std::vector<u16> dirt;
    /// `#minecraft:replaceable_by_trees` — which *contains* the leaves tag, so
    /// a tree may grow through another tree's canopy but not through its
    /// trunk.
    std::vector<u16> replaceable_by_trees;

    registry::BlockId vine{0};
    registry::BlockId water{0};
    registry::BlockId air{0};

    [[nodiscard]] static bool holds(const std::vector<u16>& set, registry::BlockId block) noexcept;
};

using TreeTagsRef = std::shared_ptr<const TreeTags>;

/// The world as a tree sees it: the level, the block registry, and the tags.
///
/// A struct rather than three parameters because it is threaded through every
/// placer and decorator, and because the predicates below belong with it.
struct TreeWorld {
    const registry::BlockRegistry* blocks{nullptr};
    const TreeTags*                tags{nullptr};
    const FeatureLevel*            level{nullptr};

    [[nodiscard]] registry::BlockId block_at(BlockPos at) const;

    /// Air or `#replaceable_by_trees`. What a log or a leaf may be written on.
    [[nodiscard]] bool valid_tree_pos(BlockPos at) const;
    /// `valid_tree_pos` or a log. What counts as clearance for the trunk.
    [[nodiscard]] bool is_free(BlockPos at) const;
    [[nodiscard]] bool is_air_or_leaves(BlockPos at) const;
    [[nodiscard]] bool is_dirt(BlockPos at) const;
    [[nodiscard]] bool is_air(BlockPos at) const;
    [[nodiscard]] bool is_vine(BlockPos at) const;
    /// A water *source*: a still water block, or a waterlogged one.
    [[nodiscard]] bool is_water_source(BlockPos at) const;
};

/// Where foliage hangs off the trunk, and how much wider it is there.
///
/// `double_trunk` is the two-by-two flag: it widens the leaf rows and changes
/// how the skip test treats a coordinate, which is why a giant spruce is not
/// simply a tall one.
struct FoliageAttachment {
    BlockPos pos{};
    i32      radius_offset{0};
    bool     double_trunk{false};
};

/// The four sets a tree fills as it is built, in `java.util.HashSet` order.
///
/// Not a detail. `TreeDecorator.Context` sorts the log and leaf sets by y with
/// a *stable* sort, so what survives is the hash set's own iteration order
/// within each y — and the decorators draw once per element of those lists.
class TreeWriter {
public:
    TreeWriter(FeatureLevel& level, const registry::BlockRegistry& blocks);

    void set_root(BlockPos at, registry::BlockStateId state);
    void set_log(BlockPos at, registry::BlockStateId state);
    void set_foliage(BlockPos at, registry::BlockStateId state);
    void set_decoration(BlockPos at, registry::BlockStateId state);

    /// Whether a leaf has already been written here by this tree. The cherry
    /// foliage placer's hanging leaves ask it.
    [[nodiscard]] bool foliage_set(BlockPos at) const;

    [[nodiscard]] bool empty() const;

    /// The logs, the leaves and the roots as a decorator receives them: the
    /// hash set's order, stably sorted by ascending y.
    [[nodiscard]] std::vector<BlockPos> sorted_logs() const;
    [[nodiscard]] std::vector<BlockPos> sorted_foliage() const;
    [[nodiscard]] std::vector<BlockPos> sorted_roots() const;

    [[nodiscard]] FeatureLevel& level() const noexcept { return *level_; }

private:
    struct Set;

    FeatureLevel*                  level_;
    const registry::BlockRegistry* blocks_;
    std::shared_ptr<Set>           roots_;
    std::shared_ptr<Set>           logs_;
    std::shared_ptr<Set>           foliage_;
    std::shared_ptr<Set>           decorations_;
};

/// How wide the trunk's clearance check is at each height.
class FeatureSize {
public:
    FeatureSize()                              = default;
    FeatureSize(const FeatureSize&)            = delete;
    FeatureSize& operator=(const FeatureSize&) = delete;
    virtual ~FeatureSize();

    [[nodiscard]] virtual i32 size_at_height(i32 height, i32 y) const = 0;

    /// How short a clipped tree may be and still be built. Absent means "no
    /// shorter than asked", which is what most trees say.
    [[nodiscard]] virtual std::optional<i32> min_clipped_height() const = 0;
};

using FeatureSizeRef = std::shared_ptr<const FeatureSize>;

struct TreeConfig;

/// Builds the trunk and says where the leaves hang.
class TrunkPlacer {
public:
    TrunkPlacer(i32 base_height, i32 height_rand_a, i32 height_rand_b)
        : base_height_(base_height), height_rand_a_(height_rand_a), height_rand_b_(height_rand_b) {}
    TrunkPlacer(const TrunkPlacer&)            = delete;
    TrunkPlacer& operator=(const TrunkPlacer&) = delete;
    virtual ~TrunkPlacer();

    /// `base + nextInt(a + 1) + nextInt(b + 1)`, two draws in that order.
    ///
    /// Both are always taken, `nextInt(1)` included: a bound of one still
    /// advances the generator, and skipping it as "obviously zero" moves every
    /// later draw of the tree.
    [[nodiscard]] virtual i32 tree_height(FeatureRandom& random) const;

    virtual void place_trunk(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                             i32 free_height, BlockPos at, const TreeConfig& config,
                             std::vector<FoliageAttachment>& out) const = 0;

    [[nodiscard]] virtual std::string_view type_name() const = 0;

protected:
    i32 base_height_;
    i32 height_rand_a_;
    i32 height_rand_b_;
};

using TrunkPlacerRef = std::shared_ptr<const TrunkPlacer>;

/// Grows the leaves around one attachment.
class FoliagePlacer {
public:
    FoliagePlacer(IntProviderRef radius, IntProviderRef offset)
        : radius_(std::move(radius)), offset_(std::move(offset)) {}
    FoliagePlacer(const FoliagePlacer&)            = delete;
    FoliagePlacer& operator=(const FoliagePlacer&) = delete;
    virtual ~FoliagePlacer();

    /// The public entry point: draws the offset, then defers to `grow`.
    void create_foliage(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                        const TreeConfig& config, i32 free_height,
                        const FoliageAttachment& attachment, i32 foliage_height,
                        i32 foliage_radius) const;

    [[nodiscard]] virtual i32 foliage_height(FeatureRandom& random, i32 height,
                                             const TreeConfig& config) const = 0;

    [[nodiscard]] virtual i32 foliage_radius(FeatureRandom& random, i32 height) const;

    [[nodiscard]] virtual std::string_view type_name() const = 0;

protected:
    virtual void grow(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                      const TreeConfig& config, i32 free_height,
                      const FoliageAttachment& attachment, i32 foliage_height, i32 foliage_radius,
                      i32 offset) const = 0;

    /// Whether to leave this cell of a leaf row empty. May draw — the blob
    /// placer's rounded corner is one `nextInt(2)` per corner cell.
    [[nodiscard]] virtual bool should_skip(FeatureRandom& random, i32 local_x, i32 local_y,
                                           i32 local_z, i32 range, bool large) const = 0;

    /// The signed form, which folds a coordinate onto the positive quadrant —
    /// and for a two-by-two trunk folds it around the *pair* of centre columns.
    [[nodiscard]] virtual bool should_skip_signed(FeatureRandom& random, i32 local_x, i32 local_y,
                                                  i32 local_z, i32 range, bool large) const;

    void place_leaves_row(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                          const TreeConfig& config, BlockPos at, i32 range, i32 local_y,
                          bool large) const;

    static void try_place_leaf(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                               const TreeConfig& config, BlockPos at);

    IntProviderRef radius_;
    IntProviderRef offset_;
};

using FoliagePlacerRef = std::shared_ptr<const FoliagePlacer>;

/// Moves the trunk's origin and grows roots below it. Only the mangrove has
/// one.
class RootPlacer {
public:
    RootPlacer()                             = default;
    RootPlacer(const RootPlacer&)            = delete;
    RootPlacer& operator=(const RootPlacer&) = delete;
    virtual ~RootPlacer();

    /// Where the trunk actually starts, given where the feature was placed.
    /// Draws.
    [[nodiscard]] virtual BlockPos trunk_origin(BlockPos at, FeatureRandom& random) const = 0;

    [[nodiscard]] virtual bool place_roots(const TreeWorld& world, TreeWriter& writer,
                                           FeatureRandom& random, BlockPos at,
                                           BlockPos          trunk_origin,
                                           const TreeConfig& config) const = 0;

    [[nodiscard]] virtual std::string_view type_name() const = 0;
};

using RootPlacerRef = std::shared_ptr<const RootPlacer>;

/// Runs after the tree is built, over the logs and leaves it produced.
class TreeDecorator {
public:
    TreeDecorator()                                = default;
    TreeDecorator(const TreeDecorator&)            = delete;
    TreeDecorator& operator=(const TreeDecorator&) = delete;
    virtual ~TreeDecorator();

    virtual void decorate(const TreeWorld& world, TreeWriter& writer, FeatureRandom& random,
                          const std::vector<BlockPos>& logs, const std::vector<BlockPos>& leaves,
                          const std::vector<BlockPos>& roots) const = 0;

    [[nodiscard]] virtual std::string_view type_name() const = 0;
};

using TreeDecoratorRef = std::shared_ptr<const TreeDecorator>;

/// Everything a `tree` feature is configured with.
struct TreeConfig {
    StateProviderRef trunk_provider;
    StateProviderRef foliage_provider;
    StateProviderRef dirt_provider;

    TrunkPlacerRef   trunk_placer;
    FoliagePlacerRef foliage_placer;
    RootPlacerRef    root_placer;  ///< null when there is none
    FeatureSizeRef   minimum_size;

    std::vector<TreeDecoratorRef> decorators;

    bool ignore_vines{false};
    bool force_dirt{false};

    TreeTagsRef tags;
};

/// The tree feature itself.
[[nodiscard]] FeatureRef make_tree_feature(TreeConfig config);

/// The block sets a tree needs, resolved out of the loaded block tags.
[[nodiscard]] std::expected<TreeTagsRef, FeatureError> load_tree_tags(
    const registry::BlockRegistry& blocks, const BlockTags& tags);

/// The order `java.util.HashSet<BlockPos>` iterates a set of positions in.
///
/// Exposed, and tested, because it is not an implementation detail: the
/// decorators consume it directly and a different order gives a different
/// world. Reproduces `HashMap`'s bucket walk — `hash = h ^ (h >>> 16)`, index
/// `hash & (capacity - 1)`, insertion order within a bucket, and the
/// order-preserving lo/hi split on every resize.
[[nodiscard]] std::vector<BlockPos> java_hash_order(const std::vector<BlockPos>& inserted);

/// ── worldgen-3 ── How many bins `java_hash_order` has turned into trees in
/// this process — a diagnostic count, for the measurement that has to show the
/// treeified order is actually exercised. Nothing reads it back.
[[nodiscard]] u64 java_hash_treeified_bins() noexcept;

/// `Vec3i.hashCode()`: `(y + z * 31) * 31 + x`, wrapping like a Java int.
[[nodiscard]] i32 java_block_pos_hash(BlockPos at) noexcept;

}  // namespace ov::worldgen
