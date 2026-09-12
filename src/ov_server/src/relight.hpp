// The server's light engine: sky light and block light recomputed from blocks.
//
// Moved out of server.cpp unchanged in what it computes, and changed in what it
// costs. Before, one block broken by a player relit a 3x3 neighbourhood on the
// network thread, and that pass was the single largest thing the server did —
// measured in docs/provenance/performance-tick.md. Two things made it slow and
// neither was the flood fill itself:
//
//   * every directly lit cell of nine chunks was pushed as a flood-fill seed —
//     a few hundred thousand cells — although a cell of 15 surrounded by cells
//     of 15 spreads nothing. Only lit cells beside an unlit column matter;
//   * every light read and write looked its chunk up by walking a list of
//     nine, through a lambda, half a million times a pass.
//
// Block light was also rescanned cell by cell in every one of the nine chunks,
// although a section whose palette holds no light-emitting state cannot seed
// anything. The palette answers that in a handful of lookups.
//
// The results are bit-identical to the old passes: the flood fill converges to
// the same fixed point from any seed set that contains every seed able to
// raise a cell, and tests/test_relight.cpp checks it against the previous code
// cell by cell.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_map.hpp"
#include "ov/world/light_engine.hpp"

#include <functional>

namespace ov::server {

// ── light ── The incremental engine (ov/world/light_engine.hpp) is what the
// Overworld uses now: a chunk is lit alone and stitched to its neighbours when
// it arrives, and an edit repairs the light around itself. The passes below
// stay for the Nether and the End, for the superflat generator, and as the
// "before" of the benches in tests/test_relight.cpp.

/// The server's chunk map as the light engine sees it: resident chunks only.
class MapLightSource final : public world::LightChunkSource {
public:
    explicit MapLightSource(world::ChunkMap& map) : map_{map} {}

    world::Chunk* light_chunk(i32 chunk_x, i32 chunk_z) override {
        return map_.find(ChunkPos{chunk_x, chunk_z});
    }

private:
    world::ChunkMap& map_;
};

/// The Overworld's light rules. See docs/provenance/incremental-light.md for
/// why light-filtering blocks are set the way they are.
inline constexpr world::LightRules kOverworldLight{true, false};
// ── end light ──

/// Finds a loaded chunk by chunk coordinates, or nullptr. Never loads one.
using ChunkLookup = std::function<world::Chunk*(i32, i32)>;

// ── light ── Any lookup as the light engine's source: the Nether's and the
// End's maps live inside `NetherWorld`. The engine caches what it finds, so the
// function is called once per chunk per call, not once per cell.
class LookupLightSource final : public world::LightChunkSource {
public:
    explicit LookupLightSource(ChunkLookup lookup) : lookup_{std::move(lookup)} {}

    world::Chunk* light_chunk(i32 chunk_x, i32 chunk_z) override {
        return lookup_(chunk_x, chunk_z);
    }

private:
    ChunkLookup lookup_;
};

/// The Nether's and the End's rules: no sky, block light only.
inline constexpr world::LightRules kNoSkyLight{false, false};
// ── end light ──

/// Does sky light stop at this block?
///
/// Measured, not assumed. "Anything that is not air" was the previous rule and
/// it made signs, torches and fences cast full shadows — visible only after a
/// reload, since a client lights its own placements locally.
///
/// Attenuating blocks (water, leaves, ice) are treated as transparent for now:
/// passing light through at full strength is wrong by a level or two, whereas
/// stopping it is wrong by fifteen.
[[nodiscard]] bool stops_sky_light(const registry::BlockRegistry* blocks,
                                   registry::BlockStateId         state);

/// The lowest y a column still sees full sunlight.
///
/// Not WORLD_SURFACE: that counts the highest **non-air** block, and a sign or
/// a torch raises it while letting light straight through. Direct sunlight
/// descends without losing a level until it meets something opaque, so this
/// scans for that instead.
[[nodiscard]] i32 sky_floor(const registry::BlockRegistry* blocks, const world::Chunk& chunk,
                            usize x, usize z, i32 top);

/// Fill in the light the blocks themselves give off, in one chunk.
///
/// Separate from the sky pass and stored in its own nibble array, because the
/// two answer different questions: sky light is what reaches a cell from above
/// and block light is what a torch puts there. The emission is per state — a
/// candle by how many are in the cluster, an ore by whether it is lit — and it
/// is measured, not guessed. Propagation stops at the chunk's edge.
void relight_blocks(world::Chunk& chunk, const registry::BlockRegistry& blocks);

/// Sky light for a whole chunk: direct sunlight, then flood fill.
///
/// Direct sunlight first: every column is lit to 15 from the top down to its
/// first obstruction. Then the light spreads sideways and downward, losing one
/// level per step. Propagation stops at the chunk's edge; the neighbourhood
/// pass below is what crosses it.
void relight_chunk(world::Chunk& chunk, const registry::BlockRegistry* blocks);

/// Sky light across a chunk and its eight neighbours.
///
/// The single-chunk version stops at the border, so a build sitting against
/// one casts no shadow into the chunk beside it. Only chunks already loaded
/// take part — pulling neighbours in would cascade — so the edge of the loaded
/// area still seams, out of sight.
void relight_neighbourhood(const ChunkLookup& lookup, i32 centre_x, i32 centre_z,
                           const registry::BlockRegistry* blocks);

/// What a block edit owes the light: sky light across the 3x3 around the
/// edited chunk, then block light in each of those nine chunks.
///
/// The nine block passes are kept although only the edited chunk's blocks
/// changed. Generated chunks arrive with no block light at all, and relighting
/// the neighbours of an edit is today the only thing that gives them any;
/// dropping it would be a behaviour change dressed as an optimisation.
void relight_after_edit(const ChunkLookup& lookup, i32 centre_x, i32 centre_z,
                        const registry::BlockRegistry* blocks);

}  // namespace ov::server
