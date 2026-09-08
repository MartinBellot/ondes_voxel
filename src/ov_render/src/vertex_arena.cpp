#include "ov/render/vertex_arena.hpp"

#include "ov/base/assert.hpp"

#include <algorithm>

namespace ov::render {

VertexArena::VertexArena(u64 capacity_bytes, u64 page_bytes) : page_(page_bytes) {
    OV_ASSERT(page_bytes > 0);
    // Round the capacity down: a partial trailing page can never be handed out
    // whole, and pretending otherwise makes `used == capacity` unreachable.
    capacity_ = capacity_bytes - capacity_bytes % page_;
    if (capacity_ > 0) {
        free_.push_back(Block{0, capacity_});
    }
}

u64 VertexArena::round_up(u64 bytes) const noexcept {
    const u64 remainder = bytes % page_;
    return remainder == 0 ? bytes : bytes + (page_ - remainder);
}

u64 VertexArena::allocate(u64 bytes) {
    if (bytes == 0) {
        return kNoSpace;
    }
    const u64 wanted = round_up(bytes);

    for (usize i = 0; i < free_.size(); ++i) {
        Block& block = free_[i];
        if (block.size < wanted) {
            continue;
        }
        const u64 offset = block.offset;
        if (block.size == wanted) {
            free_.erase(free_.begin() + static_cast<isize>(i));
        } else {
            block.offset += wanted;
            block.size -= wanted;
        }
        used_ += wanted;
        return offset;
    }
    return kNoSpace;
}

void VertexArena::release(u64 offset, u64 bytes) {
    if (bytes == 0) {
        return;
    }
    const u64 size = round_up(bytes);
    OV_ASSERT(offset % page_ == 0);
    OV_ASSERT(offset + size <= capacity_);
    OV_ASSERT(used_ >= size);
    used_ -= size;

    // Insert in address order, then merge with whichever neighbour touches.
    // Doing it in this order means the two merges are the only cases there are:
    // a free list that is always sorted and never overlapping cannot produce a
    // third.
    const auto at = std::ranges::lower_bound(
        free_, offset, {}, [](const Block& block) { return block.offset; });
    const auto inserted = free_.insert(at, Block{offset, size});
    const auto index    = static_cast<usize>(inserted - free_.begin());

    if (index + 1 < free_.size() &&
        free_[index].offset + free_[index].size == free_[index + 1].offset) {
        free_[index].size += free_[index + 1].size;
        free_.erase(free_.begin() + static_cast<isize>(index) + 1);
    }
    if (index > 0 && free_[index - 1].offset + free_[index - 1].size == free_[index].offset) {
        free_[index - 1].size += free_[index].size;
        free_.erase(free_.begin() + static_cast<isize>(index));
    }
}

u64 VertexArena::largest_free() const noexcept {
    u64 largest = 0;
    for (const Block& block : free_) {
        largest = std::max(largest, block.size);
    }
    return largest;
}

}  // namespace ov::render
