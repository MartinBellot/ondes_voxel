// Loading a square of real chunks off the disk.
//
// This replaces the hand-built demo scene, and that is the point of it: every
// number the renderer produces from here on is measured against a world the
// game itself wrote, not against a scene chosen to make the renderer look good.
//
// It reads region files directly rather than going through a server, because
// ov_netclient does not exist yet. When it does, this becomes the fallback for
// looking at a save without running anything.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/render/chunk_mesher.hpp"
#include "ov/world/chunk.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ov::demo {

struct LoadedWorld {
    /// Chunks by position. A map rather than a grid, because a region file has
    /// holes: a chunk that was never generated is simply absent.
    std::map<std::pair<i32, i32>, std::unique_ptr<world::Chunk>> chunks;
    usize                                                        chunks_read{0};
    usize                                                        chunks_failed{0};
    /// Present on disk but stopped before `full`, so they carry no blocks.
    usize chunks_unfinished{0};
    /// True if any chunk carried stored light. A save without it renders black,
    /// and that is worth telling apart from a broken mesher.
    bool light_stored{false};

    [[nodiscard]] const world::Chunk* at(i32 x, i32 z) const;

    /// The nine chunks around one, for ChunkSectionView.
    [[nodiscard]] render::ChunkNeighbours neighbours(i32 x, i32 z) const;

    /// A spot to stand: the highest non-air block near the middle of what was
    /// loaded, plus a couple of blocks of headroom.
    [[nodiscard]] Vec3f suggested_camera(const registry::BlockRegistry& blocks) const;
};

/// Read every chunk within `radius` of (centre_x, centre_z), in chunk
/// coordinates, from the region files under `world_directory`.
[[nodiscard]] std::optional<LoadedWorld> load_world(const std::filesystem::path&   world_directory,
                                                    const registry::BlockRegistry& blocks,
                                                    i32 centre_x, i32 centre_z, i32 radius);

}  // namespace ov::demo
