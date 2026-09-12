// ── streaming ── The carved terrain every worldgen stack of one world shares.
//
// The server generates the world in squares of 4x4 chunks from a cold pipeline
// (async_chunk_source.hpp, invariant 3), and decorating those sixteen needs
// their neighbours carved two rings out: an 8x8 of terrain for 16 chunks. Four
// terrain generations per chunk delivered, where three of the four are the
// neighbouring squares' own chunks — generated again, identically, when those
// squares come up.
//
// Identically is the point. Terrain up to the carvers is a pure function of the
// seed and the position (`worldgen::TerrainCache`), so keeping it changes how
// often it is computed and never what it is. `ov_gendet` proves that: the
// digest of the generated world is the same with the cache and without it
// (docs/provenance/chargement-terrain.md).
//
// Shared between threads, so behind a mutex — which guards this cache and
// nothing else. No chunk in here is part of the world: they are intermediate
// terrain no player can see, never written after they are stored, and handed
// out as copies. The copy is taken **under the lock** for a reason that is easy
// to miss: copying a `world::Chunk` copies its sections, and copying a section
// marks the *source* as shared (chunk_section.hpp, copy-on-write). Two workers
// copying one cached chunk at once would both write that mark. Under the lock,
// they do not.
#pragma once

#include "ov/base/types.hpp"
#include "ov/worldgen/pipeline.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server {

class SharedTerrainCache final : public worldgen::TerrainCache {
public:
    /// At most `capacity` chunks, least recently used forgotten first.
    explicit SharedTerrainCache(usize capacity);
    ~SharedTerrainCache() override;

    SharedTerrainCache(const SharedTerrainCache&)            = delete;
    SharedTerrainCache& operator=(const SharedTerrainCache&) = delete;

    [[nodiscard]] bool fetch(i32 chunk_x, i32 chunk_z, world::Chunk& chunk,
                             std::vector<std::string_view>& starts) override;
    void offer(i32 chunk_x, i32 chunk_z, const world::Chunk& chunk,
               const std::vector<std::string_view>& starts) override;

    [[nodiscard]] usize capacity() const noexcept;

    /// One line for the log: hits, misses, what it holds.
    [[nodiscard]] std::string report() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::server
