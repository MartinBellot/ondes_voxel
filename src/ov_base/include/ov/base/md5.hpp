// MD5.
//
// Present for one reason: a player's offline-mode UUID is a version 3 UUID,
// which is defined in terms of MD5. Vanilla computes it as
//
//     UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes(UTF_8))
//
// and that identity is what player data is filed under, so it has to match
// exactly or a world forgets who its players are.
//
// Not for security. MD5 is broken for every purpose that depends on collision
// resistance, and nothing here should use it for one.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ov {

using Md5Digest = std::array<u8, 16>;

[[nodiscard]] Md5Digest md5(std::span<const u8> data) noexcept;
[[nodiscard]] Md5Digest md5(std::string_view text) noexcept;

}  // namespace ov
