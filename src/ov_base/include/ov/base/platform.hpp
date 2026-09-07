// Platform, compiler and architecture detection.
#pragma once

// ── Platform ────────────────────────────────────────────────────────────────
#if defined(_WIN32)
#    define OV_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#    define OV_PLATFORM_MACOS 1
#elif defined(__linux__)
#    define OV_PLATFORM_LINUX 1
#else
#    error "Ondes VOXEL supports Windows, macOS and Linux."
#endif

#if !defined(OV_PLATFORM_WINDOWS)
#    define OV_PLATFORM_WINDOWS 0
#endif
#if !defined(OV_PLATFORM_MACOS)
#    define OV_PLATFORM_MACOS 0
#endif
#if !defined(OV_PLATFORM_LINUX)
#    define OV_PLATFORM_LINUX 0
#endif
#define OV_PLATFORM_POSIX (OV_PLATFORM_MACOS || OV_PLATFORM_LINUX)

// ── Compiler ────────────────────────────────────────────────────────────────
#if defined(__clang__)
#    define OV_COMPILER_CLANG 1
#elif defined(__GNUC__)
#    define OV_COMPILER_GCC 1
#elif defined(_MSC_VER)
#    define OV_COMPILER_MSVC 1
#endif

#if !defined(OV_COMPILER_CLANG)
#    define OV_COMPILER_CLANG 0
#endif
#if !defined(OV_COMPILER_GCC)
#    define OV_COMPILER_GCC 0
#endif
#if !defined(OV_COMPILER_MSVC)
#    define OV_COMPILER_MSVC 0
#endif

// ── Byte order ──────────────────────────────────────────────────────────────
// The Minecraft wire protocol and the NBT/Anvil formats are big-endian
// throughout. Every supported target is little-endian, so every read and write
// swaps; this is asserted rather than assumed.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    error "Ondes VOXEL assumes a little-endian host (see ov/base/byte_order.hpp)."
#endif

// ── Attributes ──────────────────────────────────────────────────────────────
#if OV_COMPILER_MSVC
#    define OV_FORCE_INLINE __forceinline
#    define OV_NEVER_INLINE __declspec(noinline)
#    define OV_RESTRICT     __restrict
#else
#    define OV_FORCE_INLINE inline __attribute__((always_inline))
#    define OV_NEVER_INLINE __attribute__((noinline))
#    define OV_RESTRICT     __restrict__
#endif

// ── Cache line ──────────────────────────────────────────────────────────────
// Apple Silicon uses 128-byte cache lines, x86-64 uses 64. Getting this wrong
// costs 3-5x on lock-free queue heads and tails through false sharing.
#if defined(__aarch64__) && OV_PLATFORM_MACOS
#    define OV_CACHE_LINE 128
#else
#    define OV_CACHE_LINE 64
#endif

#define OV_CACHE_ALIGNED alignas(OV_CACHE_LINE)

// ── Misc ────────────────────────────────────────────────────────────────────
#define OV_UNUSED(x) (void)(x)

#if OV_COMPILER_MSVC
#    define OV_DEBUG_BREAK() __debugbreak()
#else
#    define OV_DEBUG_BREAK() __builtin_trap()
#endif
