// Where a feature is allowed to happen, read out of the datapack.
//
// A placed feature is a configured feature plus a *pipeline* of modifiers, and
// the pipeline is the whole of the decision: `count` says how many attempts,
// `in_square` scatters them across the chunk, `height_range` picks an
// elevation from a distribution, `biome` throws away the attempts that landed
// somewhere the feature is not listed. Nothing here knows what a tree or an
// ore is.
//
// Two things about it are easy to get wrong and invisible when you do.
//
// The first is that the pipeline is consumed lazily, one position at a time,
// with the feature placed as each position falls out of the far end. So the
// draws go: count, then for the *first* position its offset, its height, and
// every draw the feature itself makes, and only then the second position's
// offset. Running the modifiers stage by stage over a whole batch consumes the
// same number of draws in a different order and puts every ore somewhere else,
// with no error anywhere. `expand()` below is depth-first for that reason and
// no other.
//
// The second is that a modifier this code does not implement has to stop the
// placed feature from loading, by name. Treating it as a no-op would place the
// feature anyway — everywhere, at any height, in any biome — and the world
// would look entirely reasonable.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/heightmap.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ov::worldgen {

enum class FeatureError : u8 {
    /// A file a feature or a placement names is not there.
    Missing,
    /// A file is not JSON, or not the shape the thing it names has.
    Malformed,
    /// A type this interpreter does not implement. Named loudly and refused,
    /// never treated as a no-op: a placement modifier silently skipped places
    /// its feature everywhere it was meant to be kept out of.
    Unsupported,
};

[[nodiscard]] std::string_view to_string(FeatureError error) noexcept;

/// The world a feature reads and writes while it is being placed.
///
/// Deliberately not `world::Chunk`. Decoration writes across chunk borders — an
/// ore vein that starts in the last column of a chunk finishes in the next one
/// — so what a feature is handed has to be wider than a chunk. It is also what
/// lets the same feature code run against a chunk the *game* generated, which
/// is the only way to tell a placement bug apart from a terrain bug.
class FeatureLevel {
public:
    FeatureLevel()                               = default;
    FeatureLevel(const FeatureLevel&)            = delete;
    FeatureLevel& operator=(const FeatureLevel&) = delete;
    virtual ~FeatureLevel();

    [[nodiscard]] virtual registry::BlockStateId block_at(i32 x, i32 y, i32 z) const = 0;

    /// Write a block. False when the position is outside what this level
    /// covers — which is not an error: a feature may reach past the edge of
    /// the region being generated, and the part that does is dropped, exactly
    /// as it is when the game comes to generate the neighbour.
    virtual bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) = 0;

    /// The heightmap value: one above the highest block that counts.
    [[nodiscard]] virtual i32 height(world::HeightmapType type, i32 x, i32 z) const = 0;

    /// The biome at a *block* position, by name.
    [[nodiscard]] virtual std::string_view biome_at(i32 x, i32 y, i32 z) const = 0;

    [[nodiscard]] virtual i32 min_y() const        = 0;
    [[nodiscard]] virtual i32 world_height() const = 0;
    [[nodiscard]] virtual i32 sea_level() const    = 0;

    [[nodiscard]] i32 max_y() const { return min_y() + world_height() - 1; }

    [[nodiscard]] bool outside_build_height(i32 y) const { return y < min_y() || y > max_y(); }
};

/// The generator a feature draws from.
///
/// Either family, chosen at construction. That is not future-proofing: the
/// noise settings carry a `legacy_random_source` flag, so a datapack really can
/// ask for the old generator, and the two are not interchangeable — they
/// consume different amounts of state and their bounded draws use different
/// algorithms, so the same seed gives two different worlds. Which one the
/// features use is a fact about the game that has to be measured rather than
/// assumed, and this is what makes measuring it one flag instead of a rewrite.
class FeatureRandom {
public:
    enum class Kind : u8 { Xoroshiro, Legacy };

    FeatureRandom(Kind kind, i64 seed) noexcept
        : kind_(kind), xoroshiro_(seed), legacy_(seed) {}

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    [[nodiscard]] i64 next_long() noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_long() : legacy_.next_long();
    }
    [[nodiscard]] i32 next_int() noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_int() : legacy_.next_int();
    }
    [[nodiscard]] i32 next_int(i32 bound) noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_int(bound) : legacy_.next_int(bound);
    }
    [[nodiscard]] bool next_boolean() noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_boolean() : legacy_.next_boolean();
    }
    [[nodiscard]] f32 next_float() noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_float() : legacy_.next_float();
    }
    [[nodiscard]] f64 next_double() noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_double() : legacy_.next_double();
    }
    [[nodiscard]] f64 next_gaussian() noexcept {
        return kind_ == Kind::Xoroshiro ? xoroshiro_.next_gaussian() : legacy_.next_gaussian();
    }

private:
    Kind                        kind_;
    math::XoroshiroRandomSource xoroshiro_;
    math::LegacyRandomSource    legacy_;
};

/// Which family the features use, from OV_FEATURE_RANDOM.
///
/// Defaults to the one the measurement in docs/provenance/features.md settled
/// on; the other is one environment variable away, so the claim stays checkable
/// rather than becoming folklore.
[[nodiscard]] FeatureRandom::Kind configured_feature_random() noexcept;

/// A height measured from the bottom of the world, from the top of it, or from
/// nothing.
///
/// `above_bottom` and `below_top` are not conveniences: they are what makes a
/// datapack's heights follow a world whose floor moved, and reading one as an
/// absolute puts every deep ore sixty-four blocks out.
struct VerticalAnchor {
    enum class Kind : u8 { Absolute, AboveBottom, BelowTop };

    Kind kind{Kind::Absolute};
    i32  value{0};

    [[nodiscard]] i32 resolve(i32 min_y, i32 world_height) const noexcept;
};

/// A number drawn from a distribution, as the datapack spells one out.
class IntProvider {
public:
    IntProvider()                              = default;
    IntProvider(const IntProvider&)            = delete;
    IntProvider& operator=(const IntProvider&) = delete;
    virtual ~IntProvider();

    [[nodiscard]] virtual i32 sample(FeatureRandom& random) const = 0;
};

using IntProviderRef = std::shared_ptr<const IntProvider>;

/// A y drawn from a distribution between two anchors.
class HeightProvider {
public:
    HeightProvider()                                 = default;
    HeightProvider(const HeightProvider&)            = delete;
    HeightProvider& operator=(const HeightProvider&) = delete;
    virtual ~HeightProvider();

    [[nodiscard]] virtual i32 sample(FeatureRandom& random, i32 min_y,
                                     i32 world_height) const = 0;
};

using HeightProviderRef = std::shared_ptr<const HeightProvider>;

/// The block tags a rule test or a predicate names, resolved to block ids.
///
/// Not in ov_registry, because they are not needed to talk to a client: these
/// exist so that worldgen can say "anything stone-like", and they come out of
/// the same generated data as everything else.
class BlockTags {
public:
    /// Read `tags/blocks/` under the generated data root, and resolve the
    /// `#other_tag` references between them.
    [[nodiscard]] static std::expected<BlockTags, FeatureError> load(
        const std::filesystem::path& data_root, const registry::BlockRegistry& blocks);

    /// Whether a block is in a tag.
    ///
    /// An unknown tag is not silently empty — `known()` answers that
    /// separately, and a rule test over an unknown tag is refused when it is
    /// built rather than matching nothing for the life of the world.
    [[nodiscard]] bool contains(std::string_view tag, registry::BlockId block) const;
    [[nodiscard]] bool known(std::string_view tag) const;

    [[nodiscard]] usize tag_count() const noexcept;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

/// A yes/no question about one position, as `block_predicate_filter` asks it
/// and as the disk feature's target does.
class BlockPredicate {
public:
    BlockPredicate()                                 = default;
    BlockPredicate(const BlockPredicate&)            = delete;
    BlockPredicate& operator=(const BlockPredicate&) = delete;
    virtual ~BlockPredicate();

    [[nodiscard]] virtual bool test(const FeatureLevel& level, BlockPos at) const = 0;
};

using BlockPredicateRef = std::shared_ptr<const BlockPredicate>;

/// Which placed features a biome lists.
///
/// The `biome` modifier is not a biome *test*: it asks whether the biome at
/// the position lists the feature being placed. A chunk is decorated with the
/// union of the features of every biome around it, so without this every
/// desert would grow the neighbouring forest's trees along its border.
class BiomeFeatures {
public:
    BiomeFeatures()                                = default;
    BiomeFeatures(const BiomeFeatures&)            = delete;
    BiomeFeatures& operator=(const BiomeFeatures&) = delete;
    virtual ~BiomeFeatures();

    [[nodiscard]] virtual bool lists(std::string_view biome, std::string_view feature) const = 0;
};

/// Everything a modifier or a feature needs besides the world itself.
struct FeatureContext {
    const registry::BlockRegistry* blocks{nullptr};
    /// The placed feature currently being placed, by name. The `biome`
    /// modifier needs it.
    std::string_view     feature_name;
    const BiomeFeatures* biomes{nullptr};
};

/// One stage of the pipeline.
class PlacementModifier {
public:
    PlacementModifier()                                    = default;
    PlacementModifier(const PlacementModifier&)            = delete;
    PlacementModifier& operator=(const PlacementModifier&) = delete;
    virtual ~PlacementModifier();

    /// The positions this stage turns one position into: none for a filter
    /// that rejects, one for a transform, several for a count.
    ///
    /// Appends to `out`, which the caller cleared, so that a pipeline of eight
    /// stages allocates nothing per position after the first chunk.
    virtual void positions(const FeatureContext& context, const FeatureLevel& level,
                           FeatureRandom& random, BlockPos at,
                           std::vector<BlockPos>& out) const = 0;

    /// What this stage is, for logs and for a parity harness that wants to say
    /// which modifier threw a position away.
    [[nodiscard]] virtual std::string_view name() const = 0;
};

using PlacementModifierRef = std::shared_ptr<const PlacementModifier>;

/// Run a pipeline over one origin, calling `sink` for every position that
/// comes out of the end of it.
///
/// Depth first, with the feature running inside `sink` before the next
/// position is produced. See the note at the top of this file: any other order
/// draws the same random numbers in the wrong places.
void expand(const std::vector<PlacementModifierRef>& pipeline, const FeatureContext& context,
            const FeatureLevel& level, FeatureRandom& random, BlockPos origin,
            const std::function<void(BlockPos)>& sink);

}  // namespace ov::worldgen
