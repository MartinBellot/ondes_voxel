#include "ov/render/language.hpp"

#include "json.hpp"

#include <string>

namespace ov::render {

std::string_view to_string(LanguageError error) noexcept {
    switch (error) {
        case LanguageError::NotFound:
            return "no such language file in the pack stack";
        case LanguageError::Malformed:
            return "the language file is not a flat object of strings";
    }
    return "unknown";
}

std::expected<Language, LanguageError> Language::load(const AssetSource& source,
                                                      std::string_view   code) {
    std::string path = "assets/minecraft/lang/";
    path += code;
    path += ".json";
    const auto bytes = source.read(path);
    if (!bytes) {
        return std::unexpected(LanguageError::NotFound);
    }
    const auto document = json::Document::parse(*bytes);
    if (!document) {
        return std::unexpected(LanguageError::Malformed);
    }
    const json::Value root = document->root();
    if (!root.is_object()) {
        return std::unexpected(LanguageError::Malformed);
    }

    Language language;
    for (u32 i = 0; i < root.size(); ++i) {
        const json::Value value = root.value_at(i);
        if (value.is_string()) {
            language.entries_.emplace(std::string(root.key_at(i)), std::string(value.as_string()));
        }
    }
    return language;
}

std::string_view Language::translate(std::string_view key) const noexcept {
    const auto found = entries_.find(key);
    return found == entries_.end() ? key : std::string_view(found->second);
}

bool Language::contains(std::string_view key) const noexcept {
    return entries_.contains(key);
}

std::string_view Language::item_name(std::string_view item) const noexcept {
    const auto             colon = item.find(':');
    const std::string_view name_space =
        colon == std::string_view::npos ? std::string_view("minecraft") : item.substr(0, colon);
    const std::string_view path = colon == std::string_view::npos ? item : item.substr(colon + 1);

    std::string key;
    key.reserve(path.size() + name_space.size() + 8);
    key = "block.";
    key += name_space;
    key += '.';
    key += path;
    if (const auto found = entries_.find(key); found != entries_.end()) {
        return found->second;
    }
    key = "item.";
    key += name_space;
    key += '.';
    key += path;
    if (const auto found = entries_.find(key); found != entries_.end()) {
        return found->second;
    }
    return item;
}

}  // namespace ov::render
