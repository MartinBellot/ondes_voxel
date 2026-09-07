// A namespaced identifier: `minecraft:stone`, `ov:test/fixture`.
//
// Every name in the game is one of these — blocks, items, biomes, recipes,
// tags, sounds, advancements, loot tables, dimensions. They arrive from three
// untrusted places (datapack JSON, resource pack paths, the network) and are
// compared millions of times, so parsing is strict and comparison is cheap.
//
// Storage is one canonical string, `namespace:path`, with the namespace always
// written out. That makes equality a single string compare, hashing a single
// pass, and printing free. The alternative — two strings — costs two
// allocations and two compares for no benefit, and the namespace is elided so
// often in source data that storing it implicitly invites bugs where
// `stone` and `minecraft:stone` fail to match.
//
// Layering note: the plan filed this under ov_registry. It lives in ov_base
// instead, because the datapack loader in ov_data sits *below* ov_registry and
// deals in these identifiers constantly. The type has no dependencies of its
// own, so the lower home costs nothing.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace ov {

/// The namespace assumed when a location omits one.
inline constexpr std::string_view kDefaultNamespace = "minecraft";

/// Why a string is not a valid resource location.
enum class ResourceLocationError : u8 {
    /// The path is empty. `minecraft:` names nothing.
    EmptyPath,
    /// A namespace character outside `[a-z0-9_.-]`. Uppercase is the usual cause.
    InvalidNamespace,
    /// A path character outside `[a-z0-9_.-/]`.
    InvalidPath,
    /// Longer than an identifier may be on the wire.
    TooLong,
};

[[nodiscard]] std::string_view to_string(ResourceLocationError error) noexcept;

/// The protocol sends identifiers as strings capped at 32767 UTF-16 units.
/// Nothing in the game comes close, but the decoder must not be the place
/// where an oversized one is discovered.
inline constexpr usize kMaxResourceLocationLength = 32767;

/// A validated `namespace:path` identifier.
///
/// Always canonical: an instance that exists has a non-empty path, a namespace
/// (defaulted to `minecraft` when the source omitted one), and only characters
/// the format permits. There is no way to build an invalid one, which is why
/// none of the accessors can fail.
///
/// ⚠️ Validity is **not** a filesystem guarantee. Both `.` and `/` are legal
/// path characters, so `minecraft:../../etc/passwd` is a well-formed location
/// — as it is in vanilla. Anything that turns a location into a file path must
/// validate that path separately.
class ResourceLocation {
public:
    /// Parse `namespace:path`, or a bare `path` in the default namespace.
    ///
    /// A leading colon is not an error: `:stone` is `minecraft:stone`. Source
    /// data does contain them, and rejecting them would refuse files the game
    /// accepts.
    [[nodiscard]] static std::expected<ResourceLocation, ResourceLocationError> parse(
        std::string_view text);

    /// Build from parts already separated. An empty namespace defaults.
    [[nodiscard]] static std::expected<ResourceLocation, ResourceLocationError> make(
        std::string_view name_space, std::string_view path);

    /// The namespace, never empty.
    [[nodiscard]] std::string_view name_space() const noexcept {
        return std::string_view{full_}.substr(0, colon_);
    }

    /// The path, never empty.
    [[nodiscard]] std::string_view path() const noexcept {
        return std::string_view{full_}.substr(colon_ + 1);
    }

    /// The canonical `namespace:path` form — what goes on the wire and to disk.
    [[nodiscard]] const std::string& full() const noexcept { return full_; }

    /// True for the vanilla namespace, which is most of them.
    [[nodiscard]] bool is_vanilla() const noexcept { return name_space() == kDefaultNamespace; }

    [[nodiscard]] friend bool operator==(const ResourceLocation& a,
                                         const ResourceLocation& b) noexcept {
        return a.full_ == b.full_;
    }

    /// Ordered by namespace then path, so a sorted range groups by namespace.
    /// Comparing the canonical strings gives exactly that, since ':' (0x3A)
    /// sorts below every character a path may contain.
    [[nodiscard]] friend auto operator<=>(const ResourceLocation& a,
                                          const ResourceLocation& b) noexcept {
        return a.full_ <=> b.full_;
    }

private:
    ResourceLocation(std::string full, u32 colon) : full_{std::move(full)}, colon_{colon} {}

    std::string full_;
    u32         colon_{0};
};

/// True if every character may appear in a namespace: `[a-z0-9_.-]`.
[[nodiscard]] bool is_valid_namespace(std::string_view text) noexcept;

/// True if every character may appear in a path: `[a-z0-9_.-/]`.
[[nodiscard]] bool is_valid_path(std::string_view text) noexcept;

}  // namespace ov

template<>
struct std::hash<ov::ResourceLocation> {
    [[nodiscard]] std::size_t operator()(const ov::ResourceLocation& location) const noexcept {
        return std::hash<std::string>{}(location.full());
    }
};
