// One device-local buffer for the whole terrain, and a free list over it.
//
// The renderer used to give every section of every layer its own buffer. At a
// radius of 12 that is 9674 of them, and it costs on three fronts at once: the
// driver keeps an allocation and a residency entry for each, a draw has to
// rebind before it can issue, and nothing can ever be merged. Measured on an
// M2 at 2560x1440 the recording alone — bind, push, draw, 2491 times — was
// 10.49 ms at the 99th percentile, against a budget of 20 for the whole frame.
//
// So the vertices all live in one buffer and a section owns a range of it.
// That is what makes a single vkCmdDrawIndexedIndirect per layer possible,
// because every command in it addresses the same vertex buffer and differs
// only by its vertexOffset.
//
// There is no Vulkan in this file on purpose. An allocator is exactly the kind
// of thing that is wrong in ways a screenshot will not show — a range handed
// out twice, a coalesce that loses a page — and those are unit tests, not
// frames.
#pragma once

#include "ov/base/types.hpp"

#include <vector>

namespace ov::render {

/// Allocation granularity, in bytes.
///
/// The plan said 4 KiB pages. It cannot be 4096, and the reason is worth
/// writing down: an indirect command's vertexOffset counts *vertices*, not
/// bytes, so every allocation has to start on a whole number of the 12-byte
/// terrain vertex — and 4096 is not a multiple of 12. 3072 is: 256 vertices,
/// 64 quads, and still a round fraction of a page of memory.
inline constexpr u64 kArenaPageBytes = 3072;

/// A first-fit free list with coalescing, over a flat byte range.
///
/// First fit rather than best fit, deliberately. Best fit leaves a trail of
/// slivers too small for anything; first fit over a list kept in address order
/// keeps the large blocks at the end where the next big section finds them.
/// The allocation pattern here is not adversarial — a few thousand ranges of a
/// few kilobytes each, freed in bursts when chunks unload — and the whole
/// structure is rebuilt from scratch cheaply if it ever does fragment.
class VertexArena {
public:
    /// Returned by allocate when the request does not fit.
    static constexpr u64 kNoSpace = ~0ULL;

    explicit VertexArena(u64 capacity_bytes, u64 page_bytes = kArenaPageBytes);

    /// A byte offset into the buffer, always a multiple of the page size, or
    /// kNoSpace. Zero is a valid offset, which is why the failure value is not.
    [[nodiscard]] u64 allocate(u64 bytes);

    /// Give a range back. The size must be the one that was asked for, not the
    /// rounded one: the arena rounds again the same way.
    void release(u64 offset, u64 bytes);

    [[nodiscard]] u64 capacity() const noexcept { return capacity_; }
    /// Bytes handed out, after rounding up to whole pages.
    [[nodiscard]] u64 used() const noexcept { return used_; }
    [[nodiscard]] u64 page_bytes() const noexcept { return page_; }
    /// The largest single allocation that would still succeed. A useful thing
    /// to log when one fails: it separates "full" from "fragmented".
    [[nodiscard]] u64 largest_free() const noexcept;
    /// How many separate free ranges there are. One means no fragmentation.
    [[nodiscard]] usize free_block_count() const noexcept { return free_.size(); }

    [[nodiscard]] u64 round_up(u64 bytes) const noexcept;

private:
    struct Block {
        u64 offset{0};
        u64 size{0};
    };

    u64                capacity_{0};
    u64                page_{kArenaPageBytes};
    u64                used_{0};
    /// Kept sorted by offset and never overlapping, which is what makes
    /// coalescing a look at the two neighbours instead of a scan.
    std::vector<Block> free_;
};

}  // namespace ov::render
