// What a feature *is*, read out of the datapack rather than written here.
//
// The same decision density.cpp made for the terrain, made again one storey
// up. `worldgen/configured_feature/` holds 194 files in 53 types and
// `worldgen/placed_feature/` holds 231 more; hand-writing "an iron vein is
// nine blocks between y = 80 and y = 384" would be an approximation of one
// datapack that could never be seed-exact and would be wrong the moment
// anyone loaded a different one. So this is an interpreter, and a type it does
// not know is refused by name.
//
// What is here and what is not:
//
//   * `ore` and `scattered_ore`, complete, in ore_feature.cpp.
//   * `spring_feature` and `disk`, complete, below.
//   * `tree`, with its trunk placers, foliage placers, root placers and
//     decorators, in tree_feature.cpp.
//   * `simple_block`, the patches, the piles, the columns and the three
//     selectors, in vegetation_feature.cpp.
//   * Everything else — lakes, geodes, the nether's vegetation, structures —
//     is named and refused. `unavailable()` lists what did not load and why,
//     so the gap is a number rather than a surprise.
#pragma once

#include "ov/worldgen/placement.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::worldgen {

/// Which block state to write at a position.
///
/// Public because it is shared: a tree's trunk, its leaves and the dirt it
/// puts under itself each come from one of these, and so does every block a
/// vegetation patch scatters. Some of them draw — a weighted list picks with
/// one `nextInt` — so *when* a provider is asked is part of the seed and not
/// an implementation detail.
class StateProvider {
public:
    StateProvider()                                = default;
    StateProvider(const StateProvider&)            = delete;
    StateProvider& operator=(const StateProvider&) = delete;
    virtual ~StateProvider();

    [[nodiscard]] virtual registry::BlockStateId state(const FeatureLevel& level,
                                                       FeatureRandom&      random,
                                                       BlockPos            at) const = 0;

    /// Every state this provider could ever return.
    ///
    /// For checks that must happen when the datapack is read rather than in
    /// the world — the vegetation layer refuses a feature whose blocks it has
    /// no survival rule for, and it can only do that if it can enumerate them.
    /// An **empty** result means "cannot be enumerated" and must be treated as
    /// a refusal, never as "places nothing".
    [[nodiscard]] virtual std::vector<registry::BlockStateId> possible_states() const;
};

using StateProviderRef = std::shared_ptr<const StateProvider>;

/// One configured feature: a thing that can happen at a position.
class Feature {
public:
    Feature()                          = default;
    Feature(const Feature&)            = delete;
    Feature& operator=(const Feature&) = delete;
    virtual ~Feature();

    /// Try to happen at `at`. True when it wrote at least one block.
    ///
    /// The random source is the one the whole placed feature shares, and every
    /// draw made here lands between the draws of the placement pipeline. That
    /// interleaving is the reason `place` takes the generator rather than
    /// forking its own.
    virtual bool place(const FeatureContext& context, FeatureLevel& level,
                       FeatureRandom& random, BlockPos at) const = 0;

    [[nodiscard]] virtual std::string_view type_name() const = 0;
};

using FeatureRef = std::shared_ptr<const Feature>;

/// A configured feature plus the pipeline that decides where it goes.
struct PlacedFeature {
    std::string                       name;
    FeatureRef                        feature;
    std::vector<PlacementModifierRef> placement;
};

/// Every feature the datapack defines, and every one it could not build.
class FeatureRegistry {
public:
    /// Read `worldgen/configured_feature/`, `worldgen/placed_feature/` and the
    /// block tags they need, from the generated data root.
    [[nodiscard]] static std::expected<FeatureRegistry, FeatureError> load(
        const std::filesystem::path& data_root, const registry::BlockRegistry& blocks);

    /// A placed feature by name, e.g. "minecraft:ore_iron_upper", or nothing.
    ///
    /// Nothing, and never a feature that does not do anything: a placed
    /// feature whose configured feature or whose placement this interpreter
    /// cannot build is absent, and `unavailable()` says why.
    [[nodiscard]] const PlacedFeature* placed(std::string_view name) const;

    [[nodiscard]] usize placed_count() const noexcept;
    [[nodiscard]] usize configured_count() const noexcept;

    /// Everything that failed to load, by name, with the reason. Sorted, so
    /// that two runs produce comparable lists.
    [[nodiscard]] std::vector<std::pair<std::string, FeatureError>> unavailable() const;

    /// How many failures there were of each kind.
    [[nodiscard]] usize unsupported_count() const noexcept;

    [[nodiscard]] const BlockTags& tags() const noexcept;

    FeatureRegistry(FeatureRegistry&&) noexcept;
    FeatureRegistry& operator=(FeatureRegistry&&) noexcept;
    ~FeatureRegistry();

private:
    struct Impl;

    FeatureRegistry();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
