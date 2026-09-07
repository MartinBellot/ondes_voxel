// Whole-file reading, and writing that cannot leave a half-written file behind.
#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "ov/base/types.hpp"

namespace ov::io {

enum class FileError {
    NotFound,
    PermissionDenied,
    /// File is larger than the caller's cap.
    TooLarge,
    /// Read or write failed partway through.
    IoFailure,
};

[[nodiscard]] std::string_view to_string(FileError error) noexcept;

template <typename T>
using FileResult = std::expected<T, FileError>;

/// A region file is at most a few tens of megabytes; 512 MiB is a generous
/// ceiling that still refuses to map something absurd into memory by accident.
inline constexpr usize kDefaultFileSizeLimit = 512ull * 1024 * 1024;

[[nodiscard]] FileResult<std::vector<u8>> read_file(const std::filesystem::path& path,
                                                    usize limit = kDefaultFileSizeLimit);

/// Write via a temporary file and rename over the target.
///
/// This is how level.dat has to be written: a crash or a power cut partway
/// through a plain write leaves a truncated world that will not load. Vanilla
/// keeps a level.dat_old for the same reason. Rename is atomic on every
/// platform we target, so the file on disk is always either the old one or the
/// new one, never a mixture.
[[nodiscard]] FileResult<void> write_file_atomic(const std::filesystem::path& path,
                                                 std::span<const u8> data);

}  // namespace ov::io
