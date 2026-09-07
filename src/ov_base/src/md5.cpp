#include "ov/base/md5.hpp"

#include <cstring>

namespace ov {
namespace {

// Per-round shift amounts, from RFC 1321.
constexpr u32 kShifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

// floor(abs(sin(i + 1)) * 2^32), also from RFC 1321.
constexpr u32 kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

[[nodiscard]] constexpr u32 rotate_left(u32 value, u32 count) noexcept {
    return (value << count) | (value >> (32 - count));
}

void process_block(const u8* block, u32 state[4]) noexcept {
    // MD5 reads its message words little-endian, unlike everything else in this
    // project.
    u32 words[16];
    for (u32 i = 0; i < 16; ++i) {
        words[i] = static_cast<u32>(block[i * 4]) | (static_cast<u32>(block[i * 4 + 1]) << 8) |
                   (static_cast<u32>(block[i * 4 + 2]) << 16) |
                   (static_cast<u32>(block[i * 4 + 3]) << 24);
    }

    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];

    for (u32 i = 0; i < 64; ++i) {
        u32 f = 0;
        u32 g = 0;

        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }

        const u32 temp = d;
        d              = c;
        c              = b;
        b              = b + rotate_left(a + f + kSine[i] + words[g], kShifts[i]);
        a              = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

}  // namespace

Md5Digest md5(std::span<const u8> data) noexcept {
    u32 state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

    const usize length = data.size();

    usize offset = 0;
    for (; offset + 64 <= length; offset += 64) {
        process_block(data.data() + offset, state);
    }

    // Padding: a 0x80 byte, then zeros, then the bit length as a little-endian
    // 64-bit value. Two blocks are needed when the remainder leaves no room for
    // the length.
    u8          tail[128]{};
    const usize remaining = length - offset;
    std::memcpy(tail, data.data() + offset, remaining);
    tail[remaining] = 0x80;

    const usize tail_size = remaining < 56 ? 64 : 128;
    const u64   bits      = static_cast<u64>(length) * 8;
    for (u32 i = 0; i < 8; ++i) {
        tail[tail_size - 8 + i] = static_cast<u8>((bits >> (8 * i)) & 0xFF);
    }

    process_block(tail, state);
    if (tail_size == 128) {
        process_block(tail + 64, state);
    }

    Md5Digest digest{};
    for (u32 i = 0; i < 4; ++i) {
        digest[i * 4]     = static_cast<u8>(state[i] & 0xFF);
        digest[i * 4 + 1] = static_cast<u8>((state[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = static_cast<u8>((state[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = static_cast<u8>((state[i] >> 24) & 0xFF);
    }
    return digest;
}

Md5Digest md5(std::string_view text) noexcept {
    return md5(std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()});
}

}  // namespace ov
