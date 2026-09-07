// Bounds-checked big-endian reader over a borrowed byte range.
//
// Every format Ondes VOXEL parses is big-endian: NBT, the region file header,
// and the wire protocol. Every supported host is little-endian, so every read
// swaps. That is not a detail to be remembered at each call site.
//
// The other thing this exists for is that all three formats arrive from
// untrusted sources — a region file on disk, a packet from the network. A
// reader that trusts its input is a remote crash. Nothing here can read past
// the end: every operation returns std::expected and the error is a value, not
// an exception, because the decode path is hot and exceptions do not belong in
// it.
#pragma once

#include "ov/base/types.hpp"

#include <bit>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>

namespace ov::io {

enum class ReadError {
    /// Fewer bytes remain than the operation needs.
    OutOfBounds,
    /// A length prefix exceeds what remains, or a sanity limit.
    InvalidLength,
    /// Byte sequence is not valid for the declared encoding.
    MalformedEncoding,
};

[[nodiscard]] std::string_view to_string(ReadError error) noexcept;

template<typename T>
using ReadResult = std::expected<T, ReadError>;

class ByteReader {
public:
    explicit ByteReader(std::span<const u8> data) noexcept : data_{data} {}

    [[nodiscard]] usize position() const noexcept { return position_; }

    [[nodiscard]] usize size() const noexcept { return data_.size(); }

    [[nodiscard]] usize remaining() const noexcept { return data_.size() - position_; }

    [[nodiscard]] bool exhausted() const noexcept { return position_ >= data_.size(); }

    void seek(usize position) noexcept {
        position_ = position < data_.size() ? position : data_.size();
    }

    [[nodiscard]] ReadResult<void> skip(usize count) noexcept {
        if (count > remaining()) {
            return std::unexpected{ReadError::OutOfBounds};
        }
        position_ += count;
        return {};
    }

    [[nodiscard]] ReadResult<u8> read_u8() noexcept {
        if (remaining() < 1) {
            return std::unexpected{ReadError::OutOfBounds};
        }
        return data_[position_++];
    }

    [[nodiscard]] ReadResult<i8> read_i8() noexcept {
        return read_u8().transform([](u8 v) { return static_cast<i8>(v); });
    }

    [[nodiscard]] ReadResult<u16> read_u16() noexcept { return read_big_endian<u16>(); }

    [[nodiscard]] ReadResult<i16> read_i16() noexcept { return read_big_endian<i16>(); }

    [[nodiscard]] ReadResult<u32> read_u32() noexcept { return read_big_endian<u32>(); }

    [[nodiscard]] ReadResult<i32> read_i32() noexcept { return read_big_endian<i32>(); }

    [[nodiscard]] ReadResult<u64> read_u64() noexcept { return read_big_endian<u64>(); }

    [[nodiscard]] ReadResult<i64> read_i64() noexcept { return read_big_endian<i64>(); }

    /// IEEE-754 in big-endian byte order, which is what NBT and the protocol use.
    [[nodiscard]] ReadResult<f32> read_f32() noexcept {
        return read_big_endian<u32>().transform([](u32 bits) { return std::bit_cast<f32>(bits); });
    }

    [[nodiscard]] ReadResult<f64> read_f64() noexcept {
        return read_big_endian<u64>().transform([](u64 bits) { return std::bit_cast<f64>(bits); });
    }

    // ── Little-endian ───────────────────────────────────────────────────────
    // ZIP is the one format here that is little-endian, and jars, resource
    // packs and datapacks are all ZIP. Kept separate and explicitly named so
    // that nothing reads a ZIP field with the big-endian accessors by reflex.

    [[nodiscard]] ReadResult<u16> read_u16_le() noexcept { return read_little_endian<u16>(); }

    [[nodiscard]] ReadResult<u32> read_u32_le() noexcept { return read_little_endian<u32>(); }

    [[nodiscard]] ReadResult<u64> read_u64_le() noexcept { return read_little_endian<u64>(); }

    /// A view into the underlying buffer. Valid only while that buffer lives;
    /// no copy is made, which is what keeps chunk parsing cheap.
    [[nodiscard]] ReadResult<std::span<const u8>> read_bytes(usize count) noexcept {
        if (count > remaining()) {
            return std::unexpected{ReadError::OutOfBounds};
        }
        const auto view = data_.subspan(position_, count);
        position_ += count;
        return view;
    }

    /// Everything not yet consumed, without advancing.
    [[nodiscard]] std::span<const u8> peek_remaining() const noexcept {
        return data_.subspan(position_);
    }

private:
    template<typename T>
    [[nodiscard]] ReadResult<T> read_big_endian() noexcept {
        if (remaining() < sizeof(T)) {
            return std::unexpected{ReadError::OutOfBounds};
        }
        T value{};
        std::memcpy(&value, data_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        if constexpr (std::endian::native == std::endian::little && sizeof(T) > 1) {
            value = std::byteswap(value);
        }
        return value;
    }

    template<typename T>
    [[nodiscard]] ReadResult<T> read_little_endian() noexcept {
        if (remaining() < sizeof(T)) {
            return std::unexpected{ReadError::OutOfBounds};
        }
        T value{};
        std::memcpy(&value, data_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
            value = std::byteswap(value);
        }
        return value;
    }

    std::span<const u8> data_;
    usize               position_{0};
};

}  // namespace ov::io
