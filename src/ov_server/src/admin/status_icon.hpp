// server-icon.png: the picture beside the server in the multiplayer list.
//
// A PNG of exactly 64 by 64 pixels, sent in the Status Response as
// `data:image/png;base64,…`. The size is read from the PNG's IHDR chunk; a
// file of another size is refused with vanilla's reason and no icon is sent.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace ov::server::admin {

/// RFC 4648 base64, with padding.
[[nodiscard]] std::string base64(std::span<const u8> bytes);

/// The data URI for PNG bytes, or why they cannot be an icon.
[[nodiscard]] std::expected<std::string, std::string> status_icon(std::span<const u8> png);

/// None when there is no file; else the icon or the reason it was refused.
[[nodiscard]] std::optional<std::expected<std::string, std::string>> load_status_icon(
    const std::filesystem::path& path);

}  // namespace ov::server::admin
