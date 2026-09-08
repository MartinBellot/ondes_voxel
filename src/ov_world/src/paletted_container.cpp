#include "ov/world/paletted_container.hpp"

#include <algorithm>

namespace ov::world {
namespace {

/// Blocks per section: 16 x 16 x 16.
constexpr usize kBlockCapacity = 4096;

/// Biome cells per section: 4 x 4 x 4, one per 4-block cube.
constexpr usize kBiomeCapacity = 64;

[[nodiscard]] constexpr u64 mask_for(u8 bits) noexcept {
    return bits >= 64 ? ~u64{0} : ((u64{1} << bits) - 1);
}

/// Read one entry out of packed longs.
///
/// The division and modulo are by entries-per-long rather than by a bit count,
/// which is the whole of the "an entry never spans two longs" rule.
[[nodiscard]] u16 unpack(std::span<const u64> data, usize index, u8 bits) noexcept {
    const u32   per_long = entries_per_long(bits);
    const usize word     = index / per_long;
    if (word >= data.size()) {
        return 0;
    }
    const u32 shift = static_cast<u32>(index % per_long) * bits;
    return static_cast<u16>((data[word] >> shift) & mask_for(bits));
}

void pack(std::span<u64> data, usize index, u8 bits, u16 value) noexcept {
    const u32   per_long = entries_per_long(bits);
    const usize word     = index / per_long;
    if (word >= data.size()) {
        return;
    }
    const u32 shift = static_cast<u32>(index % per_long) * bits;
    const u64 mask  = mask_for(bits) << shift;
    data[word]      = (data[word] & ~mask) | ((static_cast<u64>(value) << shift) & mask);
}

}  // namespace

PalettedContainer::PalettedContainer(usize capacity, u16 value, u8 direct_bits,
                                     u8 min_indirect_bits, u8 max_indirect_bits)
    : capacity_{capacity},
      direct_bits_{direct_bits},
      min_indirect_bits_{min_indirect_bits},
      max_indirect_bits_{max_indirect_bits},
      bits_{0},
      kind_{PaletteKind::SingleValue},
      palette_{value} {}

PalettedContainer PalettedContainer::blocks(u16 fill) {
    return PalettedContainer{kBlockCapacity, fill, kDirectBlockBits, kMinIndirectBits,
                             kMaxIndirectBits};
}

PalettedContainer PalettedContainer::biomes(u16 fill) {
    return PalettedContainer{kBiomeCapacity, fill, kDirectBiomeBits, kMinBiomeIndirectBits,
                             kMaxBiomeIndirectBits};
}

u8 PalettedContainer::bits_for(usize palette_size) const noexcept {
    const u8 bits = raw_bits_for_palette(palette_size);
    return bits < min_indirect_bits_ ? min_indirect_bits_ : bits;
}

usize PalettedContainer::palette_size() const noexcept {
    return kind_ == PaletteKind::Direct ? 0 : palette_.size();
}

u16 PalettedContainer::get(usize index) const noexcept {
    if (index >= capacity_) {
        return 0;
    }
    switch (kind_) {
        case PaletteKind::SingleValue: return palette_.empty() ? 0 : palette_[0];
        case PaletteKind::Indirect: {
            const u16 slot = unpack(data_, index, bits_);
            return slot < palette_.size() ? palette_[slot] : 0;
        }
        case PaletteKind::Direct: return unpack(data_, index, bits_);
    }
    return 0;
}

void PalettedContainer::repack(u8 new_bits, std::span<const u16> new_palette) {
    // Read every entry through the old representation before touching anything,
    // then write it back through the new one.
    std::vector<u16> values(capacity_);
    for (usize i = 0; i < capacity_; ++i) {
        values[i] = get(i);
    }

    palette_.assign(new_palette.begin(), new_palette.end());
    bits_ = new_bits;
    kind_ = new_palette.empty() ? PaletteKind::Direct : PaletteKind::Indirect;

    data_.assign(packed_length(capacity_, bits_), 0);

    for (usize i = 0; i < capacity_; ++i) {
        u16 encoded = values[i];
        if (kind_ == PaletteKind::Indirect) {
            const auto it = std::ranges::find(palette_, values[i]);
            encoded       = static_cast<u16>(std::distance(palette_.begin(), it));
        }
        pack(data_, i, bits_, encoded);
    }
}

void PalettedContainer::set(usize index, u16 value) {
    if (index >= capacity_) {
        return;
    }

    if (kind_ == PaletteKind::SingleValue) {
        if (!palette_.empty() && palette_[0] == value) {
            return;  // nothing changes
        }
        // A second distinct value: the section stops being uniform and needs
        // real storage for the first time.
        std::vector<u16> palette{palette_.empty() ? u16{0} : palette_[0], value};
        repack(bits_for(palette.size()), palette);
        pack(data_, index, bits_, 1);
        return;
    }

    if (kind_ == PaletteKind::Indirect) {
        const auto it = std::ranges::find(palette_, value);
        if (it != palette_.end()) {
            pack(data_, index, bits_, static_cast<u16>(std::distance(palette_.begin(), it)));
            return;
        }

        std::vector<u16> palette = palette_;
        palette.push_back(value);

        const u8 needed = bits_for(palette.size());
        if (needed > max_indirect_bits_) {
            // The palette has outgrown its usefulness: past 256 entries the
            // indices cost as much as the ids themselves.
            repack(direct_bits_, {});
            pack(data_, index, bits_, value);
            return;
        }

        if (needed > bits_) {
            repack(needed, palette);
        } else {
            palette_ = std::move(palette);
        }
        pack(data_, index, bits_, static_cast<u16>(palette_.size() - 1));
        return;
    }

    pack(data_, index, bits_, value);
}

void PalettedContainer::assign(std::span<const u16> values) {
    if (values.size() != capacity_) {
        return;
    }

    // Choose the tightest representation for what is actually there, rather
    // than growing into it one entry at a time.
    std::vector<u16> distinct;
    distinct.reserve(16);
    bool overflowed = false;

    for (const u16 value : values) {
        if (std::ranges::find(distinct, value) != distinct.end()) {
            continue;
        }
        distinct.push_back(value);
        if (bits_for(distinct.size()) > max_indirect_bits_) {
            overflowed = true;
            break;
        }
    }

    if (!overflowed && distinct.size() == 1) {
        kind_ = PaletteKind::SingleValue;
        bits_ = 0;
        palette_.assign(1, distinct[0]);
        data_.clear();
        return;
    }

    if (overflowed) {
        kind_ = PaletteKind::Direct;
        bits_ = direct_bits_;
        palette_.clear();
    } else {
        kind_    = PaletteKind::Indirect;
        bits_    = bits_for(distinct.size());
        palette_ = std::move(distinct);
    }

    data_.assign(packed_length(capacity_, bits_), 0);
    for (usize i = 0; i < capacity_; ++i) {
        u16 encoded = values[i];
        if (kind_ == PaletteKind::Indirect) {
            const auto it = std::ranges::find(palette_, values[i]);
            encoded       = static_cast<u16>(std::distance(palette_.begin(), it));
        }
        pack(data_, i, bits_, encoded);
    }
}

bool PalettedContainer::load_packed(u8 bits, std::span<const u16> palette,
                                    std::span<const u64> data) {
    // A section from disk or from the network declares its own shape, and a
    // chunk that lies about it must be refused rather than read past.
    if (bits == 0) {
        if (palette.size() != 1 || !data.empty()) {
            return false;
        }
        kind_ = PaletteKind::SingleValue;
        bits_ = 0;
        palette_.assign(palette.begin(), palette.end());
        data_.clear();
        return true;
    }

    if (bits > 64) {
        return false;
    }
    if (data.size() != packed_length(capacity_, bits)) {
        return false;
    }

    if (!palette.empty()) {
        // Indirect: every index the data can express must exist in the palette,
        // or a lookup would run off the end.
        if (bits > max_indirect_bits_ || palette.size() > (usize{1} << bits)) {
            return false;
        }
        for (usize i = 0; i < capacity_; ++i) {
            if (unpack(data, i, bits) >= palette.size()) {
                return false;
            }
        }
        kind_ = PaletteKind::Indirect;
    } else {
        kind_ = PaletteKind::Direct;
    }

    bits_ = bits;
    palette_.assign(palette.begin(), palette.end());
    data_.assign(data.begin(), data.end());
    return true;
}

}  // namespace ov::world
