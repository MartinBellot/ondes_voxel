#include "ov/gameplay/piston.hpp"

#include <algorithm>
#include <array>

namespace ov::gameplay {
namespace {

/// What a piston does to each block, measured on a real 1.20.1 server.
///
/// A piston was pointed at every block in the game with a redstone block on its
/// head, and the three cells — piston, target, and the space beyond — read back
/// out of the save. 979 of the 987 candidates answered:
///
///     552 moved       126 refused and stopped the piston       301 vanished
///
/// The 301 needed a second pass. A block that is not there afterwards may have
/// been broken by the piston, or may have removed itself for its own reasons —
/// a sapling with no dirt under it, a door whose upper half was never placed,
/// coral out of water. Cross-checking against the blocks that failed to survive
/// standing alone in the conductor grid splits them: **200** are genuinely
/// broken by the push, and **101** are unmeasurable and named below rather than
/// quietly assumed.
///
/// Leaves are the surprise. They are a full cube, they are pushable-looking,
/// and a piston breaks them — which no rule about collision shapes would have
/// produced, and which is why these are tables and not a predicate.

constexpr std::array<std::string_view, 126> kImmovableNames{{
    "minecraft:acacia_hanging_sign",
    "minecraft:acacia_sign",
    "minecraft:acacia_wall_hanging_sign",
    "minecraft:acacia_wall_sign",
    "minecraft:anvil",
    "minecraft:bamboo_hanging_sign",
    "minecraft:bamboo_sign",
    "minecraft:bamboo_wall_hanging_sign",
    "minecraft:bamboo_wall_sign",
    "minecraft:barrel",
    "minecraft:barrier",
    "minecraft:beacon",
    "minecraft:bedrock",
    "minecraft:bee_nest",
    "minecraft:beehive",
    "minecraft:birch_hanging_sign",
    "minecraft:birch_sign",
    "minecraft:birch_wall_hanging_sign",
    "minecraft:birch_wall_sign",
    "minecraft:black_banner",
    "minecraft:black_wall_banner",
    "minecraft:blast_furnace",
    "minecraft:blue_banner",
    "minecraft:blue_wall_banner",
    "minecraft:brewing_stand",
    "minecraft:brown_banner",
    "minecraft:brown_wall_banner",
    "minecraft:calibrated_sculk_sensor",
    "minecraft:campfire",
    "minecraft:chain_command_block",
    "minecraft:cherry_hanging_sign",
    "minecraft:cherry_sign",
    "minecraft:cherry_wall_hanging_sign",
    "minecraft:cherry_wall_sign",
    "minecraft:chest",
    "minecraft:chipped_anvil",
    "minecraft:chiseled_bookshelf",
    "minecraft:command_block",
    "minecraft:conduit",
    "minecraft:crimson_hanging_sign",
    "minecraft:crimson_sign",
    "minecraft:crimson_wall_hanging_sign",
    "minecraft:crimson_wall_sign",
    "minecraft:crying_obsidian",
    "minecraft:cyan_banner",
    "minecraft:cyan_wall_banner",
    "minecraft:damaged_anvil",
    "minecraft:dark_oak_hanging_sign",
    "minecraft:dark_oak_sign",
    "minecraft:dark_oak_wall_hanging_sign",
    "minecraft:dark_oak_wall_sign",
    "minecraft:daylight_detector",
    "minecraft:dispenser",
    "minecraft:dropper",
    "minecraft:enchanting_table",
    "minecraft:end_portal_frame",
    "minecraft:ender_chest",
    "minecraft:furnace",
    "minecraft:gray_banner",
    "minecraft:gray_wall_banner",
    "minecraft:green_banner",
    "minecraft:green_wall_banner",
    "minecraft:grindstone",
    "minecraft:honey_block",
    "minecraft:hopper",
    "minecraft:jigsaw",
    "minecraft:jukebox",
    "minecraft:jungle_hanging_sign",
    "minecraft:jungle_sign",
    "minecraft:jungle_wall_hanging_sign",
    "minecraft:jungle_wall_sign",
    "minecraft:lectern",
    "minecraft:light",
    "minecraft:light_blue_banner",
    "minecraft:light_blue_wall_banner",
    "minecraft:light_gray_banner",
    "minecraft:light_gray_wall_banner",
    "minecraft:lime_banner",
    "minecraft:lime_wall_banner",
    "minecraft:lodestone",
    "minecraft:magenta_banner",
    "minecraft:magenta_wall_banner",
    "minecraft:mangrove_hanging_sign",
    "minecraft:mangrove_sign",
    "minecraft:mangrove_wall_hanging_sign",
    "minecraft:mangrove_wall_sign",
    "minecraft:oak_hanging_sign",
    "minecraft:oak_sign",
    "minecraft:oak_wall_hanging_sign",
    "minecraft:oak_wall_sign",
    "minecraft:obsidian",
    "minecraft:orange_banner",
    "minecraft:orange_wall_banner",
    "minecraft:pink_banner",
    "minecraft:pink_wall_banner",
    "minecraft:piston",
    "minecraft:piston_head",
    "minecraft:purple_banner",
    "minecraft:purple_wall_banner",
    "minecraft:red_banner",
    "minecraft:red_wall_banner",
    "minecraft:reinforced_deepslate",
    "minecraft:repeating_command_block",
    "minecraft:respawn_anchor",
    "minecraft:sculk_catalyst",
    "minecraft:sculk_sensor",
    "minecraft:sculk_shrieker",
    "minecraft:slime_block",
    "minecraft:smoker",
    "minecraft:soul_campfire",
    "minecraft:spawner",
    "minecraft:spruce_hanging_sign",
    "minecraft:spruce_sign",
    "minecraft:spruce_wall_hanging_sign",
    "minecraft:spruce_wall_sign",
    "minecraft:sticky_piston",
    "minecraft:structure_block",
    "minecraft:trapped_chest",
    "minecraft:warped_hanging_sign",
    "minecraft:warped_sign",
    "minecraft:warped_wall_hanging_sign",
    "minecraft:warped_wall_sign",
    "minecraft:white_banner",
    "minecraft:white_wall_banner",
    "minecraft:yellow_banner",
    "minecraft:yellow_wall_banner",
}};

constexpr std::array<std::string_view, 200> kDestroyNames{{
    "minecraft:acacia_button",
    "minecraft:acacia_leaves",
    "minecraft:acacia_pressure_plate",
    "minecraft:amethyst_cluster",
    "minecraft:azalea_leaves",
    "minecraft:bamboo_button",
    "minecraft:bamboo_pressure_plate",
    "minecraft:bell",
    "minecraft:big_dripleaf",
    "minecraft:birch_button",
    "minecraft:birch_leaves",
    "minecraft:birch_pressure_plate",
    "minecraft:black_bed",
    "minecraft:black_candle",
    "minecraft:black_candle_cake",
    "minecraft:black_shulker_box",
    "minecraft:blue_bed",
    "minecraft:blue_candle",
    "minecraft:blue_candle_cake",
    "minecraft:blue_shulker_box",
    "minecraft:brown_bed",
    "minecraft:brown_candle",
    "minecraft:brown_candle_cake",
    "minecraft:brown_shulker_box",
    "minecraft:budding_amethyst",
    "minecraft:cake",
    "minecraft:candle",
    "minecraft:candle_cake",
    "minecraft:carved_pumpkin",
    "minecraft:cherry_button",
    "minecraft:cherry_leaves",
    "minecraft:cherry_pressure_plate",
    "minecraft:cobweb",
    "minecraft:cocoa",
    "minecraft:comparator",
    "minecraft:creeper_head",
    "minecraft:creeper_wall_head",
    "minecraft:crimson_button",
    "minecraft:crimson_pressure_plate",
    "minecraft:cyan_bed",
    "minecraft:cyan_candle",
    "minecraft:cyan_candle_cake",
    "minecraft:cyan_shulker_box",
    "minecraft:dark_oak_button",
    "minecraft:dark_oak_leaves",
    "minecraft:dark_oak_pressure_plate",
    "minecraft:dead_brain_coral_wall_fan",
    "minecraft:dead_bubble_coral_wall_fan",
    "minecraft:dead_fire_coral_wall_fan",
    "minecraft:dead_horn_coral_wall_fan",
    "minecraft:dead_tube_coral_wall_fan",
    "minecraft:decorated_pot",
    "minecraft:dragon_egg",
    "minecraft:dragon_head",
    "minecraft:dragon_wall_head",
    "minecraft:flower_pot",
    "minecraft:flowering_azalea_leaves",
    "minecraft:gray_bed",
    "minecraft:gray_candle",
    "minecraft:gray_candle_cake",
    "minecraft:gray_shulker_box",
    "minecraft:green_bed",
    "minecraft:green_candle",
    "minecraft:green_candle_cake",
    "minecraft:green_shulker_box",
    "minecraft:heavy_weighted_pressure_plate",
    "minecraft:jack_o_lantern",
    "minecraft:jungle_button",
    "minecraft:jungle_leaves",
    "minecraft:jungle_pressure_plate",
    "minecraft:ladder",
    "minecraft:lantern",
    "minecraft:large_amethyst_bud",
    "minecraft:lever",
    "minecraft:light_blue_bed",
    "minecraft:light_blue_candle",
    "minecraft:light_blue_candle_cake",
    "minecraft:light_blue_shulker_box",
    "minecraft:light_gray_bed",
    "minecraft:light_gray_candle",
    "minecraft:light_gray_candle_cake",
    "minecraft:light_gray_shulker_box",
    "minecraft:light_weighted_pressure_plate",
    "minecraft:lime_bed",
    "minecraft:lime_candle",
    "minecraft:lime_candle_cake",
    "minecraft:lime_shulker_box",
    "minecraft:magenta_bed",
    "minecraft:magenta_candle",
    "minecraft:magenta_candle_cake",
    "minecraft:magenta_shulker_box",
    "minecraft:mangrove_button",
    "minecraft:mangrove_leaves",
    "minecraft:mangrove_pressure_plate",
    "minecraft:medium_amethyst_bud",
    "minecraft:melon",
    "minecraft:moss_block",
    "minecraft:moss_carpet",
    "minecraft:oak_button",
    "minecraft:oak_leaves",
    "minecraft:oak_pressure_plate",
    "minecraft:orange_bed",
    "minecraft:orange_candle",
    "minecraft:orange_candle_cake",
    "minecraft:orange_shulker_box",
    "minecraft:piglin_head",
    "minecraft:piglin_wall_head",
    "minecraft:pink_bed",
    "minecraft:pink_candle",
    "minecraft:pink_candle_cake",
    "minecraft:pink_shulker_box",
    "minecraft:player_head",
    "minecraft:player_wall_head",
    "minecraft:pointed_dripstone",
    "minecraft:polished_blackstone_button",
    "minecraft:polished_blackstone_pressure_plate",
    "minecraft:potted_acacia_sapling",
    "minecraft:potted_allium",
    "minecraft:potted_azalea_bush",
    "minecraft:potted_azure_bluet",
    "minecraft:potted_bamboo",
    "minecraft:potted_birch_sapling",
    "minecraft:potted_blue_orchid",
    "minecraft:potted_brown_mushroom",
    "minecraft:potted_cactus",
    "minecraft:potted_cherry_sapling",
    "minecraft:potted_cornflower",
    "minecraft:potted_crimson_fungus",
    "minecraft:potted_crimson_roots",
    "minecraft:potted_dandelion",
    "minecraft:potted_dark_oak_sapling",
    "minecraft:potted_dead_bush",
    "minecraft:potted_fern",
    "minecraft:potted_flowering_azalea_bush",
    "minecraft:potted_jungle_sapling",
    "minecraft:potted_lily_of_the_valley",
    "minecraft:potted_mangrove_propagule",
    "minecraft:potted_oak_sapling",
    "minecraft:potted_orange_tulip",
    "minecraft:potted_oxeye_daisy",
    "minecraft:potted_pink_tulip",
    "minecraft:potted_poppy",
    "minecraft:potted_red_mushroom",
    "minecraft:potted_red_tulip",
    "minecraft:potted_spruce_sapling",
    "minecraft:potted_torchflower",
    "minecraft:potted_warped_fungus",
    "minecraft:potted_warped_roots",
    "minecraft:potted_white_tulip",
    "minecraft:potted_wither_rose",
    "minecraft:pumpkin",
    "minecraft:purple_bed",
    "minecraft:purple_candle",
    "minecraft:purple_candle_cake",
    "minecraft:purple_shulker_box",
    "minecraft:red_bed",
    "minecraft:red_candle",
    "minecraft:red_candle_cake",
    "minecraft:red_shulker_box",
    "minecraft:redstone_torch",
    "minecraft:redstone_wall_torch",
    "minecraft:redstone_wire",
    "minecraft:repeater",
    "minecraft:scaffolding",
    "minecraft:sea_pickle",
    "minecraft:shulker_box",
    "minecraft:skeleton_skull",
    "minecraft:skeleton_wall_skull",
    "minecraft:small_amethyst_bud",
    "minecraft:snow",
    "minecraft:soul_lantern",
    "minecraft:soul_torch",
    "minecraft:soul_wall_torch",
    "minecraft:spruce_button",
    "minecraft:spruce_leaves",
    "minecraft:spruce_pressure_plate",
    "minecraft:stone_button",
    "minecraft:stone_pressure_plate",
    "minecraft:structure_void",
    "minecraft:suspicious_gravel",
    "minecraft:suspicious_sand",
    "minecraft:tripwire",
    "minecraft:tripwire_hook",
    "minecraft:turtle_egg",
    "minecraft:twisting_vines",
    "minecraft:wall_torch",
    "minecraft:warped_button",
    "minecraft:warped_pressure_plate",
    "minecraft:white_bed",
    "minecraft:white_candle",
    "minecraft:white_candle_cake",
    "minecraft:white_shulker_box",
    "minecraft:wither_skeleton_skull",
    "minecraft:wither_skeleton_wall_skull",
    "minecraft:yellow_bed",
    "minecraft:yellow_candle",
    "minecraft:yellow_candle_cake",
    "minecraft:yellow_shulker_box",
    "minecraft:zombie_head",
    "minecraft:zombie_wall_head",
}};

constexpr std::array<std::string_view, 101> kPushUnmeasuredNames{{
    "minecraft:acacia_door",
    "minecraft:acacia_sapling",
    "minecraft:allium",
    "minecraft:attached_melon_stem",
    "minecraft:attached_pumpkin_stem",
    "minecraft:azalea",
    "minecraft:azure_bluet",
    "minecraft:bamboo",
    "minecraft:bamboo_door",
    "minecraft:bamboo_sapling",
    "minecraft:beetroots",
    "minecraft:big_dripleaf_stem",
    "minecraft:birch_door",
    "minecraft:birch_sapling",
    "minecraft:blue_orchid",
    "minecraft:brain_coral",
    "minecraft:brain_coral_fan",
    "minecraft:brain_coral_wall_fan",
    "minecraft:brown_mushroom",
    "minecraft:bubble_coral",
    "minecraft:bubble_coral_fan",
    "minecraft:bubble_coral_wall_fan",
    "minecraft:cactus",
    "minecraft:carrots",
    "minecraft:cave_vines",
    "minecraft:cave_vines_plant",
    "minecraft:cherry_door",
    "minecraft:cherry_sapling",
    "minecraft:chorus_flower",
    "minecraft:chorus_plant",
    "minecraft:cornflower",
    "minecraft:crimson_door",
    "minecraft:crimson_fungus",
    "minecraft:crimson_roots",
    "minecraft:dandelion",
    "minecraft:dark_oak_door",
    "minecraft:dark_oak_sapling",
    "minecraft:dead_bush",
    "minecraft:fern",
    "minecraft:fire_coral",
    "minecraft:fire_coral_fan",
    "minecraft:fire_coral_wall_fan",
    "minecraft:flowering_azalea",
    "minecraft:glow_lichen",
    "minecraft:grass",
    "minecraft:hanging_roots",
    "minecraft:horn_coral",
    "minecraft:horn_coral_fan",
    "minecraft:horn_coral_wall_fan",
    "minecraft:iron_door",
    "minecraft:jungle_door",
    "minecraft:jungle_sapling",
    "minecraft:large_fern",
    "minecraft:lilac",
    "minecraft:lily_of_the_valley",
    "minecraft:lily_pad",
    "minecraft:mangrove_door",
    "minecraft:mangrove_propagule",
    "minecraft:melon_stem",
    "minecraft:nether_sprouts",
    "minecraft:nether_wart",
    "minecraft:oak_door",
    "minecraft:oak_sapling",
    "minecraft:orange_tulip",
    "minecraft:oxeye_daisy",
    "minecraft:peony",
    "minecraft:pink_petals",
    "minecraft:pink_tulip",
    "minecraft:pitcher_crop",
    "minecraft:pitcher_plant",
    "minecraft:poppy",
    "minecraft:potatoes",
    "minecraft:pumpkin_stem",
    "minecraft:red_mushroom",
    "minecraft:red_tulip",
    "minecraft:rose_bush",
    "minecraft:sculk_vein",
    "minecraft:small_dripleaf",
    "minecraft:spore_blossom",
    "minecraft:spruce_door",
    "minecraft:spruce_sapling",
    "minecraft:sugar_cane",
    "minecraft:sunflower",
    "minecraft:sweet_berry_bush",
    "minecraft:tall_grass",
    "minecraft:torch",
    "minecraft:torchflower",
    "minecraft:torchflower_crop",
    "minecraft:tube_coral",
    "minecraft:tube_coral_fan",
    "minecraft:tube_coral_wall_fan",
    "minecraft:twisting_vines_plant",
    "minecraft:vine",
    "minecraft:warped_door",
    "minecraft:warped_fungus",
    "minecraft:warped_roots",
    "minecraft:weeping_vines",
    "minecraft:weeping_vines_plant",
    "minecraft:wheat",
    "minecraft:white_tulip",
    "minecraft:wither_rose",
}};

/// The two blocks that stick to what they touch.
///
/// Their measured verdict is wrong for a reason the grid could not avoid: they
/// stuck to the floor the whole grid stands on.
constexpr std::array<std::string_view, 2> kPushOnly{"minecraft:slime_block",
                                                    "minecraft:honey_block"};

/// Blocks whose reading the probe itself spoiled. Named, not silently kept.
constexpr std::array<std::string_view, 2> kContaminated{"minecraft:piston",
                                                        "minecraft:sticky_piston"};

}  // namespace

Pistons::Pistons(const registry::BlockRegistry& blocks, const registry::Registries& registries,
                 const Redstone& redstone)
    : blocks_{&blocks}, redstone_{&redstone} {
    (void)registries;

    reactions_.assign(blocks.state_count(), PushReaction::Normal);

    std::vector<bool> immovable(blocks.block_count(), false);
    std::vector<bool> destroy(blocks.block_count(), false);
    std::vector<bool> unmeasured(blocks.block_count(), false);
    const auto        mark = [&](std::span<const std::string_view> names, std::vector<bool>& into) {
        for (const std::string_view name : names) {
            if (const auto id = blocks.find_block(name)) {
                into[id->value()] = true;
            }
        }
    };
    mark(kImmovableNames, immovable);
    mark(kDestroyNames, destroy);
    mark(kPushUnmeasuredNames, unmeasured);
    mark(kContaminated, unmeasured);

    // Three of the measured verdicts are the probe's fault rather than the
    // block's, and are corrected here rather than left to lie:
    //
    //   * Slime and honey stuck to the stone floor the grid stands on, so the
    //     push they were asked for was really a push of the whole plate and it
    //     failed on the twelve-block limit. They move; that is what the line
    //     scenario shows, where a slime block on a piston head does travel.
    //   * A piston in the target cell was quasi-powered by the redstone block
    //     that fires the piston pushing it — the cell above it touches that
    //     block — so it extended and became immovable. Which is a pretty proof
    //     that quasi-connectivity is real, and useless as a push reading.
    for (const std::string_view name : kPushOnly) {
        if (const auto id = blocks.find_block(name)) {
            immovable[id->value()] = false;
            destroy[id->value()]   = false;
        }
    }
    for (const std::string_view name : kContaminated) {
        if (const auto id = blocks.find_block(name)) {
            immovable[id->value()] = false;
            destroy[id->value()]   = false;
        }
    }

    slime_ = blocks.find_block("minecraft:slime_block").value_or(registry::BlockId{0});
    honey_ = blocks.find_block("minecraft:honey_block").value_or(registry::BlockId{0});
    for (const std::string_view name : kPushUnmeasuredNames) {
        unmeasured_.push_back(name);
    }
    for (const std::string_view name : kContaminated) {
        unmeasured_.push_back(name);
    }

    for (usize index = 0; index < blocks.state_count(); ++index) {
        const registry::BlockStateId state{static_cast<u16>(index)};
        const registry::BlockId      block = blocks.block_of(state);
        if (blocks.is_air(block)) {
            reactions_[index] = PushReaction::Ignore;
            continue;
        }
        if (immovable[block.value()]) {
            reactions_[index] = PushReaction::Block;
            continue;
        }
        if (destroy[block.value()]) {
            reactions_[index] = PushReaction::Destroy;
            continue;
        }
        // An extended piston refuses; a retracted one is an ordinary block.
        // Per state, so the same piston answers both ways over its own cycle.
        if (redstone.signals().flag_of(state, "extended")) {
            reactions_[index] = PushReaction::Block;
            continue;
        }
        if (!unmeasured[block.value()]) {
            continue;
        }
        // The 101 the probe could not read. They fall back to the shape rule —
        // a block with no collision box of its own breaks rather than moving —
        // and every one of them is listed by `unmeasured()` so a caller can say
        // which answers are inferred rather than seen.
        if (blocks.collision_boxes(state).empty() && !blocks.blocks_motion(block)) {
            reactions_[index] = PushReaction::Destroy;
        }
    }
}

PushReaction Pistons::reaction_of(registry::BlockStateId state) const noexcept {
    return state.value() < reactions_.size() ? reactions_[state.value()] : PushReaction::Block;
}

bool Pistons::is_sticky_block(registry::BlockStateId state) const noexcept {
    const registry::BlockId block = blocks_->block_of(state);
    return block == slime_ || block == honey_;
}

bool Pistons::wants_extended(const RedstoneWorld& world, BlockPos pos, Direction facing) const {
    const Signals& signals = redstone_->signals();

    // The piston's own ring, minus the face it points at: a block in front of a
    // piston never powers it, which is what stops a piston from holding itself
    // out through whatever it just pushed.
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto d = static_cast<Direction>(i);
        if (d == facing) {
            continue;
        }
        if (signals.signal_at(world, pos.offset(d), d) > 0) {
            return true;
        }
    }
    // The block directly below counts even when `facing` is down, because this
    // second pass does not skip it.
    if (signals.signal_at(world, pos.below(), Direction::Down) > 0) {
        return true;
    }

    // Quasi-connectivity: the ring around the block **above** the piston, minus
    // the one below it — which is the piston itself, and would otherwise let an
    // extended piston hold itself out forever.
    const BlockPos above = pos.above();
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto d = static_cast<Direction>(i);
        if (d == Direction::Down) {
            continue;
        }
        if (signals.signal_at(world, above.offset(d), d) > 0) {
            return true;
        }
    }
    return false;
}

bool Pistons::gather(const RedstoneWorld& world, BlockPos start, Direction facing,
                     PushPlan& plan) const {
    // Walk the straight line first, then everything slime pulls in sideways.
    // The order matters: vanilla's list is the line, then the branches, and the
    // twelve-block limit counts them all together.
    std::vector<BlockPos> line;
    BlockPos              cursor = start;
    while (true) {
        const registry::BlockStateId state = world.block_at(cursor);
        const PushReaction           how   = reaction_of(state);
        if (how == PushReaction::Ignore) {
            break;
        }
        if (how == PushReaction::Destroy) {
            plan.destroyed.push_back(MovingBlock{cursor, state});
            break;
        }
        if (how == PushReaction::Block) {
            plan.refusal = PushPlan::Refusal::Immovable;
            return false;
        }
        line.push_back(cursor);
        if (line.size() > kPistonPushLimit) {
            // Thirteen is not "push twelve": the whole push fails.
            plan.refusal = PushPlan::Refusal::TooMany;
            return false;
        }
        cursor = cursor.offset(facing);
    }

    for (const BlockPos& pos : line) {
        plan.moved.push_back(MovingBlock{pos, world.block_at(pos)});
    }

    // Sticky branches. Each moving slime or honey block drags its neighbours,
    // and those neighbours drag theirs, into the same twelve.
    for (usize index = 0; index < plan.moved.size(); ++index) {
        const MovingBlock entry = plan.moved[index];
        if (!is_sticky_block(entry.state)) {
            continue;
        }
        for (u8 i = 0; i < kDirectionCount; ++i) {
            const auto d = static_cast<Direction>(i);
            if (d == facing || d == opposite(facing)) {
                continue;
            }
            BlockPos branch = entry.from.offset(d);
            while (true) {
                const registry::BlockStateId state = world.block_at(branch);
                const PushReaction           how   = reaction_of(state);
                if (how == PushReaction::Ignore || how == PushReaction::Destroy) {
                    break;
                }
                if (how == PushReaction::Block) {
                    plan.refusal = PushPlan::Refusal::Immovable;
                    return false;
                }
                const bool already = std::ranges::any_of(
                    plan.moved, [&](const MovingBlock& m) { return m.from == branch; });
                if (already) {
                    break;
                }
                plan.moved.push_back(MovingBlock{branch, state});
                if (plan.moved.size() > kPistonPushLimit) {
                    plan.refusal = PushPlan::Refusal::TooMany;
                    return false;
                }
                // A branch only continues while it is itself sticky; a plain
                // block on the side of a slime block comes along but does not
                // recruit its own neighbours in that direction.
                if (!is_sticky_block(state)) {
                    break;
                }
                branch = branch.offset(d);
            }
        }
    }
    return true;
}

PushPlan Pistons::plan_push(const RedstoneWorld& world, BlockPos piston, Direction facing,
                            bool extending) const {
    PushPlan plan;
    if (!extending) {
        plan.possible = true;
        return plan;
    }
    const BlockPos first = piston.offset(facing);
    plan.possible        = gather(world, first, facing, plan);
    if (!plan.possible) {
        plan.moved.clear();
        plan.destroyed.clear();
    }
    return plan;
}

PushPlan Pistons::plan_pull(const RedstoneWorld& world, BlockPos piston, Direction facing) const {
    // Retracting a sticky piston pulls exactly one block: the one against the
    // head, two away from the body. It is one block and not a column, and that
    // is why a sticky piston cannot drag a stack back.
    PushPlan plan;
    plan.possible                      = true;
    const BlockPos               stuck = piston.offset(facing).offset(facing);
    const registry::BlockStateId state = world.block_at(stuck);
    const PushReaction           how   = reaction_of(state);
    if (how != PushReaction::Normal && how != PushReaction::PushOnly) {
        return plan;
    }
    PushPlan pulled;
    if (gather(world, stuck, opposite(facing), pulled) &&
        pulled.moved.size() <= kPistonPushLimit) {
        plan.moved = std::move(pulled.moved);
    }
    return plan;
}

bool Pistons::neighbour_changed(RedstoneWorld& world, BlockPos pos) {
    const registry::BlockStateId state = world.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);
    const Redstone::Ids&         ids   = redstone_->ids();
    if (block != ids.piston && block != ids.sticky_piston) {
        return false;
    }
    const Signals& signals = redstone_->signals();
    const auto     facing  = signals.facing_of(state);
    if (!facing.has_value()) {
        return false;
    }
    const bool extended = signals.flag_of(state, "extended");
    const bool wanted    = wants_extended(world, pos, *facing);
    if (extended == wanted || world.tick_scheduled(pos, block)) {
        return false;
    }
    // A piston takes a tick to make up its mind, which is what makes a piston
    // fed by a one-tick pulse do nothing at all.
    world.schedule_tick(pos, block, 1, world::TickPriority::Normal);
    return true;
}

bool Pistons::scheduled_tick(RedstoneWorld& world, BlockPos pos, registry::BlockId block) {
    const registry::BlockStateId state = world.block_at(pos);
    if (blocks_->block_of(state) != block) {
        return false;
    }
    const Redstone::Ids& ids = redstone_->ids();
    if (block != ids.piston && block != ids.sticky_piston) {
        return false;
    }
    const Signals& signals = redstone_->signals();
    const auto     facing  = signals.facing_of(state);
    if (!facing.has_value()) {
        return false;
    }
    const bool extended = signals.flag_of(state, "extended");
    const bool wanted   = wants_extended(world, pos, *facing);
    if (extended == wanted) {
        return false;
    }

    const bool sticky = block == ids.sticky_piston;

    if (wanted) {
        const PushPlan plan = plan_push(world, pos, *facing, true);
        if (!plan.possible) {
            // A refused push is not an error and not a retry: the piston simply
            // stays where it is until something changes.
            return false;
        }
        // Furthest first, so nothing is written into a cell another block is
        // still leaving.
        for (auto it = plan.destroyed.rbegin(); it != plan.destroyed.rend(); ++it) {
            world.set_block(it->from, blocks_->default_state(ids.air));
        }
        for (auto it = plan.moved.rbegin(); it != plan.moved.rend(); ++it) {
            world.set_block(it->from.offset(*facing), it->state);
            world.set_block(it->from, blocks_->default_state(ids.air));
        }
        world.set_block(pos, signals.with_flag(state, "extended", true));

        // The head. A `moving_piston` with its block entity is what vanilla
        // puts here for the two ticks the arm takes; the settled state is the
        // head itself, and that is what a save has to hold.
        registry::BlockStateId head = blocks_->default_state(ids.piston_head);
        if (const auto prop = blocks_->find_property(ids.piston_head, "facing")) {
            for (u16 v = 0; v < prop->values.size(); ++v) {
                if (prop->values[v] == direction_name(*facing)) {
                    head = blocks_->with_property(head, *prop, v);
                    break;
                }
            }
        }
        if (const auto prop = blocks_->find_property(ids.piston_head, "type")) {
            for (u16 v = 0; v < prop->values.size(); ++v) {
                if (prop->values[v] == (sticky ? "sticky" : "normal")) {
                    head = blocks_->with_property(head, *prop, v);
                    break;
                }
            }
        }
        world.set_block(pos.offset(*facing), head);
        return true;
    }

    // Retracting. The head goes first, then whatever a sticky piston brings
    // back with it.
    world.set_block(pos, signals.with_flag(state, "extended", false));
    world.set_block(pos.offset(*facing), blocks_->default_state(ids.air));
    if (sticky) {
        const PushPlan plan = plan_pull(world, pos, *facing);
        for (const MovingBlock& entry : plan.moved) {
            world.set_block(entry.from.offset(opposite(*facing)), entry.state);
            world.set_block(entry.from, blocks_->default_state(ids.air));
        }
    }
    return true;
}

}  // namespace ov::gameplay
