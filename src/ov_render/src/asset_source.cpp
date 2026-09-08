#include "ov/render/asset_source.hpp"

#include "ov/io/file.hpp"

#include <utility>

namespace ov::render {

AssetSource::~AssetSource() = default;

DirectoryAssetSource::DirectoryAssetSource(std::filesystem::path root) : root_(std::move(root)) {}

std::optional<std::vector<u8>> DirectoryAssetSource::read(std::string_view path) const {
    // A pack-relative path never escapes the pack. Refusing "..", rather than
    // relying on the filesystem, keeps a downloaded pack from naming
    // ../../.ssh/id_rsa and getting it read back into a texture.
    if (path.empty() || path.front() == '/' || path.find("..") != std::string_view::npos) {
        return std::nullopt;
    }

    auto full = root_ / std::filesystem::path(path);
    auto data = io::read_file(full);
    if (!data) {
        return std::nullopt;
    }
    return std::move(*data);
}

void MemoryAssetSource::add(std::string path, std::string_view content) {
    files_.insert_or_assign(std::move(path), std::vector<u8>(content.begin(), content.end()));
}

std::optional<std::vector<u8>> MemoryAssetSource::read(std::string_view path) const {
    const auto it = files_.find(std::string(path));
    if (it == files_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void AssetStack::push(const AssetSource& source) {
    sources_.push_back(&source);
}

std::optional<std::vector<u8>> AssetStack::read(std::string_view path) const {
    for (auto it = sources_.rbegin(); it != sources_.rend(); ++it) {
        if (auto data = (*it)->read(path)) {
            return data;
        }
    }
    return std::nullopt;
}

}  // namespace ov::render
