// What the carvers cut, recorded separately from the blocks they cut it out of.
//
// A carver does not write blocks directly. It sets a bit per cell in a mask
// that belongs to the chunk, and the block write is a second, separate
// decision: the same bit means air in stone, lava below the lava level, and
// nothing at all in a block the carver is not allowed to replace.
//
// Keeping the two apart is not tidiness. The mask is a real part of a saved
// chunk — `CarvingMasks: {AIR: [L;…]}` in the region file, kept for as long as
// the chunk has not reached `minecraft:full` — so a chunk we write has to carry
// the same bits in the same order, and a chunk the game wrote is an exact
// oracle for ours. That oracle is the only way to tell "our caves are the right
// size" from "our caves are in the right place", and the two are not the same
// result.
#pragma once

#include "ov/base/types.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// Which pass of carving a mask belongs to.
///
/// The game keeps one mask per `GenerationStep.Carving` value. 1.20.1 runs
/// only `AIR` in the overworld — `LIQUID` still exists in the format and is
/// written empty — so the enum exists to name what is saved, not to offer a
/// choice.
enum class CarvingStep : u8 {
    Air    = 0,
    Liquid = 1,
};

[[nodiscard]] std::string_view to_string(CarvingStep step) noexcept;

/// One chunk's worth of carved cells.
///
/// Indexed by chunk-local x and z (0..15) and by absolute y. The bit order is
/// the game's and is load-bearing, because it is what reaches the disk:
///
///     index = (x & 15) | ((z & 15) << 4) | ((y - min_y) << 8)
///
/// and the bits are packed into 64-bit words low bit first, which is what
/// Java's `BitSet.toLongArray` produces. Trailing all-zero words are dropped on
/// the way out, again as `BitSet` does; a mask that carved nothing serialises
/// to an empty array rather than to 1536 zeroes.
class CarvingMask {
public:
    /// `min_y` and `height` are the dimension's, not a section's: the mask
    /// spans the whole column, because a canyon reaching from y = 10 down to
    /// bedrock is one mask entry, not several.
    CarvingMask(i32 min_y, i32 height);

    [[nodiscard]] i32 min_y() const noexcept { return min_y_; }

    [[nodiscard]] i32 height() const noexcept { return height_; }

    /// Out-of-range y is silently outside the mask rather than an error: the
    /// carvers clamp their own loops, and a stray query from a caller that has
    /// not is a question about a cell that cannot be carved, whose answer is
    /// no.
    [[nodiscard]] bool get(i32 local_x, i32 y, i32 local_z) const noexcept;

    void set(i32 local_x, i32 y, i32 local_z) noexcept;

    /// How many cells are marked. The single number a parity run compares
    /// first, before it looks at which ones.
    [[nodiscard]] usize count() const noexcept;

    [[nodiscard]] bool empty() const noexcept { return count() == 0; }

    void clear() noexcept;

    /// The words as the region file stores them: little-endian bit order
    /// inside each word, trailing zero words dropped.
    [[nodiscard]] std::vector<i64> to_long_array() const;

    /// The inverse, for reading a chunk the game wrote.
    [[nodiscard]] static CarvingMask from_long_array(std::span<const i64> words, i32 min_y,
                                                     i32 height);

    /// Raw word access, for the bulk comparisons a parity harness does.
    [[nodiscard]] std::span<const u64> words() const noexcept { return words_; }

private:
    i32              min_y_{-64};
    i32              height_{384};
    std::vector<u64> words_;
};

}  // namespace ov::worldgen
