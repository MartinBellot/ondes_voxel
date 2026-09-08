// Where client assets are read from, and in what order packs override.
//
// This is deliberately not the server's VFS. Client assets come from a *stack*
// of resource packs — the player's on top, the vanilla jar at the bottom — and
// the whole point is that the top one wins per file, not per pack. A single
// `read` that walks the stack downwards is the entire mechanism.
#pragma once

#include "ov/base/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::render {

/// Reads asset files by pack-relative path, e.g.
/// `assets/minecraft/models/block/cube.json`.
class AssetSource {
public:
    AssetSource()                              = default;
    AssetSource(const AssetSource&)            = default;
    AssetSource(AssetSource&&)                 = default;
    AssetSource& operator=(const AssetSource&) = default;
    AssetSource& operator=(AssetSource&&)      = default;
    virtual ~AssetSource();

    [[nodiscard]] virtual std::optional<std::vector<u8>> read(std::string_view path) const = 0;

    [[nodiscard]] bool contains(std::string_view path) const { return read(path).has_value(); }
};

/// A pack that has already been unpacked into a directory.
///
/// tools/ov_assetimport produces exactly this: the vanilla jar's assets with
/// the player's resource packs stacked on top, flattened into `run/assets`. A
/// zip-backed source belongs next to it once packs are loaded in place, and
/// changes nothing above this interface.
class DirectoryAssetSource final : public AssetSource {
public:
    explicit DirectoryAssetSource(std::filesystem::path root);

    [[nodiscard]] std::optional<std::vector<u8>> read(std::string_view path) const override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

/// Assets held in memory. Exists so that model and blockstate tests can state
/// their fixtures inline instead of depending on files nobody may commit.
class MemoryAssetSource final : public AssetSource {
public:
    void add(std::string path, std::string_view content);

    [[nodiscard]] std::optional<std::vector<u8>> read(std::string_view path) const override;

private:
    std::unordered_map<std::string, std::vector<u8>> files_;
};

/// A stack of packs. The most recently pushed source wins.
///
/// Holds references, not ownership: the sources outlive the stack, which is
/// what lets one directory source back several stacks without copying.
class AssetStack final : public AssetSource {
public:
    void push(const AssetSource& source);

    [[nodiscard]] std::optional<std::vector<u8>> read(std::string_view path) const override;

    [[nodiscard]] usize size() const noexcept { return sources_.size(); }

private:
    std::vector<const AssetSource*> sources_;
};

}  // namespace ov::render
