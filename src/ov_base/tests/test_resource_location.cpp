#include "ov/base/resource_location.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ov;

TEST_CASE("a namespaced identifier splits at the first colon", "[base][resource]") {
    const auto stone = ResourceLocation::parse("minecraft:stone");
    REQUIRE(stone.has_value());
    REQUIRE(stone->name_space() == "minecraft");
    REQUIRE(stone->path() == "stone");
    REQUIRE(stone->full() == "minecraft:stone");
    REQUIRE(stone->is_vanilla());
}

TEST_CASE("a bare path takes the default namespace", "[base][resource]") {
    // Datapacks and commands omit it constantly. Storing it implicitly would
    // make `stone` and `minecraft:stone` two different keys, which is the bug
    // this type exists to prevent.
    const auto bare = ResourceLocation::parse("stone");
    REQUIRE(bare.has_value());
    REQUIRE(bare->name_space() == "minecraft");
    REQUIRE(bare->full() == "minecraft:stone");

    REQUIRE(*bare == *ResourceLocation::parse("minecraft:stone"));
}

TEST_CASE("a leading colon means the default namespace, not an error", "[base][resource]") {
    // ":stone" appears in real data. Rejecting it would refuse files the game
    // loads without complaint.
    const auto leading = ResourceLocation::parse(":stone");
    REQUIRE(leading.has_value());
    REQUIRE(leading->full() == "minecraft:stone");
}

TEST_CASE("a path may contain slashes and a namespace may not", "[base][resource]") {
    // Loot tables and advancements are all nested paths.
    const auto nested = ResourceLocation::parse("minecraft:blocks/diamond_ore");
    REQUIRE(nested.has_value());
    REQUIRE(nested->path() == "blocks/diamond_ore");

    REQUIRE_FALSE(ResourceLocation::parse("some/where:thing").has_value());
    REQUIRE(ResourceLocation::parse("some/where:thing").error() ==
            ResourceLocationError::InvalidNamespace);
}

TEST_CASE("a custom namespace is preserved", "[base][resource]") {
    const auto ours = ResourceLocation::parse("ov:test/fixture");
    REQUIRE(ours.has_value());
    REQUIRE(ours->name_space() == "ov");
    REQUIRE_FALSE(ours->is_vanilla());
}

// ── What has to be refused ──────────────────────────────────────────────────

TEST_CASE("uppercase is not a valid identifier", "[base][resource][malformed]") {
    // The single most common mistake in hand-written datapacks, and the one
    // that silently names nothing if it is accepted.
    REQUIRE(ResourceLocation::parse("minecraft:Stone").error() ==
            ResourceLocationError::InvalidPath);
    REQUIRE(ResourceLocation::parse("Minecraft:stone").error() ==
            ResourceLocationError::InvalidNamespace);
}

TEST_CASE("an empty path names nothing", "[base][resource][malformed]") {
    REQUIRE(ResourceLocation::parse("").error() == ResourceLocationError::EmptyPath);
    REQUIRE(ResourceLocation::parse("minecraft:").error() == ResourceLocationError::EmptyPath);
    REQUIRE(ResourceLocation::parse(":").error() == ResourceLocationError::EmptyPath);
}

TEST_CASE("a second colon is a bad path character, not a second separator",
          "[base][resource][malformed]") {
    // Keeping the last segment would quietly turn "a:b:c" into "b:c" and hide
    // whatever produced it.
    const auto twice = ResourceLocation::parse("minecraft:stone:extra");
    REQUIRE_FALSE(twice.has_value());
    REQUIRE(twice.error() == ResourceLocationError::InvalidPath);
}

TEST_CASE("spaces and punctuation are refused", "[base][resource][malformed]") {
    REQUIRE_FALSE(ResourceLocation::parse("minecraft:red stone").has_value());
    REQUIRE_FALSE(ResourceLocation::parse("minecraft:stone!").has_value());
    REQUIRE_FALSE(ResourceLocation::parse("minecraft:sto\\ne").has_value());
}

TEST_CASE("a valid identifier is not a safe file path", "[base][resource][malformed]") {
    // Both '.' and '/' are legal path characters, so "../../etc/passwd" is a
    // perfectly well-formed resource location — and vanilla accepts it too.
    //
    // This is pinned as a passing case on purpose. The dangerous belief is the
    // reverse one: that having parsed an identifier, joining it onto a
    // directory is safe. It is not, and anything that resolves a location to a
    // file must check the path itself (io::is_safe_archive_path and friends).
    const auto climbing = ResourceLocation::parse("minecraft:../../etc/passwd");
    REQUIRE(climbing.has_value());
    REQUIRE(climbing->path() == "../../etc/passwd");
}

TEST_CASE("an oversized identifier is refused before it is built", "[base][resource][malformed]") {
    const std::string huge(kMaxResourceLocationLength + 1, 'a');
    REQUIRE(ResourceLocation::parse(huge).error() == ResourceLocationError::TooLong);
}

TEST_CASE("the permitted character sets are exactly these", "[base][resource]") {
    // Pinned rather than derived: std::isalnum is locale-dependent and would
    // accept different characters on different machines.
    for (int c = 0; c < 256; ++c) {
        const char        ch = static_cast<char>(c);
        const std::string one(1, ch);

        const bool expected_ns = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                                 ch == '_' || ch == '.' || ch == '-';
        REQUIRE(is_valid_namespace(one) == expected_ns);
        REQUIRE(is_valid_path(one) == (expected_ns || ch == '/'));
    }
}

// ── Use as a key ────────────────────────────────────────────────────────────

TEST_CASE("identifiers hash and compare as map keys", "[base][resource]") {
    std::unordered_set<ResourceLocation> seen;
    seen.insert(*ResourceLocation::parse("minecraft:stone"));
    seen.insert(*ResourceLocation::parse("stone"));  // the same thing
    seen.insert(*ResourceLocation::parse("minecraft:dirt"));

    REQUIRE(seen.size() == 2);
    REQUIRE(seen.contains(*ResourceLocation::parse("minecraft:stone")));
}

TEST_CASE("sorting groups by namespace then path", "[base][resource]") {
    // ':' sorts below every character a path may contain, so comparing the
    // canonical strings gives namespace-major order for free. Without that,
    // "ov:a" and "ov_extra:a" would interleave.
    std::vector<ResourceLocation> locations{
        *ResourceLocation::parse("ov:zebra"),   *ResourceLocation::parse("minecraft:stone"),
        *ResourceLocation::parse("ov:apple"),   *ResourceLocation::parse("minecraft:dirt"),
        *ResourceLocation::parse("ov_x:apple"),
    };
    std::ranges::sort(locations);

    std::vector<std::string> full;
    for (const auto& location : locations) {
        full.push_back(location.full());
    }
    REQUIRE(full == std::vector<std::string>{"minecraft:dirt", "minecraft:stone", "ov:apple",
                                             "ov:zebra", "ov_x:apple"});
}
