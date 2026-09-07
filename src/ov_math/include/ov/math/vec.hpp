// Small fixed-size vectors.
//
// Deliberately hand-written rather than pulled from glm: these types appear in
// public headers everywhere, and a heavy template library in a public header is
// what turns a one-line edit into a four-minute rebuild (risk R5).
#pragma once

#include "ov/base/types.hpp"

#include <cmath>
#include <optional>
#include <string_view>

namespace ov {

template<typename T>
struct Vec3 {
    T x{};
    T y{};
    T z{};

    constexpr Vec3() noexcept = default;

    constexpr Vec3(T x_, T y_, T z_) noexcept : x{x_}, y{y_}, z{z_} {}

    friend constexpr bool operator==(const Vec3&, const Vec3&) noexcept = default;

    constexpr Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }

    constexpr Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }

    constexpr Vec3 operator*(T s) const noexcept { return {x * s, y * s, z * s}; }

    constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }

    constexpr Vec3& operator+=(const Vec3& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    constexpr Vec3& operator-=(const Vec3& o) noexcept {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }

    constexpr Vec3& operator*=(T s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    [[nodiscard]] constexpr T dot(const Vec3& o) const noexcept {
        return x * o.x + y * o.y + z * o.z;
    }

    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }

    /// Squared length. Prefer it over length() for comparisons: entity range
    /// checks run millions of times per tick and a square root is pure waste.
    [[nodiscard]] constexpr T length_squared() const noexcept { return dot(*this); }

    [[nodiscard]] T length() const noexcept { return std::sqrt(length_squared()); }

    [[nodiscard]] Vec3 normalized() const noexcept {
        const T len = length();
        return len > T{0} ? *this * (T{1} / len) : Vec3{};
    }
};

using Vec3d = Vec3<f64>;
using Vec3f = Vec3<f32>;
using Vec3i = Vec3<i32>;

/// The six block faces, in the order Minecraft assigns them. The numbering is
/// not arbitrary decoration: it is what the wire protocol sends for block
/// placement and what block models index, so it must not be reordered.
enum class Direction : u8 {
    Down  = 0,  // -Y
    Up    = 1,  // +Y
    North = 2,  // -Z
    South = 3,  // +Z
    West  = 4,  // -X
    East  = 5,  // +X
};

inline constexpr u8 kDirectionCount = 6;

[[nodiscard]] constexpr Vec3i direction_offset(Direction d) noexcept {
    switch (d) {
        case Direction::Down: return {0, -1, 0};
        case Direction::Up: return {0, 1, 0};
        case Direction::North: return {0, 0, -1};
        case Direction::South: return {0, 0, 1};
        case Direction::West: return {-1, 0, 0};
        case Direction::East: return {1, 0, 0};
    }
    return {};
}

[[nodiscard]] constexpr Direction opposite(Direction d) noexcept {
    // The enum is laid out in opposing pairs, so flipping the low bit is the
    // opposite face. Guarded by a test rather than left as folklore.
    return static_cast<Direction>(static_cast<u8>(d) ^ 1u);
}

/// The name Minecraft uses for a face, in blockstate files, command arguments
/// and NBT. Not a debug label: these strings are parsed and written.
[[nodiscard]] std::string_view direction_name(Direction d) noexcept;

/// Inverse of direction_name. Returns nullopt for anything else.
[[nodiscard]] std::optional<Direction> direction_from_name(std::string_view name) noexcept;

}  // namespace ov
