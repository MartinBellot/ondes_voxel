#include "ov/world/light_array.hpp"

#include <algorithm>

namespace ov::world {
namespace {

/// The even cell lives in the low nibble. This is the whole of the format, and
/// reversing it lights the world in a fine checkerboard that looks like a
/// shader problem rather than a storage one.
[[nodiscard]] constexpr u32 shift_for(usize index) noexcept {
    return (index & 1) * 4;
}

}  // namespace

u8 LightArray::get(usize index) const noexcept {
    if (index >= kLightCellCount) {
        return 0;
    }
    if (data_.empty()) {
        return uniform_;
    }
    return static_cast<u8>((data_[index >> 1] >> shift_for(index)) & 0xF);
}

void LightArray::materialise() {
    // Every cell already holds the uniform value, so fill with both nibbles set
    // to it rather than zero — otherwise materialising a uniformly-lit section
    // would plunge it into darkness.
    const u8 both = static_cast<u8>((uniform_ << 4) | uniform_);
    data_.assign(kLightByteCount, both);
}

void LightArray::set(usize index, u8 value) {
    if (index >= kLightCellCount) {
        return;
    }
    value &= 0xF;

    if (data_.empty()) {
        if (value == uniform_) {
            return;  // still uniform: nothing to store
        }
        materialise();
    }

    const u32 shift   = shift_for(index);
    const u8  mask    = static_cast<u8>(0xF << shift);
    data_[index >> 1] = static_cast<u8>((data_[index >> 1] & ~mask) | ((value << shift) & mask));
}

bool LightArray::load(std::span<const u8> bytes) {
    if (bytes.empty()) {
        // An absent array is legitimate and common: it means uniformly dark.
        data_.clear();
        uniform_ = 0;
        return true;
    }
    if (bytes.size() != kLightByteCount) {
        return false;
    }
    data_.assign(bytes.begin(), bytes.end());
    return true;
}

std::vector<u8> LightArray::to_bytes() const {
    if (!data_.empty()) {
        return data_;
    }
    const u8 both = static_cast<u8>((uniform_ << 4) | uniform_);
    return std::vector<u8>(kLightByteCount, both);
}

void LightArray::compact() {
    if (data_.empty()) {
        return;
    }

    // Uniform means both nibbles of every byte agree, so one byte comparison
    // covers two cells.
    const u8 first = data_.front();
    if ((first >> 4) != (first & 0xF)) {
        return;
    }
    if (!std::ranges::all_of(data_, [first](u8 byte) { return byte == first; })) {
        return;
    }

    uniform_ = static_cast<u8>(first & 0xF);
    data_.clear();
    data_.shrink_to_fit();
}

}  // namespace ov::world
