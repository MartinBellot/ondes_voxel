// The ore features: `ore` and `scattered_ore`.
//
// An ore vein is not a blob. It is a *line* of spheres: two endpoints scattered
// around the origin, `size` spheres interpolated between them, each with its
// own radius, and every block whose centre falls inside any of them replaced —
// once, which is what the occupancy set is for. The line's direction is a
// single angle drawn before anything else and its length is size / 8, so a vein
// of nine blocks is a short sausage and a vein of twenty is a seam.
//
// Two details decide whether the numbers come out right rather than merely
// plausible:
//
//   * A sphere entirely inside another is *discarded* before anything is
//     placed, which is why a vein of size 9 almost never contains nine blocks'
//     worth of ore.
//   * `discard_chance_on_air_exposure` draws only when it is neither zero nor
//     one. At zero — which is every overworld ore but the buried ones — no
//     draw happens at all, and adding one would shift every later draw in the
//     chunk and move every feature after it.
//
// `scattered_ore` shares the target rules and nothing else: single blocks
// thrown around the origin, with the spread growing for the first eight of
// them and then holding.
#pragma once

#include "ov/worldgen/feature.hpp"

namespace ov::worldgen {

/// One rule of an ore: which blocks it may replace, and with what.
///
/// The blocks are resolved to ids when the feature is built rather than tested
/// through a tag at every candidate position. That is not only speed: it means
/// a rule naming a tag nobody exported fails at load, where a lazy lookup would
/// simply never match and the ore would silently not exist.
struct OreTarget {
    /// The state to write.
    registry::BlockStateId state{};
    /// Block ids this rule accepts, sorted.
    std::vector<u16> replaceable;

    [[nodiscard]] bool accepts(registry::BlockId block) const noexcept;
};

/// The shared configuration of both ore features.
struct OreConfig {
    std::vector<OreTarget> targets;
    i32                    size{0};
    f32                    discard_chance_on_air_exposure{0.0F};
};

/// `ore`: the interpolated line of spheres.
[[nodiscard]] FeatureRef make_ore_feature(OreConfig config);

/// `scattered_ore`: single blocks scattered around the origin.
[[nodiscard]] FeatureRef make_scattered_ore_feature(OreConfig config);

}  // namespace ov::worldgen
