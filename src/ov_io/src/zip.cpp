#include "ov/io/zip.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"

#include <algorithm>

namespace ov::io {
namespace {

// Signatures, little-endian on disk.
constexpr u32 kCentralHeaderSignature = 0x02014b50;
constexpr u32 kLocalHeaderSignature   = 0x04034b50;
constexpr u32 kZip64LocatorSignature  = 0x07064b50;

constexpr usize kEocdMinSize        = 22;
constexpr usize kLocalHeaderMinSize = 30;

// A ZIP comment is a 16-bit length, so the record starts at most this far back.
constexpr usize kMaxCommentSize = 0xFFFF;

constexpr u16 kMethodStore   = 0;
constexpr u16 kMethodDeflate = 8;

/// Bit 0 of the general-purpose flags means the entry is encrypted.
constexpr u16 kFlagEncrypted = 0x0001;

/// Find the end-of-central-directory record by scanning backwards.
///
/// It has no fixed position: a trailing comment of up to 64 KiB may follow it.
/// Scanning from the end finds the last one, which is the right one for an
/// archive that has been appended to.
[[nodiscard]] std::optional<usize> find_eocd(std::span<const u8> data) noexcept {
    if (data.size() < kEocdMinSize) {
        return std::nullopt;
    }
    const usize search_start = data.size() >= kEocdMinSize + kMaxCommentSize
                                   ? data.size() - kEocdMinSize - kMaxCommentSize
                                   : 0;

    for (usize offset = data.size() - kEocdMinSize + 1; offset-- > search_start;) {
        if (data[offset] == 0x50 && data[offset + 1] == 0x4b && data[offset + 2] == 0x05 &&
            data[offset + 3] == 0x06) {
            return offset;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(ZipError error) noexcept {
    switch (error) {
        case ZipError::NotAnArchive: return "not a ZIP archive";
        case ZipError::Corrupt: return "corrupt ZIP archive";
        case ZipError::Unsupported: return "unsupported ZIP feature";
        case ZipError::UnsafePath: return "unsafe entry path";
        case ZipError::NotFound: return "entry not found";
        case ZipError::TooLarge: return "entry exceeds size limit";
    }
    return "unknown ZIP error";
}

bool is_safe_archive_path(std::string_view name) noexcept {
    if (name.empty() || name.size() > 4096) {
        return false;
    }
    // Absolute paths escape the output directory outright.
    if (name.front() == '/' || name.front() == '\\') {
        return false;
    }
    // A Windows drive letter does the same on that platform.
    if (name.size() >= 2 && name[1] == ':') {
        return false;
    }
    // Backslashes are not a ZIP path separator; an entry using them is either
    // broken or trying to look harmless to a POSIX check while resolving
    // differently on Windows.
    if (name.find('\\') != std::string_view::npos) {
        return false;
    }

    // Reject any ".." component. Checking the substring alone would also reject
    // a legitimate "a..b", so this splits on separators.
    usize start = 0;
    while (start <= name.size()) {
        const usize end       = std::min(name.find('/', start), name.size());
        const auto  component = name.substr(start, end - start);
        if (component == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

ZipResult<ZipArchive> ZipArchive::open(std::vector<u8> data) {
    const auto eocd_offset = find_eocd(data);
    if (!eocd_offset) {
        return std::unexpected{ZipError::NotAnArchive};
    }

    ByteReader reader{std::span<const u8>{data}};
    reader.seek(*eocd_offset + 4);  // past the signature

    const auto disk_number   = reader.read_u16_le();
    const auto cd_start_disk = reader.read_u16_le();
    const auto entries_here  = reader.read_u16_le();
    const auto entries_total = reader.read_u16_le();
    const auto cd_size       = reader.read_u32_le();
    const auto cd_offset     = reader.read_u32_le();
    if (!disk_number || !cd_start_disk || !entries_here || !entries_total || !cd_size ||
        !cd_offset) {
        return std::unexpected{ZipError::Corrupt};
    }

    // Multi-disk archives are a floppy-era feature no pack uses.
    if (*disk_number != 0 || *cd_start_disk != 0) {
        return std::unexpected{ZipError::Unsupported};
    }

    // 0xFFFF entries or a 0xFFFFFFFF offset means the real values live in a
    // Zip64 record. Refuse rather than read the placeholder as a real number.
    if (*entries_total == 0xFFFF || *cd_offset == 0xFFFFFFFF || *cd_size == 0xFFFFFFFF) {
        return std::unexpected{ZipError::Unsupported};
    }
    if (*eocd_offset >= 20) {
        ByteReader locator{std::span<const u8>{data}};
        locator.seek(*eocd_offset - 20);
        if (const auto signature = locator.read_u32_le();
            signature && *signature == kZip64LocatorSignature) {
            return std::unexpected{ZipError::Unsupported};
        }
    }

    if (static_cast<usize>(*cd_offset) + *cd_size > data.size()) {
        return std::unexpected{ZipError::Corrupt};
    }

    ZipArchive archive;
    archive.data_ = std::move(data);
    archive.entries_.reserve(*entries_total);

    ByteReader central{std::span<const u8>{archive.data_}};
    central.seek(*cd_offset);

    for (u16 i = 0; i < *entries_total; ++i) {
        const auto signature = central.read_u32_le();
        if (!signature || *signature != kCentralHeaderSignature) {
            return std::unexpected{ZipError::Corrupt};
        }

        // version made by, version needed
        if (!central.skip(4))
            return std::unexpected{ZipError::Corrupt};

        const auto flags  = central.read_u16_le();
        const auto method = central.read_u16_le();
        if (!central.skip(4))
            return std::unexpected{ZipError::Corrupt};  // mod time, mod date
        const auto crc32             = central.read_u32_le();
        const auto compressed_size   = central.read_u32_le();
        const auto uncompressed_size = central.read_u32_le();
        const auto name_length       = central.read_u16_le();
        const auto extra_length      = central.read_u16_le();
        const auto comment_length    = central.read_u16_le();
        if (!central.skip(8))
            return std::unexpected{ZipError::Corrupt};  // disk, attrs
        const auto local_offset = central.read_u32_le();

        if (!flags || !method || !crc32 || !compressed_size || !uncompressed_size || !name_length ||
            !extra_length || !comment_length || !local_offset) {
            return std::unexpected{ZipError::Corrupt};
        }

        if ((*flags & kFlagEncrypted) != 0) {
            return std::unexpected{ZipError::Unsupported};
        }
        if (*compressed_size == 0xFFFFFFFF || *uncompressed_size == 0xFFFFFFFF ||
            *local_offset == 0xFFFFFFFF) {
            return std::unexpected{ZipError::Unsupported};  // Zip64
        }

        const auto name_bytes = central.read_bytes(*name_length);
        if (!name_bytes) {
            return std::unexpected{ZipError::Corrupt};
        }
        if (!central.skip(static_cast<usize>(*extra_length) + *comment_length)) {
            return std::unexpected{ZipError::Corrupt};
        }

        std::string name{reinterpret_cast<const char*>(name_bytes->data()), name_bytes->size()};

        // Refused at parse time, not at extraction time: an archive containing
        // a traversal entry is hostile, and the safest thing to do with the
        // rest of it is nothing.
        if (!name.empty() && name.back() != '/' && !is_safe_archive_path(name)) {
            return std::unexpected{ZipError::UnsafePath};
        }
        if (static_cast<usize>(*local_offset) + kLocalHeaderMinSize > archive.data_.size()) {
            return std::unexpected{ZipError::Corrupt};
        }

        archive.entries_.push_back(ZipEntry{std::move(name), *uncompressed_size, *compressed_size,
                                            *crc32, *method, *local_offset});
    }

    // Built after the vector stops growing, so the string_view keys stay valid.
    archive.index_.reserve(archive.entries_.size());
    for (usize i = 0; i < archive.entries_.size(); ++i) {
        archive.index_.emplace(std::string_view{archive.entries_[i].name}, i);
    }

    return archive;
}

ZipResult<ZipArchive> ZipArchive::open(const std::filesystem::path& path) {
    auto data = read_file(path);
    if (!data) {
        return std::unexpected{ZipError::NotAnArchive};
    }
    return open(std::move(*data));
}

const ZipEntry* ZipArchive::find(std::string_view name) const noexcept {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &entries_[it->second];
}

bool ZipArchive::contains(std::string_view name) const noexcept {
    return find(name) != nullptr;
}

std::vector<std::string_view> ZipArchive::list(std::string_view prefix) const {
    std::vector<std::string_view> names;
    for (const auto& entry : entries_) {
        if (!entry.is_directory() && entry.name.starts_with(prefix)) {
            names.emplace_back(entry.name);
        }
    }
    return names;
}

ZipResult<std::vector<u8>> ZipArchive::read(std::string_view name, usize limit) const {
    const ZipEntry* entry = find(name);
    if (entry == nullptr) {
        return std::unexpected{ZipError::NotFound};
    }
    if (entry->is_directory()) {
        return std::vector<u8>{};
    }

    // The central directory is the authority on sizes, but the local header is
    // what says where the data actually starts — its name and extra fields can
    // be a different length from the central directory's.
    ByteReader reader{std::span<const u8>{data_}};
    reader.seek(entry->local_header_offset);

    const auto signature = reader.read_u32_le();
    if (!signature || *signature != kLocalHeaderSignature) {
        return std::unexpected{ZipError::Corrupt};
    }
    if (!reader.skip(22)) {  // version, flags, method, time, date, crc, sizes
        return std::unexpected{ZipError::Corrupt};
    }
    const auto name_length  = reader.read_u16_le();
    const auto extra_length = reader.read_u16_le();
    if (!name_length || !extra_length) {
        return std::unexpected{ZipError::Corrupt};
    }
    if (!reader.skip(static_cast<usize>(*name_length) + *extra_length)) {
        return std::unexpected{ZipError::Corrupt};
    }

    const auto compressed = reader.read_bytes(entry->compressed_size);
    if (!compressed) {
        return std::unexpected{ZipError::Corrupt};
    }

    if (entry->uncompressed_size > limit) {
        return std::unexpected{ZipError::TooLarge};
    }

    if (entry->method == kMethodStore) {
        if (entry->compressed_size != entry->uncompressed_size) {
            return std::unexpected{ZipError::Corrupt};
        }
        return std::vector<u8>{compressed->begin(), compressed->end()};
    }

    if (entry->method != kMethodDeflate) {
        return std::unexpected{ZipError::Unsupported};
    }

    // Raw deflate: a ZIP entry has no zlib or gzip wrapper of its own, and the
    // exact output size comes from the central directory. If the two disagree
    // the archive contradicts itself, which deflate_decompress reports rather
    // than papers over.
    auto output = deflate_decompress(*compressed, entry->uncompressed_size);
    if (!output) {
        return std::unexpected{ZipError::Corrupt};
    }
    return std::move(*output);
}

}  // namespace ov::io
