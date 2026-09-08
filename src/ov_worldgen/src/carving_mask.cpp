#include "ov/worldgen/carving_mask.hpp"

#include "ov/base/assert.hpp"

#include <algorithm>
#include <bit>

namespace ov::worldgen {

namespace {

/// Cells per horizontal slice. 16 x 16, and the shift that indexes them.
constexpr usize kSliceShift = 8;
constexpr usize kSliceSize  = 1U << kSliceShift;

}  // namespace

std::string_view to_string(CarvingStep step) noexcept {
    switch (step) {
        case CarvingStep::Air: return "AIR";
        case CarvingStep::Liquid: return "LIQUID";
    }
    return "AIR";
}

CarvingMask::CarvingMask(i32 min_y, i32 height) : min_y_(min_y), height_(height) {
    OV_ASSERT(height > 0);
    const usize cells = kSliceSize * static_cast<usize>(height);
    words_.assign((cells + 63) / 64, 0);
}

bool CarvingMask::get(i32 local_x, i32 y, i32 local_z) const noexcept {
    const i32 relative = y - min_y_;
    if (relative < 0 || relative >= height_) {
        return false;
    }
    const usize index = (static_cast<usize>(local_x) & 15U) |
                        ((static_cast<usize>(local_z) & 15U) << 4U) |
                        (static_cast<usize>(relative) << kSliceShift);
    return (words_[index >> 6U] & (1ULL << (index & 63U))) != 0;
}

void CarvingMask::set(i32 local_x, i32 y, i32 local_z) noexcept {
    const i32 relative = y - min_y_;
    if (relative < 0 || relative >= height_) {
        return;
    }
    const usize index = (static_cast<usize>(local_x) & 15U) |
                        ((static_cast<usize>(local_z) & 15U) << 4U) |
                        (static_cast<usize>(relative) << kSliceShift);
    words_[index >> 6U] |= 1ULL << (index & 63U);
}

usize CarvingMask::count() const noexcept {
    usize total = 0;
    for (const u64 word : words_) {
        total += static_cast<usize>(std::popcount(word));
    }
    return total;
}

void CarvingMask::clear() noexcept {
    std::ranges::fill(words_, 0ULL);
}

std::vector<i64> CarvingMask::to_long_array() const {
    // Java's BitSet drops trailing zero words, and a saved chunk therefore has
    // an array whose length says how deep the deepest carved cell is. Writing
    // the full 1536 words would still read back correctly but would not be the
    // same bytes, and the point of this type is that it is.
    usize last = words_.size();
    while (last > 0 && words_[last - 1] == 0) {
        --last;
    }
    std::vector<i64> out;
    out.reserve(last);
    for (usize i = 0; i < last; ++i) {
        out.push_back(static_cast<i64>(words_[i]));
    }
    return out;
}

CarvingMask CarvingMask::from_long_array(std::span<const i64> words, i32 min_y, i32 height) {
    CarvingMask mask{min_y, height};
    const usize copied = std::min(words.size(), mask.words_.size());
    for (usize i = 0; i < copied; ++i) {
        mask.words_[i] = static_cast<u64>(words[i]);
    }
    return mask;
}

}  // namespace ov::worldgen
