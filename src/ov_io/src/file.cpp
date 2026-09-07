#include "ov/io/file.hpp"

#include "ov/base/platform.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <system_error>

namespace ov::io {
namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/// Open a path, whatever the platform spells paths in.
///
/// On Windows std::filesystem::path::c_str() is const wchar_t*, so fopen does
/// not take it. Going through path.string() would compile, but it converts to
/// the active code page and throws or mangles on anything outside it — and the
/// paths this opens are a player's Minecraft folder and their user directory,
/// which routinely contain accented characters. _wfopen keeps them intact.
[[nodiscard]] FilePtr open_file(const std::filesystem::path& path, const char* mode) {
#if OV_PLATFORM_WINDOWS
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    return FilePtr{::_wfopen(path.c_str(), wide_mode.c_str())};
#else
    return FilePtr{std::fopen(path.c_str(), mode)};
#endif
}

}  // namespace

std::string_view to_string(FileError error) noexcept {
    switch (error) {
        case FileError::NotFound: return "file not found";
        case FileError::PermissionDenied: return "permission denied";
        case FileError::TooLarge: return "file too large";
        case FileError::IoFailure: return "I/O failure";
    }
    return "unknown file error";
}

FileResult<std::vector<u8>> read_file(const std::filesystem::path& path, usize limit) {
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected{std::filesystem::exists(path) ? FileError::PermissionDenied
                                                             : FileError::NotFound};
    }
    if (size > limit) {
        return std::unexpected{FileError::TooLarge};
    }

    FilePtr file{open_file(path, "rb")};
    if (!file) {
        return std::unexpected{FileError::NotFound};
    }

    std::vector<u8> data(static_cast<usize>(size));
    if (size > 0 && std::fread(data.data(), 1, data.size(), file.get()) != data.size()) {
        return std::unexpected{FileError::IoFailure};
    }
    return data;
}

FileResult<void> write_file_atomic(const std::filesystem::path& path, std::span<const u8> data) {
    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        FilePtr file{open_file(temp, "wb")};
        if (!file) {
            return std::unexpected{FileError::PermissionDenied};
        }
        if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file.get()) != data.size()) {
            std::filesystem::remove(temp);
            return std::unexpected{FileError::IoFailure};
        }
        // Flush before the rename: renaming a file whose contents are still in
        // the page cache would defeat the point of writing atomically.
        if (std::fflush(file.get()) != 0) {
            std::filesystem::remove(temp);
            return std::unexpected{FileError::IoFailure};
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp);
        return std::unexpected{FileError::IoFailure};
    }
    return {};
}

}  // namespace ov::io
