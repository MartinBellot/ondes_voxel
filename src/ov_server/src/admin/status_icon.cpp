#include "status_icon.hpp"

#include "ov/io/file.hpp"

#include <algorithm>
#include <array>

namespace ov::server::admin {

std::string base64(std::span<const u8> bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const u32 n = (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8) |
                      bytes[i + 2];
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (const usize rest = bytes.size() - i; rest > 0) {
        u32 n = static_cast<u32>(bytes[i]) << 16;
        if (rest == 2) {
            n |= static_cast<u32>(bytes[i + 1]) << 8;
        }
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += rest == 2 ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::expected<std::string, std::string> status_icon(std::span<const u8> png) {
    static constexpr std::array<u8, 8> kSignature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    // Signature, then the IHDR chunk: length, "IHDR", width, height.
    if (png.size() < 24 || !std::equal(kSignature.begin(), kSignature.end(), png.begin()) ||
        png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R') {
        return std::unexpected{std::string{"not a PNG image"}};
    }
    const auto be32 = [&](usize at) {
        return (static_cast<u32>(png[at]) << 24) | (static_cast<u32>(png[at + 1]) << 16) |
               (static_cast<u32>(png[at + 2]) << 8) | static_cast<u32>(png[at + 3]);
    };
    if (be32(16) != 64) {
        return std::unexpected{std::string{"Must be 64 pixels wide"}};
    }
    if (be32(20) != 64) {
        return std::unexpected{std::string{"Must be 64 pixels high"}};
    }
    return "data:image/png;base64," + base64(png);
}

std::optional<std::expected<std::string, std::string>> load_status_icon(
    const std::filesystem::path& path) {
    const auto bytes = io::read_file(path);
    if (!bytes) {
        return std::nullopt;
    }
    return status_icon(*bytes);
}

}  // namespace ov::server::admin
