// Proving that moving worldgen onto threads did not move the world.
//
// A parallel generator that produces a slightly different world on every run is
// not a faster generator, it is a broken one, and it is the failure mode this
// whole design is arranged around. The arrangement is stated in
// async_chunk_source.hpp; this is the measurement that says it holds.
//
// The check generates the same square of world twice — once entirely on the
// calling thread, once through the job pool with N workers — and compares the
// two results **cell by cell**, every block and every biome cell of every
// chunk. Anything other than zero differing cells is a failure, and the count
// is reported rather than reduced to a yes/no so that a partial failure says
// how partial it is.
//
// It lives in ov_server rather than in a tool because the thing being checked
// is `GeneratedWorld` and `AsyncChunkSource`, which are private to this module.
// A tool that reimplemented them would prove something about the reimplementation.
#pragma once

#include "ov/base/types.hpp"

#include <filesystem>
#include <string_view>

namespace ov::server {

/// What the comparison found.
struct GenerationCheck {
    /// False when the worldgen data could not be loaded at all. Every other
    /// field is meaningless then, which is why this is not a zero-difference
    /// result.
    bool loaded{false};

    usize blocks{0};
    usize chunks{0};

    /// Chunks the parallel arm produced that the serial arm did not, or vice
    /// versa. Non-zero means the two arms did not even generate the same set.
    usize missing{0};

    u64 block_cells{0};
    u64 block_cells_differing{0};
    u64 biome_cells{0};
    u64 biome_cells_differing{0};

    f64 serial_seconds{0.0};
    f64 parallel_seconds{0.0};

    [[nodiscard]] bool identical() const noexcept {
        return loaded && missing == 0 && block_cells_differing == 0 && biome_cells_differing == 0;
    }
};

/// Generate a `side` x `side` square of generation blocks twice and compare.
///
/// `origin_block_x/z` are in generation-block units, not chunks — see
/// `AsyncChunkSource::kBlockChunks`. `workers` is how many threads the parallel
/// arm uses; one is a legitimate value and still exercises the queue, and zero
/// makes the pool run inline, which is the degenerate case worth being able to
/// ask for.
[[nodiscard]] GenerationCheck check_generation_determinism(
    const std::filesystem::path& data_root, i64 seed, i32 origin_block_x, i32 origin_block_z,
    i32 side, usize workers);

// ── structures ──
/// What an export wrote.
struct GenerationExport {
    bool  loaded{false};
    usize squares{0};
    usize chunks{0};
    f64   seconds{0.0};
};

/// Generate every chunk of `[min, max]` (chunk coordinates, inclusive) through
/// the server's own `GeneratedWorld` — the squares the job pool asks for, with
/// the structure stage attached — and write them with `world::to_nbt` into
/// region files under `region_dir`. The disk form a player's `ov_dedicated`
/// would write, without a player: what a parity harness compares against the
/// game's regions. `settings` is "overworld", "nether" or "end".
[[nodiscard]] GenerationExport export_generated_chunks(const std::filesystem::path& data_root,
                                                       i64 seed, std::string_view settings,
                                                       i32 min_chunk_x, i32 min_chunk_z,
                                                       i32 max_chunk_x, i32 max_chunk_z,
                                                       const std::filesystem::path& region_dir);

}  // namespace ov::server
