#define OV_LOG_CATEGORY "worldgen"

// ── worldgen-3 ── See biome_zoom.hpp. SHA-256 is FIPS 180-4, written from the
// standard; the zoom is the documented eight-corner nearest-offset rule.

#include "ov/worldgen/biome_zoom.hpp"

#include <limits>
#include <vector>

namespace ov::worldgen {

namespace {

constexpr std::array<u32, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

[[nodiscard]] constexpr u32 rotr(u32 x, u32 n) noexcept { return (x >> n) | (x << (32 - n)); }

/// `LinearCongruentialGenerator.next`.
[[nodiscard]] constexpr i64 lcg(i64 a, i64 b) noexcept {
    const auto ua = static_cast<u64>(a);
    return static_cast<i64>(ua * (ua * 6364136223846793005ULL + 1442695040888963407ULL) +
                            static_cast<u64>(b));
}

/// The offset of a cell's corner, in [-0.45, 0.45).
[[nodiscard]] f64 fiddle(i64 value) noexcept {
    const i64 shifted = value >> 24;
    const i64 mod     = ((shifted % 1024) + 1024) % 1024;  // floorMod
    const f64 unit    = static_cast<f64>(mod) / 1024.0;
    return (unit - 0.5) * 0.9;
}

[[nodiscard]] f64 fiddled_distance(i64 seed, i32 x, i32 y, i32 z, f64 fx, f64 fy, f64 fz) noexcept {
    i64 mix = lcg(seed, x);
    mix     = lcg(mix, y);
    mix     = lcg(mix, z);
    mix     = lcg(mix, x);
    mix     = lcg(mix, y);
    mix     = lcg(mix, z);
    const f64 dx = fiddle(mix);
    mix          = lcg(mix, seed);
    const f64 dy = fiddle(mix);
    mix          = lcg(mix, seed);
    const f64 dz = fiddle(mix);
    return (fz + dz) * (fz + dz) + (fy + dy) * (fy + dy) + (fx + dx) * (fx + dx);
}

}  // namespace

std::array<u8, 32> sha256(std::span<const u8> message) noexcept {
    std::array<u32, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<u8> data(message.begin(), message.end());
    const u64       bits = static_cast<u64>(message.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) {
        data.push_back(0);
    }
    for (i32 shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<u8>(bits >> shift));
    }
    std::array<u32, 64> w{};
    for (usize block = 0; block < data.size(); block += 64) {
        for (usize i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32>(data[block + i * 4]) << 24) |
                   (static_cast<u32>(data[block + i * 4 + 1]) << 16) |
                   (static_cast<u32>(data[block + i * 4 + 2]) << 8) |
                   static_cast<u32>(data[block + i * 4 + 3]);
        }
        for (usize i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i]         = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (usize i = 0; i < 64; ++i) {
            const u32 s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch    = (e & f) ^ (~e & g);
            const u32 temp1 = k + s1 + ch + kRound[i] + w[i];
            const u32 s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj   = (a & b) ^ (a & c) ^ (b & c);
            const u32 temp2 = s0 + maj;
            k               = g;
            g               = f;
            f               = e;
            e               = d + temp1;
            d               = c;
            c               = b;
            b               = a;
            a               = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += k;
    }
    std::array<u8, 32> digest{};
    for (usize i = 0; i < 8; ++i) {
        digest[i * 4]     = static_cast<u8>(h[i] >> 24);
        digest[i * 4 + 1] = static_cast<u8>(h[i] >> 16);
        digest[i * 4 + 2] = static_cast<u8>(h[i] >> 8);
        digest[i * 4 + 3] = static_cast<u8>(h[i]);
    }
    return digest;
}

i64 obfuscate_biome_seed(i64 world_seed) noexcept {
    std::array<u8, 8> bytes{};
    const auto        value = static_cast<u64>(world_seed);
    for (usize i = 0; i < 8; ++i) {
        bytes[i] = static_cast<u8>(value >> (8 * i));  // little-endian in
    }
    const auto digest = sha256(bytes);
    u64        out    = 0;
    for (usize i = 0; i < 8; ++i) {
        out |= static_cast<u64>(digest[i]) << (8 * i);  // little-endian out
    }
    return static_cast<i64>(out);
}

BiomeCell fuzzy_biome_cell(i64 zoom_seed, i32 x, i32 y, i32 z) noexcept {
    const i32 bx = x - 2;
    const i32 by = y - 2;
    const i32 bz = z - 2;
    const i32 qx = bx >> 2;
    const i32 qy = by >> 2;
    const i32 qz = bz >> 2;
    const f64 fx = static_cast<f64>(bx & 3) / 4.0;
    const f64 fy = static_cast<f64>(by & 3) / 4.0;
    const f64 fz = static_cast<f64>(bz & 3) / 4.0;
    i32       best          = 0;
    f64       best_distance = std::numeric_limits<f64>::infinity();
    for (i32 corner = 0; corner < 8; ++corner) {
        const bool low_x = (corner & 4) == 0;
        const bool low_y = (corner & 2) == 0;
        const bool low_z = (corner & 1) == 0;
        const f64  distance =
            fiddled_distance(zoom_seed, low_x ? qx : qx + 1, low_y ? qy : qy + 1,
                             low_z ? qz : qz + 1, low_x ? fx : fx - 1.0, low_y ? fy : fy - 1.0,
                             low_z ? fz : fz - 1.0);
        if (best_distance > distance) {
            best          = corner;
            best_distance = distance;
        }
    }
    return {(best & 4) == 0 ? qx : qx + 1, (best & 2) == 0 ? qy : qy + 1,
            (best & 1) == 0 ? qz : qz + 1};
}

}  // namespace ov::worldgen
