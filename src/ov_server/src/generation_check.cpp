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

#include <algorithm>
#include <array>
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

/// ── streaming ── Fold one chunk into an FNV-1a digest.
///
/// Here the hash is the point, where `compare` above refuses one: the two
/// worlds being compared live in two different binaries — the generator
/// before an optimisation and after it — and cannot be put side by side in
/// one process. The count says *how* different two worlds are; this says
/// *whether*, across builds, and a single differing cell changes it.
void digest_chunk(u64 key, const world::Chunk& chunk, u64& hash) {
    const auto mix = [&hash](u64 value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFU;
            hash *= 0x100000001B3ULL;
        }
    };
    mix(key);
    const auto shape = chunk.shape();
    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                mix(static_cast<u64>(chunk.get_block(x, y, z).value()));
            }
        }
    }
    for (i32 y = shape.min_y; y <= shape.max_y(); y += 4) {
        for (usize z = 0; z < 16; z += 4) {
            for (usize x = 0; x < 16; x += 4) {
                mix(static_cast<u64>(chunk.get_biome(x, y, z)));
            }
        }
    }
    constexpr std::array kStored{world::HeightmapType::WorldSurface,
                                 world::HeightmapType::MotionBlocking,
                                 world::HeightmapType::MotionBlockingNoLeaves,
                                 world::HeightmapType::OceanFloor};
    for (const world::HeightmapType type : kStored) {
        const world::Heightmap& map = chunk.heightmap(type);
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                mix(static_cast<u64>(static_cast<i64>(map.first_free(x, z))));
            }
        }
    }
    for (const world::BlockEntity& entity : chunk.block_entities()) {
        mix(entity.x);
        mix(static_cast<u64>(static_cast<i64>(entity.y)));
        mix(entity.z);
        for (const char c : entity.type) {
            mix(static_cast<u64>(static_cast<unsigned char>(c)));
        }
    }
}

[[nodiscard]] f64 seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

GenerationCheck check_generation_determinism(const std::filesystem::path& data_root, i64 seed,
                                             i32 origin_block_x, i32 origin_block_z, i32 side,
                                             usize workers, bool parallel_arm) {
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
    if (const std::string line = world->terrain_cache_report(); !line.empty()) {
        OV_LOG_INFO("serial arm: {}", line);  // ── streaming ──
    }

    // ── streaming ── The digest, in key order: an unordered_map's iteration
    // order is not part of what is being hashed.
    {
        std::vector<u64> keys;
        keys.reserve(serial.size());
        for (const auto& [key, chunk] : serial) {
            keys.push_back(key);
        }
        std::ranges::sort(keys);
        u64 hash = 0xCBF29CE484222325ULL;
        for (const u64 key : keys) {
            digest_chunk(key, serial.at(key), hash);
        }
        result.digest = hash;
    }
    if (!parallel_arm) {
        result.chunks = serial.size();
        return result;
    }
    // ── streaming ── A cold terrain cache for the parallel arm: on the one the
    // serial arm warmed, it would copy every chunk of terrain and its timing
    // would measure copies (the first run of this measured 0.9 s for 144
    // chunks, which is what that looks like).
    world->clear_terrain_cache();

    // ── Parallel arm ────────────────────────────────────────────────────────
    //
    // The real path: the same `AsyncChunkSource` the server runs, with a real
    // pool behind it. Requests are re-offered because the source refuses when
    // its queue is full, which is exactly how the tick loop uses it.
    std::unordered_map<u64, world::Chunk> parallel;
    const auto                            parallel_start = std::chrono::steady_clock::now();
    {
        // ── streaming ── the server's scheduling class, and its switch
        ThreadRole role = ThreadRole::Generation;
        if (const char* qos = std::getenv("OV_WORKER_QOS");
            qos != nullptr && std::string_view{qos} == "utility") {
            role = ThreadRole::Worker;
        }
        AsyncChunkSource source{*world, workers, role};

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
