#include "ov/gameplay/redstone.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ov::gameplay {
namespace {

/// The horizontal directions in clockwise order, used by the diodes to name
/// their left and right sides.
[[nodiscard]] Direction clockwise(Direction d) noexcept {
    switch (d) {
        case Direction::North: return Direction::East;
        case Direction::East: return Direction::South;
        case Direction::South: return Direction::West;
        case Direction::West: return Direction::North;
        default: return d;
    }
}

[[nodiscard]] bool is_horizontal(Direction d) noexcept {
    return d != Direction::Up && d != Direction::Down;
}

/// The property a wire uses on one side, or empty for a vertical direction.
[[nodiscard]] std::string_view side_name(Direction d) noexcept {
    switch (d) {
        case Direction::North: return "north";
        case Direction::South: return "south";
        case Direction::West: return "west";
        case Direction::East: return "east";
        default: return {};
    }
}

/// The consumers whose flag simply follows the power around them.
///
/// Each entry is measured behaviour rather than a family rule: the lamp turns
/// on at once and off four ticks later, everything else answers immediately.
struct ConsumerSpec {
    std::string_view name;
    std::string_view flag;
    i32              delay;
    bool             delay_off_only;
    bool             inverted{false};
};

constexpr std::array<ConsumerSpec, 45> kConsumers{{
    {"minecraft:redstone_lamp", "lit", 4, true},
    {"minecraft:note_block", "powered", 0, false},
    // The hopper is the one entry whose flag reads backwards: `enabled` is
    // false while it is powered. Nothing drove it before this campaign, so a
    // lever on a hopper did nothing at all.
    {"minecraft:hopper", "enabled", 0, false, true},
    {"minecraft:iron_door", "powered", 0, false},
    {"minecraft:oak_door", "powered", 0, false},
    {"minecraft:spruce_door", "powered", 0, false},
    {"minecraft:birch_door", "powered", 0, false},
    {"minecraft:jungle_door", "powered", 0, false},
    {"minecraft:acacia_door", "powered", 0, false},
    {"minecraft:dark_oak_door", "powered", 0, false},
    {"minecraft:mangrove_door", "powered", 0, false},
    {"minecraft:cherry_door", "powered", 0, false},
    {"minecraft:bamboo_door", "powered", 0, false},
    {"minecraft:crimson_door", "powered", 0, false},
    {"minecraft:warped_door", "powered", 0, false},
    {"minecraft:iron_trapdoor", "powered", 0, false},
    {"minecraft:oak_trapdoor", "powered", 0, false},
    {"minecraft:spruce_trapdoor", "powered", 0, false},
    {"minecraft:birch_trapdoor", "powered", 0, false},
    {"minecraft:jungle_trapdoor", "powered", 0, false},
    {"minecraft:acacia_trapdoor", "powered", 0, false},
    {"minecraft:dark_oak_trapdoor", "powered", 0, false},
    {"minecraft:mangrove_trapdoor", "powered", 0, false},
    {"minecraft:cherry_trapdoor", "powered", 0, false},
    {"minecraft:bamboo_trapdoor", "powered", 0, false},
    {"minecraft:crimson_trapdoor", "powered", 0, false},
    {"minecraft:warped_trapdoor", "powered", 0, false},
    {"minecraft:oak_fence_gate", "powered", 0, false},
    {"minecraft:spruce_fence_gate", "powered", 0, false},
    {"minecraft:birch_fence_gate", "powered", 0, false},
    {"minecraft:jungle_fence_gate", "powered", 0, false},
    {"minecraft:acacia_fence_gate", "powered", 0, false},
    {"minecraft:dark_oak_fence_gate", "powered", 0, false},
    {"minecraft:mangrove_fence_gate", "powered", 0, false},
    {"minecraft:cherry_fence_gate", "powered", 0, false},
    {"minecraft:bamboo_fence_gate", "powered", 0, false},
    {"minecraft:crimson_fence_gate", "powered", 0, false},
    {"minecraft:warped_fence_gate", "powered", 0, false},
    {"minecraft:powered_rail", "powered", 0, false},
    {"minecraft:activator_rail", "powered", 0, false},
    {"minecraft:dispenser", "triggered", 0, false},
    {"minecraft:dropper", "triggered", 0, false},
    {"minecraft:bell", "powered", 0, false},
    {"minecraft:lectern", "powered", 0, false},
    {"minecraft:big_dripleaf", "powered", 0, false},
}};

/// The wood families, in the order the registry lists them. Every one of them
/// has a button, a plate, a door, a trapdoor and a fence gate.
constexpr std::array<std::string_view, 11> kWoods{
    "oak",      "spruce", "birch",  "jungle",  "acacia", "dark_oak",
    "mangrove", "cherry", "bamboo", "crimson", "warped",
};

/// How long a switch stays on, in ticks. **Measured**, tick by tick, against a
/// real 1.20.1 server: see `scripts/measure_redstone.py switches` and
/// docs/provenance/redstone.md. Before that campaign these were two literals in
/// item_use.cpp under a comment saying they had never been timed.
constexpr i32 kStoneButtonTicks   = 20;
constexpr i32 kWoodenButtonTicks  = 30;
constexpr i32 kPlateTicks         = 20;
constexpr i32 kWeightedPlateTicks = 10;

struct SwitchSpec {
    std::string_view name;
    i32              ticks;
    bool             rearms;
    bool             analogue;
};

/// Which instrument a note block plays, by the block **underneath** it.
///
/// Exhaustively measured, not derived: a note block was placed on top of every
/// one of the 987 blocks in the game and its own `instrument` block state was
/// read back off the save. 16 instruments, 0 refusals. See
/// docs/provenance/redstone.md and scripts/measure_redstone.py noteblock.
///
/// It has to be a table. There is **no naming rule**: a wooden button and a
/// wooden door are `harp`, not `bass`, and a suffix heuristic tried against the
/// measurement got 51 blocks wrong. Nothing about a block's name, its material
/// or its shape predicts the answer — vanilla declares it per block in Java.
///
/// `harp` is the default and the largest family (401 blocks), so it is the one
/// family that is *not* listed: everything absent from these tables is harp.
// generated from data/vanilla/1.20.1/normalized/redstone_noteblock.json
constexpr std::array<std::string_view, 280> kInstrument_basedrum{
    "andesite", "andesite_slab", "andesite_stairs", "andesite_wall", "basalt", "bedrock",
    "black_concrete", "black_glazed_terracotta", "black_terracotta", "blackstone",
    "blackstone_slab", "blackstone_stairs", "blackstone_wall", "blast_furnace", "blue_concrete",
    "blue_glazed_terracotta", "blue_terracotta", "brain_coral_block", "brick_slab",
    "brick_stairs", "brick_wall", "bricks", "brown_concrete", "brown_glazed_terracotta",
    "brown_terracotta", "bubble_coral", "bubble_coral_block", "bubble_coral_fan", "calcite",
    "chiseled_deepslate", "chiseled_nether_bricks", "chiseled_polished_blackstone",
    "chiseled_quartz_block", "chiseled_red_sandstone", "chiseled_sandstone",
    "chiseled_stone_bricks", "coal_block", "coal_ore", "cobbled_deepslate",
    "cobbled_deepslate_slab", "cobbled_deepslate_stairs", "cobbled_deepslate_wall",
    "cobblestone", "cobblestone_slab", "cobblestone_stairs", "cobblestone_wall", "copper_ore",
    "cracked_deepslate_bricks", "cracked_deepslate_tiles", "cracked_nether_bricks",
    "cracked_polished_blackstone_bricks", "cracked_stone_bricks", "crimson_nylium",
    "crying_obsidian", "cut_red_sandstone", "cut_red_sandstone_slab", "cut_sandstone",
    "cut_sandstone_slab", "cyan_concrete", "cyan_glazed_terracotta", "cyan_terracotta",
    "dark_prismarine", "dark_prismarine_slab", "dark_prismarine_stairs", "dead_brain_coral",
    "dead_brain_coral_block", "dead_brain_coral_fan", "dead_brain_coral_wall_fan",
    "dead_bubble_coral", "dead_bubble_coral_block", "dead_bubble_coral_fan",
    "dead_bubble_coral_wall_fan", "dead_fire_coral", "dead_fire_coral_block",
    "dead_fire_coral_fan", "dead_fire_coral_wall_fan", "dead_horn_coral",
    "dead_horn_coral_block", "dead_horn_coral_fan", "dead_horn_coral_wall_fan",
    "dead_tube_coral", "dead_tube_coral_block", "dead_tube_coral_fan",
    "dead_tube_coral_wall_fan", "deepslate", "deepslate_brick_slab", "deepslate_brick_stairs",
    "deepslate_brick_wall", "deepslate_bricks", "deepslate_coal_ore", "deepslate_copper_ore",
    "deepslate_diamond_ore", "deepslate_emerald_ore", "deepslate_gold_ore",
    "deepslate_iron_ore", "deepslate_lapis_ore", "deepslate_redstone_ore",
    "deepslate_tile_slab", "deepslate_tile_stairs", "deepslate_tile_wall", "deepslate_tiles",
    "diamond_ore", "diorite", "diorite_slab", "diorite_stairs", "diorite_wall", "dispenser",
    "dripstone_block", "dropper", "emerald_ore", "enchanting_table", "end_portal_frame",
    "end_stone", "end_stone_brick_slab", "end_stone_brick_stairs", "end_stone_brick_wall",
    "end_stone_bricks", "ender_chest", "fire_coral_block", "furnace", "gilded_blackstone",
    "gold_ore", "granite", "granite_slab", "granite_stairs", "granite_wall", "gray_concrete",
    "gray_glazed_terracotta", "gray_terracotta", "green_concrete", "green_glazed_terracotta",
    "green_terracotta", "horn_coral", "horn_coral_block", "horn_coral_fan",
    "horn_coral_wall_fan", "iron_ore", "lapis_ore", "light_blue_concrete",
    "light_blue_glazed_terracotta", "light_blue_terracotta", "light_gray_concrete",
    "light_gray_glazed_terracotta", "light_gray_terracotta", "lime_concrete",
    "lime_glazed_terracotta", "lime_terracotta", "magenta_concrete",
    "magenta_glazed_terracotta", "magenta_terracotta", "magma_block", "mossy_cobblestone",
    "mossy_cobblestone_slab", "mossy_cobblestone_stairs", "mossy_cobblestone_wall",
    "mossy_stone_brick_slab", "mossy_stone_brick_stairs", "mossy_stone_brick_wall",
    "mossy_stone_bricks", "mud_brick_slab", "mud_brick_stairs", "mud_brick_wall", "mud_bricks",
    "nether_brick_fence", "nether_brick_slab", "nether_brick_stairs", "nether_brick_wall",
    "nether_bricks", "nether_gold_ore", "nether_quartz_ore", "netherrack", "observer",
    "obsidian", "orange_concrete", "orange_glazed_terracotta", "orange_terracotta",
    "petrified_oak_slab", "pink_concrete", "pink_glazed_terracotta", "pink_terracotta",
    "pointed_dripstone", "polished_andesite", "polished_andesite_slab",
    "polished_andesite_stairs", "polished_basalt", "polished_blackstone",
    "polished_blackstone_brick_slab", "polished_blackstone_brick_stairs",
    "polished_blackstone_brick_wall", "polished_blackstone_bricks",
    "polished_blackstone_pressure_plate", "polished_blackstone_slab",
    "polished_blackstone_stairs", "polished_blackstone_wall", "polished_deepslate",
    "polished_deepslate_slab", "polished_deepslate_stairs", "polished_deepslate_wall",
    "polished_diorite", "polished_diorite_slab", "polished_diorite_stairs", "polished_granite",
    "polished_granite_slab", "polished_granite_stairs", "prismarine", "prismarine_brick_slab",
    "prismarine_brick_stairs", "prismarine_bricks", "prismarine_slab", "prismarine_stairs",
    "prismarine_wall", "purple_concrete", "purple_glazed_terracotta", "purple_terracotta",
    "purpur_block", "purpur_pillar", "purpur_slab", "purpur_stairs", "quartz_block",
    "quartz_bricks", "quartz_pillar", "quartz_slab", "quartz_stairs", "raw_copper_block",
    "raw_gold_block", "raw_iron_block", "red_concrete", "red_glazed_terracotta",
    "red_nether_brick_slab", "red_nether_brick_stairs", "red_nether_brick_wall",
    "red_nether_bricks", "red_sandstone", "red_sandstone_slab", "red_sandstone_stairs",
    "red_sandstone_wall", "red_terracotta", "redstone_ore", "reinforced_deepslate",
    "respawn_anchor", "sandstone", "sandstone_slab", "sandstone_stairs", "sandstone_wall",
    "smoker", "smooth_basalt", "smooth_quartz", "smooth_quartz_slab", "smooth_quartz_stairs",
    "smooth_red_sandstone", "smooth_red_sandstone_slab", "smooth_red_sandstone_stairs",
    "smooth_sandstone", "smooth_sandstone_slab", "smooth_sandstone_stairs", "smooth_stone",
    "smooth_stone_slab", "spawner", "stone", "stone_brick_slab", "stone_brick_stairs",
    "stone_brick_wall", "stone_bricks", "stone_pressure_plate", "stone_slab", "stone_stairs",
    "stonecutter", "terracotta", "tube_coral", "tube_coral_block", "tube_coral_fan",
    "tube_coral_wall_fan", "tuff", "warped_nylium", "white_concrete", "white_glazed_terracotta",
    "white_terracotta", "yellow_concrete", "yellow_glazed_terracotta", "yellow_terracotta",
};

constexpr std::array<std::string_view, 221> kInstrument_bass{
    "acacia_fence", "acacia_fence_gate", "acacia_hanging_sign", "acacia_log", "acacia_planks",
    "acacia_pressure_plate", "acacia_sign", "acacia_slab", "acacia_stairs", "acacia_trapdoor",
    "acacia_wall_hanging_sign", "acacia_wall_sign", "acacia_wood", "bamboo_block",
    "bamboo_fence", "bamboo_fence_gate", "bamboo_hanging_sign", "bamboo_mosaic",
    "bamboo_mosaic_slab", "bamboo_mosaic_stairs", "bamboo_planks", "bamboo_pressure_plate",
    "bamboo_sign", "bamboo_slab", "bamboo_stairs", "bamboo_trapdoor",
    "bamboo_wall_hanging_sign", "bamboo_wall_sign", "barrel", "bee_nest", "beehive",
    "birch_fence", "birch_fence_gate", "birch_hanging_sign", "birch_log", "birch_planks",
    "birch_pressure_plate", "birch_sign", "birch_slab", "birch_stairs", "birch_trapdoor",
    "birch_wall_hanging_sign", "birch_wall_sign", "birch_wood", "black_banner",
    "black_wall_banner", "blue_banner", "blue_wall_banner", "bookshelf", "brown_banner",
    "brown_mushroom_block", "brown_wall_banner", "campfire", "cartography_table",
    "cherry_fence", "cherry_fence_gate", "cherry_hanging_sign", "cherry_log", "cherry_planks",
    "cherry_pressure_plate", "cherry_sign", "cherry_slab", "cherry_stairs", "cherry_trapdoor",
    "cherry_wall_hanging_sign", "cherry_wall_sign", "cherry_wood", "chest",
    "chiseled_bookshelf", "composter", "crafting_table", "crimson_fence", "crimson_fence_gate",
    "crimson_hanging_sign", "crimson_hyphae", "crimson_planks", "crimson_pressure_plate",
    "crimson_sign", "crimson_slab", "crimson_stairs", "crimson_stem", "crimson_trapdoor",
    "crimson_wall_hanging_sign", "crimson_wall_sign", "cyan_banner", "cyan_wall_banner",
    "dark_oak_fence", "dark_oak_fence_gate", "dark_oak_hanging_sign", "dark_oak_log",
    "dark_oak_planks", "dark_oak_pressure_plate", "dark_oak_sign", "dark_oak_slab",
    "dark_oak_stairs", "dark_oak_trapdoor", "dark_oak_wall_hanging_sign", "dark_oak_wall_sign",
    "dark_oak_wood", "daylight_detector", "fletching_table", "gray_banner", "gray_wall_banner",
    "green_banner", "green_wall_banner", "jukebox", "jungle_fence", "jungle_fence_gate",
    "jungle_hanging_sign", "jungle_log", "jungle_planks", "jungle_pressure_plate",
    "jungle_sign", "jungle_slab", "jungle_stairs", "jungle_trapdoor",
    "jungle_wall_hanging_sign", "jungle_wall_sign", "jungle_wood", "lectern",
    "light_blue_banner", "light_blue_wall_banner", "light_gray_banner",
    "light_gray_wall_banner", "lime_banner", "lime_wall_banner", "loom", "magenta_banner",
    "magenta_wall_banner", "mangrove_fence", "mangrove_fence_gate", "mangrove_hanging_sign",
    "mangrove_log", "mangrove_planks", "mangrove_pressure_plate", "mangrove_roots",
    "mangrove_sign", "mangrove_slab", "mangrove_stairs", "mangrove_trapdoor",
    "mangrove_wall_hanging_sign", "mangrove_wall_sign", "mangrove_wood", "mushroom_stem",
    "note_block", "oak_fence", "oak_fence_gate", "oak_hanging_sign", "oak_log", "oak_planks",
    "oak_pressure_plate", "oak_sign", "oak_slab", "oak_stairs", "oak_trapdoor",
    "oak_wall_hanging_sign", "oak_wall_sign", "oak_wood", "orange_banner", "orange_wall_banner",
    "pink_banner", "pink_wall_banner", "purple_banner", "purple_wall_banner", "red_banner",
    "red_mushroom_block", "red_wall_banner", "smithing_table", "soul_campfire", "spruce_fence",
    "spruce_fence_gate", "spruce_hanging_sign", "spruce_log", "spruce_planks",
    "spruce_pressure_plate", "spruce_sign", "spruce_slab", "spruce_stairs", "spruce_trapdoor",
    "spruce_wall_hanging_sign", "spruce_wall_sign", "spruce_wood", "stripped_acacia_log",
    "stripped_acacia_wood", "stripped_bamboo_block", "stripped_birch_log",
    "stripped_birch_wood", "stripped_cherry_log", "stripped_cherry_wood",
    "stripped_crimson_hyphae", "stripped_crimson_stem", "stripped_dark_oak_log",
    "stripped_dark_oak_wood", "stripped_jungle_log", "stripped_jungle_wood",
    "stripped_mangrove_log", "stripped_mangrove_wood", "stripped_oak_log", "stripped_oak_wood",
    "stripped_spruce_log", "stripped_spruce_wood", "stripped_warped_hyphae",
    "stripped_warped_stem", "trapped_chest", "warped_fence", "warped_fence_gate",
    "warped_hanging_sign", "warped_hyphae", "warped_planks", "warped_pressure_plate",
    "warped_sign", "warped_slab", "warped_stairs", "warped_stem", "warped_trapdoor",
    "warped_wall_hanging_sign", "warped_wall_sign", "white_banner", "white_wall_banner",
    "yellow_banner", "yellow_wall_banner",
};

constexpr std::array<std::string_view, 38> kInstrument_hat{
    "beacon", "black_stained_glass", "black_stained_glass_pane", "blue_stained_glass",
    "blue_stained_glass_pane", "brown_stained_glass", "brown_stained_glass_pane", "conduit",
    "cyan_stained_glass", "cyan_stained_glass_pane", "glass", "glass_pane",
    "gray_stained_glass", "gray_stained_glass_pane", "green_stained_glass",
    "green_stained_glass_pane", "light_blue_stained_glass", "light_blue_stained_glass_pane",
    "light_gray_stained_glass", "light_gray_stained_glass_pane", "lime_stained_glass",
    "lime_stained_glass_pane", "magenta_stained_glass", "magenta_stained_glass_pane",
    "orange_stained_glass", "orange_stained_glass_pane", "pink_stained_glass",
    "pink_stained_glass_pane", "purple_stained_glass", "purple_stained_glass_pane",
    "red_stained_glass", "red_stained_glass_pane", "sea_lantern", "tinted_glass",
    "white_stained_glass", "white_stained_glass_pane", "yellow_stained_glass",
    "yellow_stained_glass_pane",
};

constexpr std::array<std::string_view, 21> kInstrument_snare{
    "black_concrete_powder", "blue_concrete_powder", "brown_concrete_powder",
    "cyan_concrete_powder", "gravel", "gray_concrete_powder", "green_concrete_powder",
    "light_blue_concrete_powder", "light_gray_concrete_powder", "lime_concrete_powder",
    "magenta_concrete_powder", "orange_concrete_powder", "pink_concrete_powder",
    "purple_concrete_powder", "red_concrete_powder", "red_sand", "sand", "suspicious_gravel",
    "suspicious_sand", "white_concrete_powder", "yellow_concrete_powder",
};

constexpr std::array<std::string_view, 16> kInstrument_guitar{
    "black_wool", "blue_wool", "brown_wool", "cyan_wool", "gray_wool", "green_wool",
    "light_blue_wool", "light_gray_wool", "lime_wool", "magenta_wool", "orange_wool",
    "pink_wool", "purple_wool", "red_wool", "white_wool", "yellow_wool",
};

constexpr std::array<std::string_view, 1> kInstrument_banjo{
    "hay_block",
};

constexpr std::array<std::string_view, 1> kInstrument_bell{
    "gold_block",
};

constexpr std::array<std::string_view, 1> kInstrument_bit{
    "emerald_block",
};

constexpr std::array<std::string_view, 1> kInstrument_chime{
    "packed_ice",
};

constexpr std::array<std::string_view, 1> kInstrument_cow_bell{
    "soul_sand",
};

constexpr std::array<std::string_view, 1> kInstrument_didgeridoo{
    "pumpkin",
};

constexpr std::array<std::string_view, 1> kInstrument_flute{
    "clay",
};

constexpr std::array<std::string_view, 1> kInstrument_iron_xylophone{
    "iron_block",
};

constexpr std::array<std::string_view, 1> kInstrument_pling{
    "glowstone",
};

constexpr std::array<std::string_view, 1> kInstrument_xylophone{
    "bone_block",
};

/// The two switches that are not made of a wood.
constexpr std::array<SwitchSpec, 6> kFixedSwitches{{
    {"minecraft:stone_button", kStoneButtonTicks, false, false},
    {"minecraft:polished_blackstone_button", kStoneButtonTicks, false, false},
    {"minecraft:stone_pressure_plate", kPlateTicks, true, false},
    {"minecraft:polished_blackstone_pressure_plate", kPlateTicks, true, false},
    {"minecraft:light_weighted_pressure_plate", kWeightedPlateTicks, true, true},
    {"minecraft:heavy_weighted_pressure_plate", kWeightedPlateTicks, true, true},
}};

}  // namespace

// ── TorchHistory ────────────────────────────────────────────────────────────

bool TorchHistory::record_and_check(BlockPos pos, i64 now) {
    expire(now);
    toggles_.push_back(Toggle{pos, now});
    i32 seen = 0;
    for (const Toggle& toggle : toggles_) {
        if (toggle.pos == pos) {
            ++seen;
        }
    }
    // Vanilla counts the toggle it has just added, so the eighth is the one
    // that burns out — not the ninth.
    return seen >= kTorchBurnoutChanges;
}

void TorchHistory::expire(i64 now) {
    while (!toggles_.empty() && now - toggles_.front().tick > kTorchBurnoutWindow) {
        toggles_.pop_front();
    }
    // A world full of clocks would otherwise grow this without bound between
    // two expiries. Dropping the oldest is what vanilla's own list does when
    // the window slides, only sooner.
    while (toggles_.size() > kTorchHistoryCap) {
        toggles_.pop_front();
    }
}

// ── Redstone ────────────────────────────────────────────────────────────────

Redstone::Redstone(const registry::BlockRegistry& blocks, const registry::Registries& registries)
    : blocks_{&blocks}, signals_{blocks, registries} {
    const auto id = [&](std::string_view name) {
        return blocks.find_block(name).value_or(registry::BlockId{0});
    };
    ids_ = Ids{
        .wire          = id("minecraft:redstone_wire"),
        .repeater      = id("minecraft:repeater"),
        .comparator    = id("minecraft:comparator"),
        .observer      = id("minecraft:observer"),
        .torch         = id("minecraft:redstone_torch"),
        .wall_torch    = id("minecraft:redstone_wall_torch"),
        .lamp          = id("minecraft:redstone_lamp"),
        .piston        = id("minecraft:piston"),
        .sticky_piston = id("minecraft:sticky_piston"),
        .piston_head   = id("minecraft:piston_head"),
        .moving_piston = id("minecraft:moving_piston"),
        .dispenser     = id("minecraft:dispenser"),
        .dropper       = id("minecraft:dropper"),
        .hopper        = id("minecraft:hopper"),
        .note_block    = id("minecraft:note_block"),
        .tnt           = id("minecraft:tnt"),
        .air           = id("minecraft:air"),
    };

    consumer_index_.assign(blocks.block_count(), -1);
    for (const ConsumerSpec& spec : kConsumers) {
        const auto block = blocks.find_block(spec.name);
        if (!block.has_value()) {
            continue;
        }
        consumer_index_[block->value()] = static_cast<i16>(consumers_.size());
        consumers_.push_back(
            ConsumerRule{*block, spec.flag, spec.delay, spec.delay_off_only, spec.inverted});
    }

    switch_index_.assign(blocks.block_count(), -1);
    const auto add_switch = [&](std::string_view name, i32 ticks, bool rearms, bool analogue) {
        const auto block = blocks.find_block(name);
        if (!block.has_value()) {
            return;
        }
        switch_index_[block->value()] = static_cast<i16>(switches_.size());
        switches_.push_back(SwitchRule{*block, ticks, rearms, analogue});
    };
    for (const SwitchSpec& spec : kFixedSwitches) {
        add_switch(spec.name, spec.ticks, spec.rearms, spec.analogue);
    }
    // The wooden families are built rather than tabulated: eleven woods times
    // two blocks is twenty-two lines that say nothing a loop does not, and a
    // wood added to the registry is then handled by naming it once.
    std::string name;
    for (const std::string_view wood : kWoods) {
        name.assign("minecraft:").append(wood).append("_button");
        add_switch(name, kWoodenButtonTicks, false, false);
        name.assign("minecraft:").append(wood).append("_pressure_plate");
        add_switch(name, kPlateTicks, true, false);
    }

    // ── The note block's instrument ─────────────────────────────────────────
    instrument_index_.assign(blocks.block_count(), -1);
    if (ids_.note_block.value() != 0) {
        instrument_property_ = blocks.find_property(ids_.note_block, "instrument");
    }
    if (instrument_property_.has_value()) {
        const auto mark = [&](std::span<const std::string_view> family,
                              std::string_view                  instrument) {
            i16 value = -1;
            for (u16 i = 0; i < instrument_property_->values.size(); ++i) {
                if (instrument_property_->values[i] == instrument) {
                    value = static_cast<i16>(i);
                    break;
                }
            }
            if (value < 0) {
                // A version whose note block does not have this instrument.
                // Refused rather than mapped to whatever is at index 0, which
                // would make a whole family play the wrong note silently.
                return;
            }
            for (const std::string_view short_name : family) {
                name.assign("minecraft:").append(short_name);
                if (const auto block = blocks.find_block(name)) {
                    instrument_index_[block->value()] = value;
                }
            }
        };
        mark(kInstrument_basedrum, "basedrum");
        mark(kInstrument_bass, "bass");
        mark(kInstrument_hat, "hat");
        mark(kInstrument_snare, "snare");
        mark(kInstrument_guitar, "guitar");
        mark(kInstrument_banjo, "banjo");
        mark(kInstrument_bell, "bell");
        mark(kInstrument_bit, "bit");
        mark(kInstrument_chime, "chime");
        mark(kInstrument_cow_bell, "cow_bell");
        mark(kInstrument_didgeridoo, "didgeridoo");
        mark(kInstrument_flute, "flute");
        mark(kInstrument_iron_xylophone, "iron_xylophone");
        mark(kInstrument_pling, "pling");
        mark(kInstrument_xylophone, "xylophone");
    }
}

std::string_view Redstone::note_instrument(registry::BlockStateId below) const noexcept {
    if (!instrument_property_.has_value()) {
        return {};
    }
    const registry::BlockId block = blocks_->block_of(below);
    const i16 value = block.value() < instrument_index_.size()
                          ? instrument_index_[block.value()]
                          : static_cast<i16>(-1);
    if (value < 0) {
        return "harp";
    }
    return instrument_property_->values[static_cast<usize>(value)];
}


/// The scheduler names blocks by their registry name rather than by an id, so
/// that one queue can hold both a repeater and a fluid without saying which
/// registry a number came from. These two wrap that translation, so the rules
/// below go on speaking in block ids.
void Redstone::wake(RedstoneWorld& world, BlockPos pos, registry::BlockId block, i32 delay,
                    world::TickPriority priority) const {
    world.schedule_tick(pos, blocks_->block_name(block), delay, world::TickQueue::Block, priority);
}

bool Redstone::waking(const RedstoneWorld& world, BlockPos pos,
                      registry::BlockId block) const {
    return world.has_scheduled_tick(pos, blocks_->block_name(block), world::TickQueue::Block);
}

const Redstone::ConsumerRule* Redstone::consumer_rule(registry::BlockId block) const noexcept {
    if (block.value() >= consumer_index_.size()) {
        return nullptr;
    }
    const i16 index = consumer_index_[block.value()];
    return index < 0 ? nullptr : &consumers_[static_cast<usize>(index)];
}

const Redstone::SwitchRule* Redstone::switch_rule(registry::BlockId block) const noexcept {
    if (block.value() >= switch_index_.size()) {
        return nullptr;
    }
    const i16 index = switch_index_[block.value()];
    return index < 0 ? nullptr : &switches_[static_cast<usize>(index)];
}

// ── Switches ────────────────────────────────────────────────────────────────

bool Redstone::switch_tick(RedstoneWorld& world, BlockPos pos, const SwitchRule& rule,
                           registry::BlockStateId state) {
    if (!rule.rearms) {
        // A button. Its tick has one meaning and no condition attached: pop
        // back up. Vanilla checks `powered` first, because a button that was
        // replaced and re-placed inside its own delay would otherwise be
        // released by a tick meant for the old one.
        if (!signals_.flag_of(state, "powered")) {
            return false;
        }
        world.set_block(pos, signals_.with_flag(state, "powered", false));
        return true;
    }

    // A plate. The tick asks the world again, and re-arms while the answer is
    // still yes — which is why the delay measured from the press is the period
    // and the delay measured from the moment the entity leaves is not.
    const i32 pressure = world.entity_pressure(pos);
    if (pressure > 0) {
        wake(world, pos, blocks_->block_of(state), rule.ticks, world::TickPriority::Normal);
        return false;
    }
    if (rule.analogue) {
        if (signals_.power_of(state) == 0) {
            return false;
        }
        world.set_block(pos, signals_.with_power(state, 0));
        return true;
    }
    if (!signals_.flag_of(state, "powered")) {
        return false;
    }
    world.set_block(pos, signals_.with_flag(state, "powered", false));
    return true;
}

bool Redstone::plate_step(RedstoneWorld& world, BlockPos pos) {
    const registry::BlockStateId state = world.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);
    const SwitchRule*            rule  = switch_rule(block);
    if (rule == nullptr || !rule->rearms) {
        return false;
    }
    const i32 pressure = world.entity_pressure(pos);
    if (pressure <= 0) {
        return false;
    }

    const i32  have    = rule->analogue ? signals_.power_of(state)
                                        : (signals_.flag_of(state, "powered") ? 15 : 0);
    const i32  want    = rule->analogue ? std::min(pressure, 15) : 15;
    const bool changed = have != want;
    if (changed) {
        world.set_block(pos, rule->analogue ? signals_.with_power(state, want)
                                            : signals_.with_flag(state, "powered", true));
    }
    // Armed on every step, not only on the change: a plate that is already down
    // and whose tick has already fired must be woken again or it never lets go.
    if (!waking(world, pos, block)) {
        wake(world, pos, block, rule->ticks, world::TickPriority::Normal);
    }
    return changed;
}

// ── Wire shape ──────────────────────────────────────────────────────────────

bool Redstone::wire_connects_to(const RedstoneWorld& world, BlockPos neighbour,
                                Direction towards) const {
    const registry::BlockStateId state = world.block_at(neighbour);
    const registry::BlockId      block = blocks_->block_of(state);
    if (block == ids_.wire) {
        return true;
    }
    if (!is_horizontal(towards)) {
        // Vertically, a wire only ever reaches another wire. Asking a lever
        // above a wire for a connection is what makes wire climb walls it has
        // no business climbing.
        return false;
    }
    if (block == ids_.repeater) {
        // A repeater faces its input, so a wire meets it head on or tail on and
        // never from the side.
        const auto facing = signals_.facing_of(state);
        return facing == std::optional{towards} || facing == std::optional{opposite(towards)};
    }
    if (block == ids_.observer) {
        // Its output face only, which is the one opposite `facing`.
        return signals_.facing_of(state) == std::optional{towards};
    }
    return signals_.kind_of(block) != SignalKind::None;
}

WireSide Redstone::wire_reach(const RedstoneWorld& world, BlockPos pos, Direction side) const {
    const BlockPos above        = pos.above();
    const bool     can_reach_up = !signals_.is_conductor(world.block_at(above));

    const BlockPos               side_pos   = pos.offset(side);
    const registry::BlockStateId side_state = world.block_at(side_pos);

    if (can_reach_up) {
        // Climbing needs a floor to land on. A trapdoor counts even though its
        // top face is not sturdy, which is the one exception in the rule.
        const bool floor_above =
            blocks_->face_is_sturdy(side_state, registry::BlockRegistry::Face::Up) ||
            blocks_->block_name(blocks_->block_of(side_state)).ends_with("_trapdoor");
        if (floor_above && wire_connects_to(world, side_pos.above(), Direction::Up)) {
            return signals_.is_full_cube(side_state) ? WireSide::Up : WireSide::Side;
        }
    }
    if (wire_connects_to(world, side_pos, side)) {
        return WireSide::Side;
    }
    // Dropping off the side onto a lower wire, but only past something that is
    // not solid: a wire does not reach through a wall to the wire behind it.
    if (signals_.is_conductor(side_state)) {
        return WireSide::None;
    }
    return wire_connects_to(world, side_pos.below(), Direction::Down) ? WireSide::Side
                                                                     : WireSide::None;
}

registry::BlockStateId Redstone::wire_shape(const RedstoneWorld& world, BlockPos pos,
                                            registry::BlockStateId state) const {
    if (blocks_->block_of(state) != ids_.wire) {
        return state;
    }
    registry::BlockStateId result = state;
    i32                    reached = 0;
    std::array<WireSide, 4> sides{};
    for (usize i = 0; i < kHorizontals.size(); ++i) {
        sides[i] = wire_reach(world, pos, kHorizontals[i]);
        if (sides[i] != WireSide::None) {
            ++reached;
        }
    }
    // A wire with a single connection, or none, draws itself as a cross rather
    // than as a stub: vanilla keeps the two opposite sides when only one side
    // connects, so a dead end still shows as a line. One connection means both
    // it and its opposite are drawn.
    std::array<WireSide, 4> drawn = sides;
    if (reached <= 1) {
        for (usize i = 0; i < kHorizontals.size(); ++i) {
            const usize other = i ^ 1u;  // the enum pairs opposites
            if (sides[i] != WireSide::None || sides[other] != WireSide::None) {
                drawn[i] = sides[i] == WireSide::Up ? WireSide::Up : WireSide::Side;
            }
        }
        if (reached == 0) {
            drawn.fill(WireSide::Side);
        }
    }

    for (usize i = 0; i < kHorizontals.size(); ++i) {
        const auto prop = blocks_->find_property(ids_.wire, side_name(kHorizontals[i]));
        if (!prop.has_value()) {
            continue;
        }
        const std::string_view want = kWireSideNames[static_cast<usize>(drawn[i])];
        for (u16 v = 0; v < prop->values.size(); ++v) {
            if (prop->values[v] == want) {
                result = blocks_->with_property(result, *prop, v);
                break;
            }
        }
    }
    return result;
}

// ── Wire power ──────────────────────────────────────────────────────────────

i32 Redstone::wire_strength(const RedstoneWorld& world, BlockPos pos) const {
    // Every wire in the world is silent for the duration of this call. That one
    // flag is what stops wire → solid block → wire from carrying anything,
    // while leaving wire → solid block → repeater working.
    const i32 outside = signals_.best_neighbour_signal(world, pos, /*wires_signal=*/false);
    if (outside >= 15) {
        return 15;
    }

    const auto wire_power = [&](registry::BlockStateId state) {
        return blocks_->block_of(state) == ids_.wire ? std::max(0, signals_.power_of(state)) : 0;
    };

    const bool ceiling = signals_.is_conductor(world.block_at(pos.above()));
    i32        best    = 0;
    for (const Direction d : kHorizontals) {
        const BlockPos               side_pos = pos.offset(d);
        const registry::BlockStateId side     = world.block_at(side_pos);
        best                                  = std::max(best, wire_power(side));
        if (signals_.is_conductor(side)) {
            // Climbing a block: only when nothing solid is over our own head.
            if (!ceiling) {
                best = std::max(best, wire_power(world.block_at(side_pos.above())));
            }
        } else {
            // Dropping down past something that is not solid.
            best = std::max(best, wire_power(world.block_at(side_pos.below())));
        }
    }
    return std::max(outside, best - 1);
}

void Redstone::update_wire(RedstoneWorld& world, BlockPos start) {
    // Vanilla recurses through neighbour updates, which on a long wire is a
    // stack a thousand frames deep. The same order comes out of an explicit
    // last-in-first-out worklist, and it cannot overflow.
    std::vector<BlockPos> pending;
    pending.push_back(start);

    // A generous ceiling on one propagation. A wire loop that never settles is
    // laggy in vanilla too; what it must not be is unbounded.
    usize steps = 0;
    while (!pending.empty() && steps < 1u << 16u) {
        const BlockPos pos = pending.back();
        pending.pop_back();
        ++steps;

        const registry::BlockStateId state = world.block_at(pos);
        if (blocks_->block_of(state) != ids_.wire) {
            continue;
        }
        // Shape first, then power. The two are not independent: a wire hands
        // its strength only to a block it visually reaches towards, so a stale
        // shape makes a correct power invisible to everything beside it.
        const registry::BlockStateId shaped = wire_shape(world, pos, state);
        const i32                    target = wire_strength(world, pos);
        const registry::BlockStateId want   = signals_.with_power(shaped, target);
        if (want == state) {
            continue;
        }
        world.set_block(pos, want);

        // The changed wire and each of its six neighbours have their own
        // neighbours told. That double ring is why a wire beside a wall powers
        // the block on the far side of it.
        pending.push_back(pos);
        for (u8 i = 0; i < kDirectionCount; ++i) {
            const BlockPos neighbour = pos.offset(static_cast<Direction>(i));
            pending.push_back(neighbour);
            for (u8 j = 0; j < kDirectionCount; ++j) {
                const BlockPos second = neighbour.offset(static_cast<Direction>(j));
                if (blocks_->block_of(world.block_at(second)) == ids_.wire) {
                    pending.push_back(second);
                }
            }
        }
    }
}

// ── Diodes ──────────────────────────────────────────────────────────────────

i32 Redstone::diode_input(const RedstoneWorld& world, BlockPos pos,
                          registry::BlockStateId state) const {
    const auto facing = signals_.facing_of(state);
    if (!facing.has_value()) {
        return 0;
    }
    const BlockPos input = pos.offset(*facing);
    i32            level = signals_.signal_at(world, input, *facing);
    if (level >= 15) {
        return 15;
    }
    // A wire in front of a diode hands over its full strength, not the strength
    // its connection shape would give a block. Without this, a wire that does
    // not visually point into a repeater still feeds it — which is what vanilla
    // does and looks like a bug until you try it.
    const registry::BlockStateId in_state = world.block_at(input);
    if (blocks_->block_of(in_state) == ids_.wire) {
        level = std::max(level, std::max(0, signals_.power_of(in_state)));
    }
    return level;
}

i32 Redstone::diode_side_input(const RedstoneWorld& world, BlockPos pos,
                               registry::BlockStateId state) const {
    const auto facing = signals_.facing_of(state);
    if (!facing.has_value() || !is_horizontal(*facing)) {
        return 0;
    }
    const bool comparator = blocks_->block_of(state) == ids_.comparator;

    const auto at = [&](Direction side) -> i32 {
        const BlockPos               side_pos   = pos.offset(side);
        const registry::BlockStateId side_state = world.block_at(side_pos);
        const registry::BlockId      block      = blocks_->block_of(side_state);
        // Only a diode counts sideways — a lever beside a repeater does not
        // lock it. A comparator additionally accepts a wire and a redstone
        // block, which a repeater does not.
        if (block == ids_.repeater || block == ids_.comparator) {
            return signals_.strong_signal(world, side_pos, side);
        }
        if (comparator) {
            if (block == ids_.wire) {
                return std::max(0, signals_.power_of(side_state));
            }
            if (blocks_->block_name(block) == "minecraft:redstone_block") {
                return 15;
            }
        }
        return 0;
    };

    const Direction left  = clockwise(*facing);
    const Direction right = clockwise(clockwise(clockwise(*facing)));
    return std::max(at(left), at(right));
}

bool Redstone::repeater_locked(const RedstoneWorld& world, BlockPos pos,
                               registry::BlockStateId state) const {
    if (blocks_->block_of(state) != ids_.repeater) {
        return false;
    }
    // A repeater is locked by a *diode* pointing at its side, and by nothing
    // else. The comparator's extra inputs do not lock.
    const auto facing = signals_.facing_of(state);
    if (!facing.has_value()) {
        return false;
    }
    const auto at = [&](Direction side) -> i32 {
        const BlockPos               side_pos   = pos.offset(side);
        const registry::BlockStateId side_state = world.block_at(side_pos);
        const registry::BlockId      block      = blocks_->block_of(side_state);
        if (block != ids_.repeater && block != ids_.comparator) {
            return 0;
        }
        return signals_.strong_signal(world, side_pos, side);
    };
    return std::max(at(clockwise(*facing)), at(clockwise(clockwise(clockwise(*facing))))) > 0;
}

i32 Redstone::comparator_output(const RedstoneWorld& world, BlockPos pos,
                                registry::BlockStateId state) const {
    const auto facing = signals_.facing_of(state);
    if (!facing.has_value()) {
        return 0;
    }
    i32 back = diode_input(world, pos, state);

    // A container behind a comparator replaces the signal entirely, and it is
    // read through one solid block: a chest with a wall in front of it still
    // reads. That second hop is a real rule and not a convenience.
    const BlockPos behind    = pos.offset(*facing);
    const i32      container = world.container_signal(behind);
    if (container >= 0) {
        back = container;
    } else if (back < 15 && signals_.is_conductor(world.block_at(behind))) {
        const i32 through = world.container_signal(behind.offset(*facing));
        if (through >= 0) {
            back = through;
        }
    }
    if (back == 0) {
        return 0;
    }

    const i32              side = diode_side_input(world, pos, state);
    const registry::BlockId block = blocks_->block_of(state);
    const auto mode = blocks_->find_property(block, "mode");
    const bool subtract =
        mode.has_value() && blocks_->property_value(state, *mode) == "subtract";
    if (subtract) {
        return std::max(0, back - side);
    }
    return side > back ? 0 : back;
}

i32 Redstone::container_reading(f32 fullness, bool any_items) noexcept {
    // floor(fill * 14) + 1 if anything at all. The +1 is why a single item in a
    // double chest reads 1 and not 0, and why a container is never at 14 unless
    // it is nearly full.
    const i32 scaled = static_cast<i32>(std::floor(fullness * 14.0F));
    return std::clamp(scaled + (any_items ? 1 : 0), 0, 15);
}

bool Redstone::consumer_powered(const RedstoneWorld& world, BlockPos pos) const {
    return signals_.has_neighbour_signal(world, pos);
}

// ── Ticks and updates ───────────────────────────────────────────────────────

bool Redstone::torch_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state) {
    const registry::BlockId block = blocks_->block_of(state);
    // A torch is powered off through the block it is stuck to, and through
    // nothing else. Every other neighbour is ignored, which is what lets a
    // torch sit inside a wire without turning itself off.
    Direction from = Direction::Up;
    if (block == ids_.wall_torch) {
        const auto facing = signals_.facing_of(state);
        from               = facing.has_value() ? *facing : Direction::Up;
    }
    const BlockPos support   = pos.offset(opposite(from));
    const bool     held_down = signals_.signal_at(world, support, opposite(from)) > 0;
    const bool     lit       = signals_.flag_of(state, "lit");

    if (lit) {
        if (!held_down) {
            return false;
        }
        world.set_block(pos, signals_.with_flag(state, "lit", false));
        if (torches_.record_and_check(pos, world.game_time())) {
            // Burnt out: it stays dark for a hundred and sixty ticks whatever
            // happens around it.
            wake(world, pos, block, 160, world::TickPriority::Normal);
        }
        return true;
    }
    if (held_down) {
        return false;
    }
    // Relighting does not add to the history — only going out does — so a torch
    // held off for a long time comes back without being counted.
    if (torches_.record_and_check(pos, world.game_time())) {
        return false;
    }
    world.set_block(pos, signals_.with_flag(state, "lit", true));
    return true;
}

bool Redstone::repeater_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state) {
    if (repeater_locked(world, pos, state)) {
        return false;
    }
    const bool powered   = signals_.flag_of(state, "powered");
    const bool should_on = diode_input(world, pos, state) > 0;
    if (powered && !should_on) {
        world.set_block(pos, signals_.with_flag(state, "powered", false));
        return true;
    }
    if (!powered) {
        world.set_block(pos, signals_.with_flag(state, "powered", true));
        if (!should_on) {
            // The input has already gone away: schedule the fall now, which is
            // what turns a one-tick pulse into a full-length one on the far
            // side of a repeater.
            const auto delay = blocks_->find_property(ids_.repeater, "delay");
            const i32  ticks =
                delay.has_value() ? 2 * (blocks_->property_index(state, *delay) + 1) : 2;
            wake(world, pos, ids_.repeater, ticks, world::TickPriority::VeryHigh);
        }
        return true;
    }
    return false;
}

bool Redstone::comparator_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state) {
    const i32  output  = comparator_output(world, pos, state);
    const bool powered = signals_.flag_of(state, "powered");
    if ((output > 0) == powered) {
        return false;
    }
    world.set_block(pos, signals_.with_flag(state, "powered", output > 0));
    return true;
}

bool Redstone::observer_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state) {
    const bool powered = signals_.flag_of(state, "powered");
    world.set_block(pos, signals_.with_flag(state, "powered", !powered));
    if (!powered) {
        // Two ticks on, then off again: the pulse an observer emits is fixed
        // and does not follow whatever it saw.
        wake(world, pos, ids_.observer, 2, world::TickPriority::Normal);
    }
    return true;
}

bool Redstone::scheduled_tick(RedstoneWorld& world, BlockPos pos, registry::BlockId block) {
    const registry::BlockStateId state = world.block_at(pos);
    if (blocks_->block_of(state) != block) {
        // The block was replaced between the request and the tick. Vanilla
        // drops the tick; so do we, rather than applying it to whatever is
        // there now.
        return false;
    }
    if (block == ids_.torch || block == ids_.wall_torch) {
        return torch_tick(world, pos, state);
    }
    if (block == ids_.repeater) {
        return repeater_tick(world, pos, state);
    }
    if (block == ids_.comparator) {
        return comparator_tick(world, pos, state);
    }
    if (block == ids_.observer) {
        return observer_tick(world, pos, state);
    }
    if (const SwitchRule* rule = switch_rule(block)) {
        return switch_tick(world, pos, *rule, state);
    }
    if (const ConsumerRule* rule = consumer_rule(block)) {
        const bool want = consumer_powered(world, pos) != rule->inverted;
        if (signals_.flag_of(state, rule->flag) != want) {
            world.set_block(pos, signals_.with_flag(state, rule->flag, want));
            return true;
        }
    }
    return false;
}

bool Redstone::scheduled_tick(RedstoneWorld& world, BlockPos pos, std::string_view what) {
    const auto block = blocks_->find_block(what);
    if (!block.has_value()) {
        return false;
    }
    return scheduled_tick(world, pos, *block);
}

bool Redstone::neighbour_changed(RedstoneWorld& world, BlockPos pos, BlockPos from) {
    (void)from;
    const registry::BlockStateId state = world.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);

    if (block == ids_.wire) {
        update_wire(world, pos);
        return true;
    }

    if (block == ids_.torch || block == ids_.wall_torch) {
        Direction face = Direction::Up;
        if (block == ids_.wall_torch) {
            face = signals_.facing_of(state).value_or(Direction::Up);
        }
        const BlockPos support = pos.offset(opposite(face));
        const bool     held    = signals_.signal_at(world, support, opposite(face)) > 0;
        if (signals_.flag_of(state, "lit") == held && !waking(world, pos, block)) {
            wake(world, pos, block, 2, world::TickPriority::Normal);
            return true;
        }
        return false;
    }

    if (block == ids_.repeater || block == ids_.comparator) {
        const bool comparator = block == ids_.comparator;
        if (!comparator && repeater_locked(world, pos, state)) {
            // A locked repeater does not even schedule. A change that arrives
            // while it is locked is dropped, not queued — which is the whole
            // point of locking it.
            return false;
        }
        const bool powered = signals_.flag_of(state, "powered");
        const bool want =
            comparator ? comparator_output(world, pos, state) > 0 : diode_input(world, pos, state) > 0;
        if (powered == want || waking(world, pos, block)) {
            return false;
        }
        i32 ticks = 2;
        if (!comparator) {
            const auto delay = blocks_->find_property(ids_.repeater, "delay");
            ticks = delay.has_value() ? 2 * (blocks_->property_index(state, *delay) + 1) : 2;
        }
        // Priority decides who wins inside one tick, and the two diodes do not
        // use the same scale. Both numbers came out of a real save's
        // `block_ticks`: a repeater turning on asked for -1, a comparator for 0.
        //
        // A repeater fed straight from another diode goes to -3 so it lands
        // before its neighbours; one turning *off* goes to -2 so it lands
        // before one turning on, which is what keeps two repeaters side by side
        // from swapping their answers. A comparator has only two steps: -1 when
        // a diode feeds it, 0 otherwise.
        const auto      facing = signals_.facing_of(state);
        registry::BlockId behind{0};
        if (facing.has_value()) {
            behind = blocks_->block_of(world.block_at(pos.offset(*facing)));
        }
        const bool fed_by_diode = behind == ids_.repeater || behind == ids_.comparator;

        world::TickPriority priority{};
        if (comparator) {
            priority = fed_by_diode ? world::TickPriority::High : world::TickPriority::Normal;
        } else if (fed_by_diode) {
            priority = world::TickPriority::ExtremelyHigh;
        } else if (powered) {
            priority = world::TickPriority::VeryHigh;
        } else {
            priority = world::TickPriority::High;
        }
        wake(world, pos, block, ticks, priority);
        return true;
    }

    // A note block reads the block **under** it for its instrument, and the
    // instrument lives in its own block state rather than in a sound: the
    // client plays whatever the state says. So changing the floor under a note
    // block has to rewrite the note block, and nothing did that before.
    //
    // Done before the consumer branch below, and with its own write, because
    // the two are independent: a note block can change instrument without
    // changing `powered`, and the `powered` branch returns early on no change.
    bool wrote_instrument = false;
    if (block == ids_.note_block && instrument_property_.has_value()) {
        const std::string_view want = note_instrument(world.block_at(pos.below()));
        const u16 have = blocks_->property_index(state, *instrument_property_);
        if (have < instrument_property_->values.size() &&
            instrument_property_->values[have] != want) {
            for (u16 i = 0; i < instrument_property_->values.size(); ++i) {
                if (instrument_property_->values[i] == want) {
                    world.set_block(pos, blocks_->with_property(state, *instrument_property_, i));
                    wrote_instrument = true;
                    break;
                }
            }
        }
    }

    if (const ConsumerRule* rule = consumer_rule(block)) {
        const registry::BlockStateId now  = wrote_instrument ? world.block_at(pos) : state;
        const bool                   want = consumer_powered(world, pos) != rule->inverted;
        const bool                   have = signals_.flag_of(now, rule->flag);
        if (want == have) {
            return wrote_instrument;
        }
        // The lamp is the one consumer with hysteresis: on at once, off four
        // ticks later, so a short pulse still leaves it visibly lit.
        if (rule->delay > 0 && (!rule->delay_off_only || have)) {
            if (!waking(world, pos, block)) {
                wake(world, pos, block, rule->delay, world::TickPriority::Normal);
            }
            return true;
        }
        world.set_block(pos, signals_.with_flag(now, rule->flag, want));
        return true;
    }

    return wrote_instrument;
}

}  // namespace ov::gameplay
