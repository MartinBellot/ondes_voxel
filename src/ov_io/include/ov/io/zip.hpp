// Read-only ZIP archives.
//
// Three things Ondes VOXEL has to read are ZIP files: the Minecraft client jar
// (block models, blockstates, fonts, language files), resource packs, and
// datapacks. A jar is a ZIP with a manifest; nothing here treats it specially.
//
// Written rather than pulled in, for two reasons. Reading is a few hundred
// lines given that libdeflate is already a dependency for the region files, and
// a resource pack is an untrusted file a player hands us — the interesting part
// of a ZIP reader is what it refuses, and that is easier to be sure of when it
// is ours.
//
// Deliberately not supported, and rejected rather than half-handled:
//   Zip64        entries above 4 GiB. A resource pack that large is not one.
//   encryption   no legitimate pack uses it.
//   methods      only stored (0) and deflate (8). Everything else is refused.
//
// Guarded against, because a ZIP is a hostile-input format:
//   path traversal   an entry named "../../etc/passwd" is refused outright
//   zip bombs        every extraction takes an explicit output cap
//   lying headers    the central directory is the authority, not the local one
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::io {

enum class ZipError {
    /// No end-of-central-directory record: not a ZIP file.
    NotAnArchive,
    /// Structurally broken beyond the point of guessing.
    Corrupt,
    /// Zip64, encryption, or a compression method other than store/deflate.
    Unsupported,
    /// Entry name is absolute or escapes the archive root.
    UnsafePath,
    /// No entry by that name.
    NotFound,
    /// Decompressed size exceeds the caller's cap.
    TooLarge,
};

[[nodiscard]] std::string_view to_string(ZipError error) noexcept;

template<typename T>
using ZipResult = std::expected<T, ZipError>;

struct ZipEntry {
    std::string name;
    /// Size once decompressed, as claimed by the central directory. A hint for
    /// sizing a buffer, never a substitute for the cap on extraction.
    u32 uncompressed_size{0};
    u32 compressed_size{0};
    u32 crc32{0};
    /// 0 = stored, 8 = deflate.
    u16 method{0};
    /// Offset of the local file header.
    u32 local_header_offset{0};

    [[nodiscard]] bool is_directory() const noexcept { return !name.empty() && name.back() == '/'; }
};

/// A resource pack or jar loaded into memory.
///
/// The whole file is held, and entries are inflated on demand: the client jar
/// is 23 MB holding some 10 000 files, and inflating all of them to read a
/// handful would be pure waste.
class ZipArchive {
public:
    [[nodiscard]] static ZipResult<ZipArchive> open(std::vector<u8> data);
    [[nodiscard]] static ZipResult<ZipArchive> open(const std::filesystem::path& path);

    /// Every entry, in central-directory order.
    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    /// Entry metadata, or nullptr when absent.
    [[nodiscard]] const ZipEntry* find(std::string_view name) const noexcept;

    /// Decompress one entry.
    [[nodiscard]] ZipResult<std::vector<u8>> read(std::string_view name,
                                                  usize limit = kDefaultEntryLimit) const;

    /// Entry names under a prefix, e.g. "assets/minecraft/blockstates/".
    /// Directories are excluded.
    [[nodiscard]] std::vector<std::string_view> list(std::string_view prefix) const;

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }

    [[nodiscard]] usize byte_size() const noexcept { return data_.size(); }

    /// A single asset is a texture, a model or a sound. 64 MiB is far above
    /// anything legitimate and still bounds a crafted pack.
    static constexpr usize kDefaultEntryLimit = 64ull * 1024 * 1024;

private:
    std::vector<u8>                             data_;
    std::vector<ZipEntry>                       entries_;
    std::unordered_map<std::string_view, usize> index_;
};

/// True if the name is safe to write under an output directory: relative, no
/// "..", no leading slash, no drive letter, no backslash separators.
///
/// Extracting an archive without this check is the classic Zip Slip: an entry
/// named "../../.ssh/authorized_keys" writes wherever the attacker likes.
[[nodiscard]] bool is_safe_archive_path(std::string_view name) noexcept;

}  // namespace ov::io
