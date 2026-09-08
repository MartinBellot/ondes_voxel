// Writing Anvil region files.
//
// The counterpart to RegionFile, and the half where mistakes cost a world
// rather than a read. A region holds up to 1024 chunks in 4 KiB sectors, with
// an 8 KiB header of offsets and timestamps at the front; a chunk whose offset
// is wrong is not a corrupt chunk, it is a chunk that reads as whatever
// happened to be at that sector.
//
// Two rules the format does not state and that a writer has to get right:
//
//   * The length field counts the compression byte. A chunk of N compressed
//     bytes is written as N+1, and being one short truncates the last byte of
//     every chunk in the file.
//   * A chunk always starts on a sector boundary, so its payload is padded.
//     The padding is not optional: the offset table addresses sectors, not
//     bytes, and there is no way to express "starts partway through".
//
// The write is whole-file: an existing region is read in, the changed chunks
// are replaced, and the result is written out atomically. Rewriting a chunk in
// place would be faster and would mean a crash mid-write leaves a region whose
// header and contents disagree.
#pragma once

#include "ov/math/block_pos.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"

#include <filesystem>
#include <map>
#include <vector>

namespace ov::nbt {

/// Builds a region file from chunk documents.
class RegionWriter {
public:
    RegionWriter() = default;

    /// Start from an existing region so chunks nobody touched survive.
    ///
    /// A missing file is not an error: the first save of a new region starts
    /// from nothing, and treating that as a failure would make a fresh world
    /// unsavable.
    [[nodiscard]] static RegionWriter open_or_empty(const std::filesystem::path& path);

    /// Replace one chunk. Coordinates are local to the region, 0..31.
    void set_chunk(u32 local_x, u32 local_z, const Document& document, u32 timestamp);

    /// Whether the region holds anything worth writing.
    [[nodiscard]] bool empty() const noexcept { return chunks_.empty(); }

    [[nodiscard]] usize chunk_count() const noexcept { return chunks_.size(); }

    /// The complete file, header included.
    [[nodiscard]] std::vector<u8> build() const;

    /// Write to disk through a temporary file and a rename.
    ///
    /// A region half-written over its predecessor is worse than no save at all:
    /// the header would point at sectors the new contents have moved.
    [[nodiscard]] bool write(const std::filesystem::path& path) const;

private:
    struct StoredChunk {
        std::vector<u8> compressed;
        u32             timestamp{0};
    };

    /// Keyed by the region slot, so iteration is in file order and the output
    /// is byte-stable for the same input.
    std::map<usize, StoredChunk> chunks_;
};

}  // namespace ov::nbt
