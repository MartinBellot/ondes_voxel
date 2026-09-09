#include "plots.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ov::lab {
namespace {

constexpr i32 kGround = Canvas::kGroundY;

/// The first buildable level: a plot builds upwards from the grass.
constexpr i32 kFloor = kGround + 1;

/// Plot origins are this far apart, so a 16x16 plot has a 16-block road on two
/// sides of it. Walking between benches is part of using them.
constexpr i32 kPitch = 32;

void plot_sign(Canvas& canvas, i32 x, i32 z, std::string_view zone, std::string_view name) {
    const std::array<std::string, 4> lines{std::string{"-- "} + std::string{zone} + " --",
                                           std::string{name}, std::string{}, std::string{}};
    canvas.sign(x, kFloor, z, lines);
}

// ── Redstone ────────────────────────────────────────────────────────────────

/// A source, sixteen blocks of dust, and a lamp. The reference measurement is
/// the level at every position: 15 next to the source, 0 past the sixteenth.
void redstone_dust_line(Canvas& canvas, i32 x, i32 z) {
    canvas.set(x + 1, kFloor, z + 8, "minecraft:redstone_block");
    for (i32 step = 2; step <= 14; ++step) {
        canvas.set(x + step, kFloor, z + 8, "minecraft:redstone_wire");
    }
    canvas.set(x + 15, kFloor, z + 8, "minecraft:redstone_lamp");

    // A second line, this one driven by a lever, because a lever and a block of
    // redstone do not power a wire the same way and the difference is the
    // point.
    canvas.set(x + 1, kFloor, z + 12, "minecraft:stone");
    canvas.set(x + 1, kFloor + 1, z + 12, "minecraft:lever",
               {{"face", "floor"}, {"facing", "north"}, {"powered", "false"}});
    for (i32 step = 2; step <= 15; ++step) {
        canvas.set(x + step, kFloor, z + 12, "minecraft:redstone_wire");
    }
}

/// Four lines, one per repeater delay. What is measured is ticks, not levels.
void redstone_repeater_delays(Canvas& canvas, i32 x, i32 z) {
    for (i32 delay = 1; delay <= 4; ++delay) {
        const i32         lane        = z + 2 + (delay - 1) * 3;
        const std::string delay_value = std::to_string(delay);
        canvas.set(x + 1, kFloor, lane, "minecraft:redstone_block");
        canvas.set(x + 2, kFloor, lane, "minecraft:redstone_wire");
        canvas.set(x + 3, kFloor, lane, "minecraft:repeater",
                   {{"delay", delay_value}, {"facing", "west"}, {"locked", "false"}});
        for (i32 step = 4; step <= 13; ++step) {
            canvas.set(x + step, kFloor, lane, "minecraft:redstone_wire");
        }
        canvas.set(x + 14, kFloor, lane, "minecraft:redstone_lamp");
    }
}

/// A repeater fed from the side by another repeater: the locked case, whose
/// state the game stores in the block itself.
void redstone_locked_repeater(Canvas& canvas, i32 x, i32 z) {
    canvas.set(x + 1, kFloor, z + 8, "minecraft:redstone_block");
    canvas.set(x + 2, kFloor, z + 8, "minecraft:redstone_wire");
    canvas.set(x + 3, kFloor, z + 8, "minecraft:repeater",
               {{"delay", "1"}, {"facing", "west"}, {"locked", "false"}});
    canvas.set(x + 4, kFloor, z + 8, "minecraft:redstone_wire");
    canvas.set(x + 5, kFloor, z + 8, "minecraft:redstone_lamp");

    // The locker: perpendicular, pointing into the repeater's side.
    canvas.set(x + 3, kFloor, z + 10, "minecraft:redstone_block");
    canvas.set(x + 3, kFloor, z + 9, "minecraft:repeater",
               {{"delay", "1"}, {"facing", "north"}, {"locked", "false"}});
}

/// Comparison, subtraction, and a container to read. The container is the case
/// that cannot be guessed: the signal is a function of how full it is and of
/// each stack's own maximum size.
void redstone_comparators(Canvas& canvas, i32 x, i32 z) {
    canvas.set(x + 1, kFloor, z + 3, "minecraft:redstone_block");
    canvas.set(x + 2, kFloor, z + 3, "minecraft:comparator",
               {{"facing", "west"}, {"mode", "compare"}, {"powered", "false"}});
    canvas.set(x + 3, kFloor, z + 3, "minecraft:redstone_wire");
    canvas.set(x + 4, kFloor, z + 3, "minecraft:redstone_lamp");

    canvas.set(x + 1, kFloor, z + 7, "minecraft:redstone_block");
    canvas.set(x + 2, kFloor, z + 7, "minecraft:comparator",
               {{"facing", "west"}, {"mode", "subtract"}, {"powered", "false"}});
    canvas.set(x + 3, kFloor, z + 7, "minecraft:redstone_wire");
    canvas.set(x + 4, kFloor, z + 7, "minecraft:redstone_lamp");

    // Three containers whose fullness is meant to be varied by hand.
    canvas.container(x + 1, kFloor, z + 11, "minecraft:barrel", "minecraft:barrel",
                     {{"facing", "up"}, {"open", "false"}});
    canvas.set(x + 2, kFloor, z + 11, "minecraft:comparator",
               {{"facing", "west"}, {"mode", "compare"}, {"powered", "false"}});
    canvas.set(x + 3, kFloor, z + 11, "minecraft:redstone_wire");
    canvas.set(x + 4, kFloor, z + 11, "minecraft:redstone_lamp");

    canvas.container(x + 8, kFloor, z + 11, "minecraft:chest", "minecraft:chest",
                     {{"facing", "north"}, {"type", "single"}});
    canvas.set(x + 9, kFloor, z + 11, "minecraft:comparator",
               {{"facing", "west"}, {"mode", "compare"}, {"powered", "false"}});
    canvas.set(x + 10, kFloor, z + 11, "minecraft:redstone_wire");
    canvas.set(x + 11, kFloor, z + 11, "minecraft:redstone_lamp");
}

/// A torch feeding itself: eight changes in sixty ticks and it burns out. The
/// rig is here because burn-out is a rule with no data file behind it.
void redstone_torch_burnout(Canvas& canvas, i32 x, i32 z) {
    canvas.set(x + 4, kFloor, z + 8, "minecraft:stone");
    canvas.set(x + 4, kFloor + 1, z + 8, "minecraft:redstone_torch", {{"lit", "true"}});
    canvas.set(x + 4, kFloor + 2, z + 8, "minecraft:redstone_wire");
    canvas.set(x + 4, kFloor + 2, z + 7, "minecraft:redstone_wire");
    canvas.set(x + 4, kFloor + 1, z + 7, "minecraft:stone");
    canvas.set(x + 4, kFloor, z + 7, "minecraft:redstone_wire");
    canvas.set(x + 4, kFloor, z + 6, "minecraft:redstone_lamp");
}

/// Twelve blocks and thirteen: the first extends, the second must refuse.
void redstone_piston_limit(Canvas& canvas, i32 x, i32 z) {
    for (i32 lane = 0; lane < 2; ++lane) {
        const i32 line   = z + 4 + lane * 6;
        const i32 length = lane == 0 ? 12 : 13;
        canvas.set(x + 1, kFloor, line, "minecraft:piston",
                   {{"extended", "false"}, {"facing", "east"}});
        for (i32 step = 0; step < length; ++step) {
            canvas.set(x + 2 + step, kFloor, line, "minecraft:stone");
        }
        canvas.set(x + 1, kFloor + 1, line, "minecraft:stone");
        canvas.set(x + 1, kFloor + 2, line, "minecraft:lever",
                   {{"face", "floor"}, {"facing", "north"}, {"powered", "false"}});
    }
}

/// The piston that fires from the block above it. A bug the game kept, so
/// parity requires keeping it too.
void redstone_quasi_connectivity(Canvas& canvas, i32 x, i32 z) {
    canvas.set(x + 4, kFloor, z + 8, "minecraft:sticky_piston",
               {{"extended", "false"}, {"facing", "east"}});
    canvas.set(x + 5, kFloor, z + 8, "minecraft:oak_planks");
    // One above the piston, not touching it: the whole point.
    canvas.set(x + 4, kFloor + 2, z + 8, "minecraft:stone");
    canvas.set(x + 4, kFloor + 3, z + 8, "minecraft:lever",
               {{"face", "floor"}, {"facing", "north"}, {"powered", "false"}});
}

/// Observer, hoppers, dispenser and dropper: the components whose behaviour is
/// about items and events rather than levels.
void redstone_transport(Canvas& canvas, i32 x, i32 z) {
    canvas.set(x + 2, kFloor, z + 3, "minecraft:observer",
               {{"facing", "east"}, {"powered", "false"}});
    canvas.set(x + 3, kFloor, z + 3, "minecraft:oak_planks");
    canvas.set(x + 1, kFloor, z + 3, "minecraft:redstone_lamp");

    canvas.container(x + 2, kFloor + 2, z + 7, "minecraft:chest", "minecraft:chest",
                     {{"facing", "north"}, {"type", "single"}});
    for (i32 step = 0; step < 5; ++step) {
        canvas.container(x + 3 + step, kFloor + 2, z + 7, "minecraft:hopper", "minecraft:hopper",
                         {{"facing", "west"}});
    }
    canvas.fill(x + 2, kFloor, z + 7, x + 8, kFloor + 1, z + 7, "minecraft:stone");

    canvas.container(x + 2, kFloor, z + 11, "minecraft:dispenser", "minecraft:dispenser",
                     {{"facing", "east"}, {"triggered", "false"}});
    canvas.container(x + 6, kFloor, z + 11, "minecraft:dropper", "minecraft:dropper",
                     {{"facing", "east"}, {"triggered", "false"}});
    canvas.set(x + 2, kFloor + 1, z + 11, "minecraft:lever",
               {{"face", "floor"}, {"facing", "north"}, {"powered", "false"}});
}

// ── Fluides ─────────────────────────────────────────────────────────────────

/// One source in an open basin. What is measured is the shape of the puddle:
/// the level at every position, not how far it went.
void fluid_basin(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 2, kFloor, z + 2, x + 14, kFloor, z + 14, "minecraft:stone");
    canvas.shell(x + 2, kFloor + 1, z + 2, x + 14, kFloor + 1, z + 14, "minecraft:stone");
    canvas.fill(x + 3, kFloor + 1, z + 3, x + 13, kFloor + 1, z + 13, "minecraft:air");
    canvas.set(x + 8, kFloor + 1, z + 8, "minecraft:water", {{"level", "0"}});
}

/// A wall with the gap off to one side. Water that spreads evenly fails this;
/// water that searches for the hole passes it.
void fluid_hole_seeking(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 1, kFloor, z + 1, x + 15, kFloor, z + 15, "minecraft:stone");
    canvas.shell(x + 1, kFloor + 1, z + 1, x + 15, kFloor + 1, z + 15, "minecraft:stone");
    canvas.fill(x + 2, kFloor + 1, z + 2, x + 14, kFloor + 1, z + 14, "minecraft:air");
    // The hole, deliberately off-centre.
    canvas.set(x + 12, kFloor, z + 4, "minecraft:air");
    canvas.set(x + 12, kFloor - 1, z + 4, "minecraft:air");
    canvas.set(x + 4, kFloor + 1, z + 8, "minecraft:water", {{"level", "0"}});
}

/// The four geometries that decide between stone, cobblestone and obsidian.
void fluid_mixing(Canvas& canvas, i32 x, i32 z) {
    for (i32 case_index = 0; case_index < 4; ++case_index) {
        const i32 base = x + 2 + case_index * 3;
        canvas.fill(base, kFloor, z + 4, base + 1, kFloor, z + 9, "minecraft:stone");
        canvas.shell(base - 1, kFloor + 1, z + 3, base + 2, kFloor + 3, z + 10, "minecraft:stone");
        canvas.fill(base, kFloor + 1, z + 4, base + 1, kFloor + 3, z + 9, "minecraft:air");
    }
    // Left to be filled by hand, or by a probe: the point is the geometry, and
    // pre-placing the fluids would let them mix before anyone watched.
}

/// Blocks that hold water in their own state rather than beside it.
void fluid_waterlogging(Canvas& canvas, i32 x, i32 z) {
    const std::array<std::string_view, 8> holders{
        "minecraft:oak_slab",       "minecraft:oak_stairs", "minecraft:oak_fence",
        "minecraft:oak_trapdoor",   "minecraft:glass_pane", "minecraft:ladder",
        "minecraft:cobblestone_wall", "minecraft:chain"};
    for (usize index = 0; index < holders.size(); ++index) {
        const i32 column = x + 2 + static_cast<i32>(index) * 2;
        canvas.set(column, kFloor, z + 8, holders[index], {{"waterlogged", "true"}});
        canvas.set(column, kFloor, z + 12, holders[index], {{"waterlogged", "false"}});
    }
}

/// Two sources with one gap: the overworld makes a third, the Nether does not.
void fluid_infinite_source(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 4, kFloor, z + 6, x + 10, kFloor, z + 10, "minecraft:stone");
    canvas.shell(x + 4, kFloor + 1, z + 6, x + 10, kFloor + 1, z + 10, "minecraft:stone");
    canvas.fill(x + 5, kFloor + 1, z + 7, x + 9, kFloor + 1, z + 9, "minecraft:air");
    canvas.set(x + 5, kFloor + 1, z + 8, "minecraft:water", {{"level", "0"}});
    canvas.set(x + 7, kFloor + 1, z + 8, "minecraft:water", {{"level", "0"}});
}

/// A pool and a sponge beside it. Radius seven, sixty-five blocks at most.
void fluid_sponge(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 2, kFloor - 3, z + 2, x + 14, kFloor - 1, z + 14, "minecraft:water",
                {{"level", "0"}});
    canvas.fill(x + 2, kFloor, z + 2, x + 14, kFloor, z + 14, "minecraft:air");
    canvas.set(x + 8, kFloor + 1, z + 8, "minecraft:sponge");
}


// ── Physique et dégâts ──────────────────────────────────────────────────────

/// Fifteen pillars, one block taller each. Jumping off the n-th is a fall of n,
/// which is the only way to measure a damage curve without trusting a table.
void physics_fall_heights(Canvas& canvas, i32 x, i32 z) {
    for (i32 height = 1; height <= 15; ++height) {
        canvas.fill(x + height, kFloor, z + 8, x + height, kFloor + height - 1, z + 8,
                    "minecraft:stone");
        canvas.set(x + height, kFloor + height, z + 8, "minecraft:white_wool");
    }
    // A tall one with a ladder, for the far end of the curve.
    canvas.fill(x + 2, kFloor, z + 2, x + 2, kFloor + 40, z + 2, "minecraft:stone");
    for (i32 rung = 0; rung <= 40; ++rung) {
        canvas.set(x + 3, kFloor + rung, z + 2, "minecraft:ladder", {{"facing", "east"}});
    }
    canvas.set(x + 2, kFloor + 41, z + 2, "minecraft:white_wool");
}

/// Ten lanes of ground, one surface each. Walking them is the measurement.
void physics_surfaces(Canvas& canvas, i32 x, i32 z) {
    const std::array<std::string_view, 8> lanes{
        "minecraft:ice",       "minecraft:packed_ice", "minecraft:blue_ice",
        "minecraft:slime_block", "minecraft:honey_block", "minecraft:soul_sand",
        "minecraft:soul_soil", "minecraft:mud"};
    for (usize index = 0; index < lanes.size(); ++index) {
        const i32 lane = z + 1 + static_cast<i32>(index) * 2;
        canvas.fill(x + 1, kFloor, lane, x + 15, kFloor, lane, lanes[index]);
    }
    // Cobweb and scaffolding are not surfaces, they are what movement happens
    // *inside*.
    canvas.fill(x + 1, kFloor + 1, z + 15, x + 5, kFloor + 3, z + 15, "minecraft:cobweb");
    canvas.fill(x + 8, kFloor + 1, z + 15, x + 8, kFloor + 8, z + 15, "minecraft:scaffolding",
                {{"bottom", "false"}, {"distance", "0"}, {"waterlogged", "false"}});
}

/// The blocks that hurt on contact, and the ones that hurt over time.
void physics_hazards(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 2, kFloor, z + 2, x + 2, kFloor, z + 6, "minecraft:sand");
    for (i32 step = 0; step < 5; ++step) {
        canvas.set(x + 2, kFloor + 1, z + 2 + step, "minecraft:cactus", {{"age", "0"}});
    }
    canvas.set(x + 5, kFloor + 1, z + 3, "minecraft:sweet_berry_bush", {{"age", "3"}});
    canvas.fill(x + 8, kFloor, z + 2, x + 10, kFloor, z + 4, "minecraft:magma_block");
    canvas.set(x + 12, kFloor + 1, z + 3, "minecraft:campfire",
               {{"facing", "north"}, {"lit", "true"}, {"signal_fire", "false"}, {"waterlogged", "false"}});
    canvas.set(x + 14, kFloor + 1, z + 3, "minecraft:wither_rose");

    // A lava pool with a walkway, and a water column to break a fall.
    canvas.fill(x + 2, kFloor, z + 9, x + 8, kFloor, z + 14, "minecraft:lava", {{"level", "0"}});
    canvas.fill(x + 5, kFloor + 1, z + 9, x + 5, kFloor + 1, z + 14, "minecraft:oak_planks");
    canvas.fill(x + 11, kFloor, z + 9, x + 13, kFloor, z + 11, "minecraft:water", {{"level", "0"}});
}

// ── Lumière ─────────────────────────────────────────────────────────────────

/// A sealed room. Sky light must not reach it and block light must be whatever
/// is put inside, which is what makes it the right place to measure emission.
void light_dark_room(Canvas& canvas, i32 x, i32 z) {
    canvas.shell(x + 2, kFloor, z + 2, x + 13, kFloor + 4, z + 13, "minecraft:stone");
    canvas.fill(x + 3, kFloor + 1, z + 3, x + 12, kFloor + 3, z + 12, "minecraft:air");
    canvas.set(x + 2, kFloor + 1, z + 8, "minecraft:oak_door",
               {{"facing", "east"}, {"half", "lower"}, {"hinge", "left"}, {"open", "false"},
                {"powered", "false"}});
    canvas.set(x + 2, kFloor + 2, z + 8, "minecraft:oak_door",
               {{"facing", "east"}, {"half", "upper"}, {"hinge", "left"}, {"open", "false"},
                {"powered", "false"}});
}

/// A roofed corridor with one torch at the end: the light curve, in place.
void light_gradient(Canvas& canvas, i32 x, i32 z) {
    canvas.shell(x + 1, kFloor, z + 6, x + 15, kFloor + 3, z + 10, "minecraft:stone");
    canvas.fill(x + 2, kFloor + 1, z + 7, x + 14, kFloor + 2, z + 9, "minecraft:air");
    canvas.set(x + 2, kFloor + 1, z + 8, "minecraft:torch");
}

/// Every light source the game has, side by side, each on its own pedestal.
void light_emitters(Canvas& canvas, i32 x, i32 z) {
    const std::array<std::string_view, 14> sources{
        "minecraft:torch",          "minecraft:glowstone",     "minecraft:sea_lantern",
        "minecraft:shroomlight",    "minecraft:ochre_froglight", "minecraft:verdant_froglight",
        "minecraft:pearlescent_froglight", "minecraft:jack_o_lantern", "minecraft:lantern",
        "minecraft:end_rod",        "minecraft:crying_obsidian", "minecraft:magma_block",
        "minecraft:beacon",         "minecraft:conduit"};
    for (usize index = 0; index < sources.size(); ++index) {
        const i32 column = x + 1 + static_cast<i32>(index);
        canvas.set(column, kFloor, z + 8, "minecraft:polished_andesite");
        canvas.set(column, kFloor + 1, z + 8, sources[index]);
    }
    // A lit redstone lamp needs its own state rather than a neighbour.
    canvas.set(x + 1, kFloor, z + 11, "minecraft:polished_andesite");
    canvas.set(x + 1, kFloor + 1, z + 11, "minecraft:redstone_lamp", {{"lit", "true"}});
}

/// A roof with a single hole in it: sky light falling down a shaft, which is
/// the case a per-column heightmap gets wrong and a flood fill gets right.
void light_shaft(Canvas& canvas, i32 x, i32 z) {
    canvas.shell(x + 2, kFloor, z + 2, x + 13, kFloor + 6, z + 13, "minecraft:stone");
    canvas.fill(x + 3, kFloor + 1, z + 3, x + 12, kFloor + 5, z + 12, "minecraft:air");
    canvas.set(x + 8, kFloor + 6, z + 8, "minecraft:air");
}

// ── Formes et connexions ────────────────────────────────────────────────────

/// Four facings by two halves, each with a neighbour that changes its shape.
void shapes_stairs(Canvas& canvas, i32 x, i32 z) {
    const std::array<std::string_view, 4> facings{"north", "south", "east", "west"};
    const std::array<std::string_view, 2> halves{"bottom", "top"};
    for (usize facing = 0; facing < facings.size(); ++facing) {
        for (usize half = 0; half < halves.size(); ++half) {
            const i32 column = x + 2 + static_cast<i32>(facing) * 3;
            const i32 row    = z + 2 + static_cast<i32>(half) * 6;
            canvas.set(column, kFloor, row, "minecraft:oak_stairs",
                       {{"facing", facings[facing]},
                        {"half", halves[half]},
                        {"shape", "straight"},
                        {"waterlogged", "false"}});
            // The neighbour that turns a straight stair into a corner.
            canvas.set(column, kFloor, row + 2, "minecraft:oak_stairs",
                       {{"facing", facings[facing]},
                        {"half", halves[half]},
                        {"shape", "straight"},
                        {"waterlogged", "false"}});
            canvas.set(column + 1, kFloor, row + 2, "minecraft:oak_stairs",
                       {{"facing", "east"},
                        {"half", halves[half]},
                        {"shape", "straight"},
                        {"waterlogged", "false"}});
        }
    }
}

/// Walls, with and without a block above: `tall` is decided by what is on top,
/// and the pole appears and disappears with the symmetry of the connections.
void shapes_walls(Canvas& canvas, i32 x, i32 z) {
    for (i32 index = 0; index < 6; ++index) {
        const i32 column = x + 2 + index * 2;
        canvas.set(column, kFloor, z + 4, "minecraft:cobblestone_wall");
        canvas.set(column, kFloor, z + 5, "minecraft:cobblestone_wall");
        if (index % 2 == 1) {
            canvas.set(column, kFloor + 1, z + 4, "minecraft:stone");
        }
    }
    // A full cross, and a straight line: the two cases the pole distinguishes.
    canvas.set(x + 8, kFloor, z + 10, "minecraft:cobblestone_wall");
    canvas.set(x + 7, kFloor, z + 10, "minecraft:cobblestone_wall");
    canvas.set(x + 9, kFloor, z + 10, "minecraft:cobblestone_wall");
    canvas.set(x + 8, kFloor, z + 9, "minecraft:cobblestone_wall");
    canvas.set(x + 8, kFloor, z + 11, "minecraft:cobblestone_wall");
    canvas.fill(x + 2, kFloor, z + 13, x + 6, kFloor, z + 13, "minecraft:cobblestone_wall");
}

/// Fences, gates, panes and bars: what connects to what, in both directions.
void shapes_connections(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 2, kFloor, z + 3, x + 8, kFloor, z + 3, "minecraft:oak_fence");
    canvas.set(x + 5, kFloor, z + 3, "minecraft:oak_fence_gate",
               {{"facing", "north"}, {"in_wall", "false"}, {"open", "false"}, {"powered", "false"}});
    canvas.fill(x + 2, kFloor, z + 7, x + 8, kFloor, z + 7, "minecraft:glass_pane");
    canvas.fill(x + 2, kFloor, z + 11, x + 8, kFloor, z + 11, "minecraft:iron_bars");

    // The measured case: a low slab does not hold a fence, the same slab
    // doubled does.
    canvas.set(x + 11, kFloor, z + 3, "minecraft:oak_slab",
               {{"type", "bottom"}, {"waterlogged", "false"}});
    canvas.set(x + 12, kFloor, z + 3, "minecraft:oak_fence");
    canvas.set(x + 11, kFloor, z + 7, "minecraft:oak_slab",
               {{"type", "double"}, {"waterlogged", "false"}});
    canvas.set(x + 12, kFloor, z + 7, "minecraft:oak_fence");
}

/// Doors and trapdoors, both halves and both hinges.
void shapes_doors(Canvas& canvas, i32 x, i32 z) {
    const std::array<std::string_view, 2> hinges{"left", "right"};
    for (usize hinge = 0; hinge < hinges.size(); ++hinge) {
        const i32 column = x + 3 + static_cast<i32>(hinge) * 4;
        canvas.set(column, kFloor, z + 5, "minecraft:oak_door",
                   {{"facing", "north"}, {"half", "lower"}, {"hinge", hinges[hinge]},
                    {"open", "false"}, {"powered", "false"}});
        canvas.set(column, kFloor + 1, z + 5, "minecraft:oak_door",
                   {{"facing", "north"}, {"half", "upper"}, {"hinge", hinges[hinge]},
                    {"open", "false"}, {"powered", "false"}});
    }
    const std::array<std::string_view, 2> halves{"bottom", "top"};
    for (usize half = 0; half < halves.size(); ++half) {
        canvas.set(x + 3 + static_cast<i32>(half) * 4, kFloor, z + 10, "minecraft:oak_trapdoor",
                   {{"facing", "north"}, {"half", halves[half]}, {"open", "false"},
                    {"powered", "false"}, {"waterlogged", "false"}});
    }
}

// ── Postes de travail et conteneurs ─────────────────────────────────────────

void stations_workbenches(Canvas& canvas, i32 x, i32 z) {
    const std::array<std::string_view, 12> stations{
        "minecraft:crafting_table", "minecraft:furnace",   "minecraft:blast_furnace",
        "minecraft:smoker",         "minecraft:anvil",     "minecraft:grindstone",
        "minecraft:stonecutter",    "minecraft:smithing_table", "minecraft:loom",
        "minecraft:cartography_table", "minecraft:fletching_table", "minecraft:lectern"};
    // Only some of these are containers. A crafting table has no block entity
    // at all, and giving it one would be as wrong as leaving the furnace
    // without.
    const auto entity_for = [](std::string_view station) -> std::string_view {
        if (station == "minecraft:furnace" || station == "minecraft:blast_furnace" ||
            station == "minecraft:smoker" || station == "minecraft:lectern") {
            return station;
        }
        return {};
    };
    for (usize index = 0; index < stations.size(); ++index) {
        const i32              column = x + 2 + static_cast<i32>(index % 6) * 2;
        const i32              row    = z + 4 + static_cast<i32>(index / 6) * 3;
        const std::string_view entity = entity_for(stations[index]);
        if (entity.empty()) {
            canvas.set(column, kFloor, row, stations[index]);
        } else {
            canvas.container(column, kFloor, row, stations[index], entity);
        }
    }
    // An enchanting table with the fifteen shelves that raise its levels.
    canvas.container(x + 8, kFloor, z + 12, "minecraft:enchanting_table",
                     "minecraft:enchanting_table");
    for (i32 index = 0; index < 15; ++index) {
        const i32 offset = index % 5;
        const i32 ring   = index / 5;
        canvas.set(x + 6 + offset, kFloor + ring, z + 10, "minecraft:bookshelf");
    }
    canvas.container(x + 12, kFloor, z + 12, "minecraft:brewing_stand", "minecraft:brewing_stand",
                     {{"has_bottle_0", "false"}, {"has_bottle_1", "false"}, {"has_bottle_2", "false"}});
    canvas.set(x + 14, kFloor, z + 12, "minecraft:cauldron");
}

void stations_containers(Canvas& canvas, i32 x, i32 z) {
    canvas.container(x + 2, kFloor, z + 4, "minecraft:chest", "minecraft:chest",
                     {{"facing", "north"}, {"type", "single"}});
    canvas.container(x + 4, kFloor, z + 4, "minecraft:chest", "minecraft:chest",
                     {{"facing", "north"}, {"type", "left"}});
    canvas.container(x + 5, kFloor, z + 4, "minecraft:chest", "minecraft:chest",
                     {{"facing", "north"}, {"type", "right"}});
    canvas.container(x + 7, kFloor, z + 4, "minecraft:trapped_chest", "minecraft:trapped_chest",
                     {{"facing", "north"}, {"type", "single"}});
    canvas.container(x + 9, kFloor, z + 4, "minecraft:ender_chest", "minecraft:ender_chest",
                     {{"facing", "north"}});
    canvas.container(x + 11, kFloor, z + 4, "minecraft:barrel", "minecraft:barrel",
                     {{"facing", "up"}, {"open", "false"}});
    canvas.container(x + 13, kFloor, z + 4, "minecraft:shulker_box", "minecraft:shulker_box",
                     {{"facing", "up"}});
    canvas.container(x + 2, kFloor, z + 8, "minecraft:hopper", "minecraft:hopper",
                     {{"enabled", "true"}, {"facing", "down"}});
    canvas.container(x + 4, kFloor, z + 8, "minecraft:jukebox", "minecraft:jukebox",
                     {{"has_record", "false"}});
    for (i32 index = 0; index < 8; ++index) {
        canvas.set(x + 6 + index, kFloor, z + 8, "minecraft:note_block",
                   {{"instrument", "harp"}, {"note", std::to_string(index * 3)},
                    {"powered", "false"}});
    }
}

/// A three-tier iron pyramid under a beacon: the shape is the input.
void stations_beacon(Canvas& canvas, i32 x, i32 z) {
    for (i32 tier = 0; tier < 3; ++tier) {
        const i32 radius = 3 - tier;
        canvas.fill(x + 8 - radius, kFloor + tier, z + 8 - radius, x + 8 + radius, kFloor + tier,
                    z + 8 + radius, "minecraft:iron_block");
    }
    canvas.container(x + 8, kFloor + 3, z + 8, "minecraft:beacon", "minecraft:beacon");
}

// ── Mobs ────────────────────────────────────────────────────────────────────

/// A sealed, unlit box: the only place where spawn rules can be observed
/// without the sky deciding the answer.
void mobs_dark_box(Canvas& canvas, i32 x, i32 z) {
    canvas.shell(x + 1, kFloor, z + 1, x + 15, kFloor + 4, z + 15, "minecraft:stone");
    canvas.fill(x + 2, kFloor + 1, z + 2, x + 14, kFloor + 3, z + 14, "minecraft:air");
    canvas.set(x + 1, kFloor + 1, z + 8, "minecraft:iron_door",
               {{"facing", "east"}, {"half", "lower"}, {"hinge", "left"}, {"open", "false"},
                {"powered", "false"}});
    canvas.set(x + 1, kFloor + 2, z + 8, "minecraft:iron_door",
               {{"facing", "east"}, {"half", "upper"}, {"hinge", "left"}, {"open", "false"},
                {"powered", "false"}});
}

/// A pen with a gate, a water pen, and open ground to summon onto.
void mobs_pens(Canvas& canvas, i32 x, i32 z) {
    canvas.fill(x + 1, kFloor + 1, z + 1, x + 7, kFloor + 1, z + 1, "minecraft:oak_fence");
    canvas.fill(x + 1, kFloor + 1, z + 7, x + 7, kFloor + 1, z + 7, "minecraft:oak_fence");
    canvas.fill(x + 1, kFloor + 1, z + 1, x + 1, kFloor + 1, z + 7, "minecraft:oak_fence");
    canvas.fill(x + 7, kFloor + 1, z + 1, x + 7, kFloor + 1, z + 7, "minecraft:oak_fence");
    canvas.set(x + 4, kFloor + 1, z + 1, "minecraft:oak_fence_gate",
               {{"facing", "north"}, {"in_wall", "false"}, {"open", "false"}, {"powered", "false"}});

    canvas.fill(x + 9, kFloor - 2, z + 9, x + 15, kFloor, z + 15, "minecraft:water",
                {{"level", "0"}});
    canvas.fill(x + 9, kFloor + 1, z + 1, x + 15, kFloor + 1, z + 7, "minecraft:smooth_stone");
}


// ── Galerie des blocs ───────────────────────────────────────────────────────

/// Blocks that must not go in the gallery.
///
/// Not because they are awkward to look at: because placing them changes the
/// world. A fluid flows out over the neighbours, a fire spreads, a portal is a
/// portal. The gallery is meant to be the same world every time it is opened.
[[nodiscard]] bool gallery_excludes(std::string_view name) {
    static constexpr std::array<std::string_view, 14> refused{
        "minecraft:air",          "minecraft:cave_air",      "minecraft:void_air",
        "minecraft:water",        "minecraft:lava",          "minecraft:fire",
        "minecraft:soul_fire",    "minecraft:nether_portal", "minecraft:end_portal",
        "minecraft:end_gateway",  "minecraft:moving_piston", "minecraft:piston_head",
        "minecraft:bubble_column", "minecraft:structure_void"};
    return std::ranges::find(refused, name) != refused.end();
}

/// Every block the registry knows, on its own pedestal, in registry order.
///
/// Registry order rather than alphabetical: it is the order the ids are in, so
/// a block that renders wrong can be found by its number as well as its name.
/// Spans several chunks — it is a gallery, not a plot.
void gallery_all_blocks(Canvas& canvas, i32 x, i32 z) {
    constexpr i32 kColumns = 32;
    constexpr i32 kSpacing = 2;

    const auto& blocks = canvas.blocks();
    i32         placed = 0;
    for (usize index = 0; index < blocks.block_count(); ++index) {
        const auto             block = registry::BlockId{static_cast<u16>(index)};
        const std::string_view name  = blocks.block_name(block);
        if (gallery_excludes(name)) {
            continue;
        }
        const i32 column = x + (placed % kColumns) * kSpacing;
        const i32 row    = z + (placed / kColumns) * kSpacing;
        canvas.set(column, kFloor, row, "minecraft:polished_andesite");
        canvas.set(column, kFloor + 1, row, blocks.default_state(block));
        ++placed;
    }
}

// ── La place centrale ───────────────────────────────────────────────────────

/// Where a player arrives. A signpost per zone, so the bench explains itself
/// to whoever walks into it six months from now.
void build_plaza(Canvas& canvas, std::span<const Plot> plots) {
    canvas.fill(-14, kFloor, -14, -2, kFloor, -2, "minecraft:smooth_stone");
    canvas.fill(-12, kFloor + 1, -12, -4, kFloor + 1, -4, "minecraft:air");

    const std::array<std::string, 4> title{std::string{"ONDES VOXEL"}, std::string{"banc de test"},
                                           std::string{"1.20.1"}, std::string{"regenere par ov-lab"}};
    // Not on the spawn block itself: a player arriving stands on top of
    // whatever is there, and standing on the signpost is a poor welcome.
    canvas.sign(-8, kFloor + 1, -11, title);

    // One post per zone, naming where that zone starts.
    std::vector<std::string_view> zones;
    for (const Plot& plot : plots) {
        if (std::ranges::find(zones, plot.zone) == zones.end()) {
            zones.push_back(plot.zone);
        }
    }
    for (usize index = 0; index < zones.size(); ++index) {
        const i32 post_x = -13 + static_cast<i32>(index) * 2;
        i32       first_x = 0;
        i32       first_z = 0;
        for (const Plot& plot : plots) {
            if (plot.zone == zones[index]) {
                first_x = plot.x;
                first_z = plot.z;
                break;
            }
        }
        canvas.set(post_x, kFloor + 1, -3, "minecraft:oak_fence");
        const std::array<std::string, 4> lines{
            std::string{zones[index]}, std::string{"x "} + std::to_string(first_x),
            std::string{"z "} + std::to_string(first_z), std::string{}};
        canvas.sign(post_x, kFloor + 1, -2, lines);
    }
}

}  // namespace

std::vector<Plot> catalogue() {
    // Column and row are plot coordinates; the pitch turns them into blocks.
    const auto at = [](i32 column, i32 row) { return std::pair{column * kPitch, row * kPitch}; };

    std::vector<Plot> plots;
    const auto add = [&](i32 column, i32 row, std::string_view zone, std::string_view name,
                         std::string_view purpose, void (*build)(Canvas&, i32, i32)) {
        const auto [x, z] = at(column, row);
        plots.push_back(Plot{x, z, zone, name, purpose, build});
    };

    add(0, 0, "redstone", "dust", "levels along sixteen blocks, from a block and from a lever",
        redstone_dust_line);
    add(1, 0, "redstone", "repeaters", "the four delays, in ticks", redstone_repeater_delays);
    add(2, 0, "redstone", "locked repeater", "the side-fed case the block stores", redstone_locked_repeater);
    add(3, 0, "redstone", "comparators", "compare, subtract, and reading a container",
        redstone_comparators);
    add(4, 0, "redstone", "torch burn-out", "eight changes in sixty ticks", redstone_torch_burnout);
    add(5, 0, "redstone", "piston limit", "twelve extends, thirteen must refuse",
        redstone_piston_limit);
    add(6, 0, "redstone", "quasi-connectivity", "the piston fired from one block above",
        redstone_quasi_connectivity);
    add(7, 0, "redstone", "transport", "observer, hopper chain, dispenser, dropper",
        redstone_transport);

    add(0, 1, "fluides", "basin", "the shape of one source's puddle", fluid_basin);
    add(1, 1, "fluides", "hole seeking", "the gap is off to one side, on purpose",
        fluid_hole_seeking);
    add(2, 1, "fluides", "mixing", "four geometries: stone, cobblestone, obsidian", fluid_mixing);
    add(3, 1, "fluides", "waterlogging", "eight holders, waterlogged and not",
        fluid_waterlogging);
    add(4, 1, "fluides", "infinite source", "two sources, one gap", fluid_infinite_source);
    add(5, 1, "fluides", "sponge", "radius seven, sixty-five blocks at most", fluid_sponge);

    add(0, 2, "physique", "fall heights", "fifteen pillars and a forty-block tower",
        physics_fall_heights);
    add(1, 2, "physique", "surfaces", "ice, slime, honey, soul sand, mud, cobweb",
        physics_surfaces);
    add(2, 2, "physique", "hazards", "cactus, berries, magma, campfire, lava, water",
        physics_hazards);

    add(0, 3, "lumiere", "dark room", "sealed: no sky light, only what is put inside",
        light_dark_room);
    add(1, 3, "lumiere", "gradient", "one torch down a roofed corridor", light_gradient);
    add(2, 3, "lumiere", "emitters", "fourteen sources side by side", light_emitters);
    add(3, 3, "lumiere", "shaft", "one hole in a roof: the case a heightmap gets wrong",
        light_shaft);

    add(0, 4, "formes", "stairs", "four facings, two halves, and the corner case", shapes_stairs);
    add(1, 4, "formes", "walls", "low, tall, and the pole", shapes_walls);
    add(2, 4, "formes", "connections", "fences, gates, panes, bars, and the slab that refuses",
        shapes_connections);
    add(3, 4, "formes", "doors", "both halves, both hinges, and trapdoors", shapes_doors);

    add(0, 5, "postes", "workbenches", "twelve stations, plus enchanting and brewing",
        stations_workbenches);
    add(1, 5, "postes", "containers", "single, double, trapped, ender, barrel, shulker, hopper",
        stations_containers);
    add(2, 5, "postes", "beacon", "a three-tier pyramid: the shape is the input",
        stations_beacon);

    add(0, 6, "mobs", "dark box", "sealed and unlit: spawn rules without the sky", mobs_dark_box);
    add(1, 6, "mobs", "pens", "fenced, watered, and open ground to summon onto", mobs_pens);

    add(0, 7, "galerie", "every block", "the whole registry, in id order, on pedestals",
        gallery_all_blocks);

    return plots;
}

void build_world(Canvas& canvas) {
    const std::vector<Plot> plots = catalogue();

    // The ground first, over everything the plots and the plaza will touch.
    i32 max_chunk_x = 0;
    i32 max_chunk_z = 0;
    for (const Plot& plot : plots) {
        max_chunk_x = std::max(max_chunk_x, (plot.x + 64) >> 4);
        max_chunk_z = std::max(max_chunk_z, (plot.z + 64) >> 4);
    }
    canvas.lay_ground(-2, -2, max_chunk_x, max_chunk_z);

    build_plaza(canvas, plots);

    for (const Plot& plot : plots) {
        plot_sign(canvas, plot.x, plot.z, plot.zone, plot.name);
        plot.build(canvas, plot.x, plot.z);
    }
}

}  // namespace ov::lab
