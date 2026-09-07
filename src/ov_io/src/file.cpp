#include "ov/io/file.hpp"

#include <cstdio>
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

    FilePtr file{std::fopen(path.c_str(), "rb")};
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
        FilePtr file{std::fopen(temp.c_str(), "wb")};
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
