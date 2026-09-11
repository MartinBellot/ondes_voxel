#define OV_LOG_CATEGORY "server"

#include "ov/server/generation_check.hpp"

#include "async_chunk_source.hpp"
#include "generated_world.hpp"
#include "ov/base/log.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/nbt/region_writer.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_storage.hpp"

#include <fmt/format.h>

#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ov::server {

namespace {

/// Compare two chunks and count the cells that differ.
///
/// Every block of every section and every biome cell — not a hash, and not a
/// spot check. A hash would say "different" and nothing else; the point of a
/// count is that a design which is nearly deterministic looks different from
/// one that is not deterministic at all.
void compare(const world::Chunk& a, const world::Chunk& b, GenerationCheck& out) {
    const auto shape = a.shape();
    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                ++out.block_cells;
                if (a.get_block(x, y, z) != b.get_block(x, y, z)) {
                    ++out.block_cells_differing;
                }
            }
        }
    }
    for (i32 y = shape.min_y; y <= shape.max_y(); y += 4) {
        for (usize z = 0; z < 16; z += 4) {
            for (usize x = 0; x < 16; x += 4) {
                ++out.biome_cells;
                if (a.get_biome(x, y, z) != b.get_biome(x, y, z)) {
                    ++out.biome_cells_differing;
                }
            }
        }
    }
}

[[nodiscard]] f64 seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

GenerationCheck check_generation_determinism(const std::filesystem::path& data_root, i64 seed,
                                             i32 origin_block_x, i32 origin_block_z, i32 side,
                                             usize workers) {
    GenerationCheck result;

    const auto pack = data_root / "vanilla" / "1.20.1" / "registry.ovpack";
    auto       blocks = registry::BlockRegistry::load(pack);
    if (!blocks) {
        OV_LOG_ERROR("determinism check: cannot read {}", pack.string());
        return result;
    }
    auto registries = registry::Registries::load(pack);
    if (!registries) {
        OV_LOG_ERROR("determinism check: cannot read the registries from {}", pack.string());
        return result;
    }

    // The block registry's own biome names as the codec's. That makes the
    // block-index-to-codec-id table the identity, which is what this check
    // wants: the translation is a pure function applied identically to both
    // arms, so mapping it through the real codec would add a step that cannot
    // fail differently and a file this check would then need.
    std::vector<std::string_view> biome_names;
    biome_names.reserve(blocks->biome_count());
    for (u32 index = 0; index < blocks->biome_count(); ++index) {
        biome_names.push_back(blocks->biome_name(index));
    }

    auto world = GeneratedWorld::load(data_root, *blocks, *registries, biome_names, seed,
                                      workers + 1);
    if (!world) {
        OV_LOG_ERROR("determinism check: the generator could not be built");
        return result;
    }
    result.loaded = true;

    // ── Serial arm ──────────────────────────────────────────────────────────
    std::unordered_map<u64, world::Chunk> serial;
    const auto                            serial_start = std::chrono::steady_clock::now();
    for (i32 dz = 0; dz < side; ++dz) {
        for (i32 dx = 0; dx < side; ++dx) {
            std::vector<std::pair<ChunkPos, world::Chunk>> produced;
            world->generate_square(0, (origin_block_x + dx) * AsyncChunkSource::kBlockChunks,
                                   (origin_block_z + dz) * AsyncChunkSource::kBlockChunks,
                                   AsyncChunkSource::kBlockChunks, produced);
            for (auto& [pos, chunk] : produced) {
                serial.emplace(pos.packed(), std::move(chunk));
            }
            ++result.blocks;
        }
    }
    result.serial_seconds = seconds_since(serial_start);

    // ── Parallel arm ────────────────────────────────────────────────────────
    //
    // The real path: the same `AsyncChunkSource` the server runs, with a real
    // pool behind it. Requests are re-offered because the source refuses when
    // its queue is full, which is exactly how the tick loop uses it.
    std::unordered_map<u64, world::Chunk> parallel;
    const auto                            parallel_start = std::chrono::steady_clock::now();
    {
        AsyncChunkSource source{*world, workers};

        std::vector<ChunkPos> pending;
        for (i32 dz = 0; dz < side; ++dz) {
            for (i32 dx = 0; dx < side; ++dx) {
                pending.push_back(ChunkPos{(origin_block_x + dx) * AsyncChunkSource::kBlockChunks,
                                           (origin_block_z + dz) * AsyncChunkSource::kBlockChunks});
            }
        }

        std::vector<bool>           received(pending.size(), false);
        std::vector<GeneratedBlock> finished;
        usize                       done = 0;
        while (done < pending.size()) {
            // Only what has not come back yet. The source forgets a block as
            // soon as it is drained, so re-offering a finished one would set it
            // generating again — which is not wrong, only twice the work, and
            // it would make the parallel timing a lie.
            for (usize i = 0; i < pending.size(); ++i) {
                if (!received[i]) {
                    (void)source.request(pending[i]);
                }
            }
            finished.clear();
            if (source.drain(finished) == 0) {
                std::this_thread::yield();
                continue;
            }
            for (GeneratedBlock& block : finished) {
                for (usize i = 0; i < pending.size(); ++i) {
                    if (AsyncChunkSource::block_of(pending[i].x) == block.block_x &&
                        AsyncChunkSource::block_of(pending[i].z) == block.block_z) {
                        received[i] = true;
                    }
                }
                for (auto& [pos, chunk] : block.chunks) {
                    parallel.emplace(pos.packed(), std::move(chunk));
                }
                ++done;
            }
        }
    }
    result.parallel_seconds = seconds_since(parallel_start);

    // ── Compare ─────────────────────────────────────────────────────────────
    for (const auto& [key, chunk] : serial) {
        const auto found = parallel.find(key);
        if (found == parallel.end()) {
            ++result.missing;
            continue;
        }
        ++result.chunks;
        compare(chunk, found->second, result);
    }
    for (const auto& [key, chunk] : parallel) {
        if (!serial.contains(key)) {
            ++result.missing;
        }
    }

    return result;
}

// ── structures ──
GenerationExport export_generated_chunks(const std::filesystem::path& data_root, i64 seed,
                                         std::string_view settings, i32 min_chunk_x,
                                         i32 min_chunk_z, i32 max_chunk_x, i32 max_chunk_z,
                                         const std::filesystem::path& region_dir) {
    GenerationExport result;
    const auto       pack   = data_root / "vanilla" / "1.20.1" / "registry.ovpack";
    auto             blocks = registry::BlockRegistry::load(pack);
    auto             registries = registry::Registries::load(pack);
    if (!blocks || !registries) {
        OV_LOG_ERROR("export: cannot read {}", pack.string());
        return result;
    }
    // The block registry's biome names as the codec's, as in the check above:
    // the disk form is by name either way.
    std::vector<std::string_view> biome_names;
    for (u32 index = 0; index < blocks->biome_count(); ++index) {
        biome_names.push_back(blocks->biome_name(index));
    }
    auto world =
        GeneratedWorld::load(data_root, *blocks, *registries, biome_names, seed, 1, settings);
    if (!world) {
        OV_LOG_ERROR("export: the generator could not be built");
        return result;
    }
    result.loaded = true;

    world::ChunkCodecContext codec;
    codec.blocks      = &*blocks;
    codec.biome_names = biome_names;
    codec.registries  = &*registries;
    codec.air         = world::AirStates::from(*blocks);

    std::error_code ignored;
    std::filesystem::create_directories(region_dir, ignored);
    const auto start = std::chrono::steady_clock::now();
    std::map<std::pair<i32, i32>, std::vector<std::pair<ChunkPos, nbt::Document>>> by_region;
    constexpr i32 kSide = AsyncChunkSource::kBlockChunks;
    for (i32 bz = AsyncChunkSource::block_of(min_chunk_z);
         bz <= AsyncChunkSource::block_of(max_chunk_z); ++bz) {
        for (i32 bx = AsyncChunkSource::block_of(min_chunk_x);
             bx <= AsyncChunkSource::block_of(max_chunk_x); ++bx) {
            std::vector<std::pair<ChunkPos, world::Chunk>> produced;
            world->generate_square(0, bx * kSide, bz * kSide, kSide, produced);
            ++result.squares;
            for (auto& [pos, chunk] : produced) {
                if (pos.x < min_chunk_x || pos.x > max_chunk_x || pos.z < min_chunk_z ||
                    pos.z > max_chunk_z) {
                    continue;
                }
                by_region[{pos.x >> 5, pos.z >> 5}].emplace_back(pos, world::to_nbt(chunk, codec));
                ++result.chunks;
            }
        }
    }
    for (auto& [region, documents] : by_region) {
        const auto path =
            region_dir / fmt::format("r.{}.{}.mca", region.first, region.second);
        auto writer = nbt::RegionWriter::open_or_empty(path);
        for (auto& [pos, document] : documents) {
            writer.set_chunk(static_cast<u32>(pos.x & 31), static_cast<u32>(pos.z & 31),
                             std::move(document), 0);
        }
        if (!writer.write(path)) {
            OV_LOG_ERROR("export: could not write {}", path.string());
        }
    }
    result.seconds = seconds_since(start);
    return result;
}

}  // namespace ov::server
