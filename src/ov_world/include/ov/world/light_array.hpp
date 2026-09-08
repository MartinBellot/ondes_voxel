// A section's worth of light levels: 4096 values of 4 bits, or none at all.
//
// Light is the largest thing a loaded world stores — larger than the blocks.
// Two full arrays per section is 4 KiB, against roughly 2.5 KiB for a typical
// paletted block container, and the naive implementation allocates both for
// every section whether or not they hold anything.
//
// They usually hold nothing. Measured on a real world: of 24 sections in one
// chunk, 7 carried a block-light array and 2 a sky-light array. Above the
// terrain the sky is uniformly full and the block light uniformly zero; below
// the stone both are uniformly zero. So an array that is entirely one value
// stores no bytes and remembers the value instead, and only materialises when
// something actually varies. That is what halves the footprint of a loaded
// world, and it is why this is a type rather than a `std::array` member.
//
// The packing is vanilla's: 2048 bytes for 4096 cells, the **even** index in
// the low nibble. Getting that backwards produces a world lit in a fine
// checkerboard, which reads as a shader bug rather than a storage one.
#pragma once

#include "ov/base/types.hpp"

#include <span>
#include <vector>

namespace ov::world {

/// Cells in a light array: one per block in a section.
inline constexpr usize kLightCellCount = 4096;

/// Bytes on disk and on the wire: two cells per byte.
inline constexpr usize kLightByteCount = kLightCellCount / 2;

/// The brightest a light level goes.
inline constexpr u8 kMaxLightLevel = 15;

class LightArray {
public:
    /// An array where every cell holds `value`, storing nothing.
    explicit LightArray(u8 value = 0) noexcept : uniform_{static_cast<u8>(value & 0xF)} {}

    /// True while no storage has been allocated.
    [[nodiscard]] bool is_uniform() const noexcept { return data_.empty(); }

    /// The value every cell holds while uniform. Meaningless otherwise.
    [[nodiscard]] u8 uniform_value() const noexcept { return uniform_; }

    [[nodiscard]] u8 get(usize index) const noexcept;

    /// Set one cell, allocating the 2048 bytes on the first write that
    /// actually differs from the uniform value.
    void set(usize index, u8 value);

    /// The packed bytes, exactly as they appear on disk and on the wire.
    /// Empty while uniform — which is how vanilla writes them: the tag is
    /// simply absent.
    [[nodiscard]] std::span<const u8> data() const noexcept { return data_; }

    /// Adopt 2048 packed bytes from a chunk file or a packet.
    ///
    /// Returns false on any other length. Both sources are untrusted, and a
    /// short array would otherwise be read past on the first lookup.
    [[nodiscard]] bool load(std::span<const u8> bytes);

    /// The 2048 packed bytes, materialising them if the array is uniform.
    ///
    /// The wire has no way to say "uniformly 15": a section is either sent with
    /// its bytes or listed in the *empty* mask, and empty means all zero. So a
    /// uniformly-lit section still has to be written out, and only a uniformly
    /// dark one can be elided. Getting that backwards renders the world black.
    [[nodiscard]] std::vector<u8> to_bytes() const;

    /// True when the array can be left out of the packet entirely: uniform and
    /// dark.
    [[nodiscard]] bool is_absent() const noexcept { return data_.empty() && uniform_ == 0; }

    /// Drop the storage if every cell turned out to hold the same value.
    ///
    /// Worth calling after bulk lighting: a section that has been fully lit and
    /// then buried is uniform again, and staying materialised would keep 2 KiB
    /// per section for nothing.
    void compact();

private:
    void materialise();

    u8              uniform_{0};
    std::vector<u8> data_;
};

}  // namespace ov::world
