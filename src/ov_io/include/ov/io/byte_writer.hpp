// Big-endian writer, the counterpart of ByteReader.
//
// Unlike reading, writing cannot fail on malformed input — the caller owns what
// it emits — so nothing here returns an error. It grows a vector and swaps
// bytes on the way out.
#pragma once

#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "ov/base/types.hpp"

namespace ov::io {

class ByteWriter {
public:
    ByteWriter() = default;

    /// Reserve up front when the size is known. Chunk serialization runs on the
    /// tick thread's budget; growing a vector eight times is avoidable work.
    explicit ByteWriter(usize reserve) { buffer_.reserve(reserve); }

    void write_u8(u8 value) { buffer_.push_back(value); }
    void write_i8(i8 value) { buffer_.push_back(static_cast<u8>(value)); }

    void write_u16(u16 value) { write_big_endian(value); }
    void write_i16(i16 value) { write_big_endian(value); }
    void write_u32(u32 value) { write_big_endian(value); }
    void write_i32(i32 value) { write_big_endian(value); }
    void write_u64(u64 value) { write_big_endian(value); }
    void write_i64(i64 value) { write_big_endian(value); }

    void write_f32(f32 value) { write_big_endian(std::bit_cast<u32>(value)); }
    void write_f64(f64 value) { write_big_endian(std::bit_cast<u64>(value)); }

    void write_bytes(std::span<const u8> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    void write_bytes(std::string_view text) {
        buffer_.insert(buffer_.end(), text.begin(), text.end());
    }

    [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool  empty() const noexcept { return buffer_.empty(); }

    [[nodiscard]] std::span<const u8> data() const noexcept { return buffer_; }
    [[nodiscard]] const std::vector<u8>& buffer() const noexcept { return buffer_; }

    /// Hand the buffer over, leaving this writer empty.
    [[nodiscard]] std::vector<u8> take() noexcept { return std::move(buffer_); }

    void clear() noexcept { buffer_.clear(); }
    void reserve(usize capacity) { buffer_.reserve(capacity); }

    /// Overwrite bytes already written. Needed by length-prefixed framing, where
    /// the length is only known after the body has been encoded.
    void patch_u32(usize offset, u32 value) {
        if constexpr (std::endian::native == std::endian::little) {
            value = byteswap_u32(value);
        }
        std::memcpy(buffer_.data() + offset, &value, sizeof(value));
    }

private:
    // std::byteswap is C++23 and available on every supported toolchain, but
    // this stays explicit because the region-file and packet paths depend on
    // it being exactly this and nothing else.
    [[nodiscard]] static constexpr u32 byteswap_u32(u32 v) noexcept {
        return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
               ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
    }

    template <typename T>
    void write_big_endian(T value) {
        if constexpr (std::endian::native == std::endian::little && sizeof(T) > 1) {
            value = std::byteswap(value);
        }
        const auto offset = buffer_.size();
        buffer_.resize(offset + sizeof(T));
        std::memcpy(buffer_.data() + offset, &value, sizeof(T));
    }

    std::vector<u8> buffer_;
};

}  // namespace ov::io
