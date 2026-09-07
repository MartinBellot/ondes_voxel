#include "ov/base/resource_location.hpp"

#include <algorithm>

namespace ov {
namespace {

/// The character sets are the whole specification, so they are spelled out
/// rather than derived from ctype — `std::isalnum` is locale-dependent and
/// would accept different things on different machines, which for an
/// identifier that has to match Mojang's byte for byte is disqualifying.
[[nodiscard]] constexpr bool namespace_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

[[nodiscard]] constexpr bool path_char(char c) noexcept {
    return namespace_char(c) || c == '/';
}

}  // namespace

std::string_view to_string(ResourceLocationError error) noexcept {
    switch (error) {
        case ResourceLocationError::EmptyPath: return "empty path";
        case ResourceLocationError::InvalidNamespace: return "invalid character in namespace";
        case ResourceLocationError::InvalidPath: return "invalid character in path";
        case ResourceLocationError::TooLong: return "identifier too long";
    }
    return "unknown error";
}

bool is_valid_namespace(std::string_view text) noexcept {
    return std::ranges::all_of(text, namespace_char);
}

bool is_valid_path(std::string_view text) noexcept {
    return std::ranges::all_of(text, path_char);
}

std::expected<ResourceLocation, ResourceLocationError> ResourceLocation::make(
    std::string_view name_space, std::string_view path) {
    if (name_space.empty()) {
        name_space = kDefaultNamespace;
    }
    if (path.empty()) {
        return std::unexpected{ResourceLocationError::EmptyPath};
    }
    if (name_space.size() + 1 + path.size() > kMaxResourceLocationLength) {
        return std::unexpected{ResourceLocationError::TooLong};
    }
    if (!is_valid_namespace(name_space)) {
        return std::unexpected{ResourceLocationError::InvalidNamespace};
    }
    if (!is_valid_path(path)) {
        return std::unexpected{ResourceLocationError::InvalidPath};
    }

    std::string full;
    full.reserve(name_space.size() + 1 + path.size());
    full.append(name_space);
    full.push_back(':');
    full.append(path);

    return ResourceLocation{std::move(full), static_cast<u32>(name_space.size())};
}

std::expected<ResourceLocation, ResourceLocationError> ResourceLocation::parse(
    std::string_view text) {
    // Split at the *first* colon. A second one is not a separator — it is an
    // invalid path character, and reporting it as such is more useful than
    // silently keeping the last segment.
    const usize colon = text.find(':');
    if (colon == std::string_view::npos) {
        return make(kDefaultNamespace, text);
    }
    return make(text.substr(0, colon), text.substr(colon + 1));
}

}  // namespace ov
