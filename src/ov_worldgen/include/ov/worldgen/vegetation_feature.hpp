// The vegetation family: what covers the ground once the trees are up.
//
//   `simple_block`             one block, if it can stay there
//   `random_patch`             sixty-four tries scattered around a point
//   `flower`, `no_bonemeal_flower`   the same thing under two registry names
//   `block_pile`               a heap of hay, snow, ice, melons or pumpkins
//   `block_column`             a hanging column, for the cave vines
//   `simple_random_selector`   one of n, uniformly
//   `random_selector`          weighted, first hit wins, with a fallback
//   `random_boolean_selector`  one coin toss between two
//
// Two things about this family are worth knowing before reading the code.
//
// The first is that the number of draws is fixed and does **not** depend on
// whether anything was placed. `random_patch` always makes six draws per try,
// all sixty-four tries, whether the ground is stone or grass. So a wrong
// survival rule costs blocks and never costs the seed — which is the opposite
// of everything in the ore and tree layers, and it is why the survival table
// below can be a table.
//
// The second is that the survival rule is a **gameplay** question, and
// gameplay is a layer above this one. The table names the rule for every block
// vanilla's vegetation features actually place, and a block that is not in it
// makes its feature refuse to load, by name. That is the whole of the
// approximation, and it is bounded and visible.
#pragma once

#include "ov/worldgen/feature.hpp"

#include <optional>
#include <string_view>

namespace ov::worldgen {

/// How one plant decides whether it may stay where it was put.
///
/// One entry per family of `BlockBehaviour.canSurvive`, named after what it
/// asks rather than after the class it came from.
enum class PlantSurvivalRule : u8 {
    /// Always. A full block — a pumpkin, a melon — has no rule.
    Always,
    /// The block below is in `#minecraft:dirt`, or is farmland. Every flower,
    /// the grasses, the ferns and the berry bush.
    DirtOrFarmland,
    /// `#minecraft:dirt` or clay. The azaleas.
    DirtOrClay,
    /// `#minecraft:dead_bush_may_place_on`.
    DeadBush,
    /// `#minecraft:nylium`, soul soil, or `#minecraft:dirt`. The nether roots.
    NyliumOrSoulSoil,
    /// Sand or another cactus below, nothing solid on any of the four sides,
    /// and no liquid above.
    Cactus,
    /// Sugar cane below, or dirt-or-sand below with water beside it.
    SugarCane,
    /// Water, or ice, directly below. The lily pad.
    Waterlily,
    /// Water here, and a sturdy top face below that is not magma.
    Seagrass,
    /// Anything but air below. The carpets.
    NotAirBelow,
    /// A sturdy face above and no water here. The spore blossom.
    HangingFromAbove,
};

/// The rule for one block, or nothing when this layer does not know it.
///
/// Nothing is a refusal and never a default: a feature whose block has no rule
/// here does not load, and `FeatureRegistry::unavailable()` names it. The four
/// vanilla features this costs are the two mushroom patches — whose rule reads
/// the light level, which worldgen does not compute — and the two fire
/// patches, whose rule asks what is flammable nearby.
[[nodiscard]] std::optional<PlantSurvivalRule> plant_survival_rule(
    std::string_view block_name) noexcept;

/// Whether a block is one of the two-tall plants, which `simple_block` places
/// as two halves rather than one block.
[[nodiscard]] bool is_double_plant(std::string_view block_name) noexcept;

}  // namespace ov::worldgen
