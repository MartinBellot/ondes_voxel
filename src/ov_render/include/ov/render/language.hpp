// A language file: `lang/en_us.json`, as a flat table of keys to strings.
//
// The interface needs it for exactly two things and both of them are visible
// the moment they are missing: the name that floats over the hotbar when the
// held item changes, and the title of a container window. `minecraft:oak_log`
// is not what a player expects to read, and neither is `block.minecraft.oak_log`.
//
// ov_assetimport extracts all 142 of the jar's language files, so this is a
// lookup rather than a table in the source. The one thing it does not do is
// *formatting*: vanilla's `%s` substitutions belong to chat components and to
// nothing the HUD draws.
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/asset_source.hpp"

#include <expected>
#include <map>
#include <string>
#include <string_view>

namespace ov::render {

enum class LanguageError : u8 {
    /// No such language file in the pack stack.
    NotFound,
    /// The file is not a flat JSON object of strings.
    Malformed,
};

[[nodiscard]] std::string_view to_string(LanguageError error) noexcept;

class Language {
public:
    /// `en_us` -> `assets/minecraft/lang/en_us.json`.
    [[nodiscard]] static std::expected<Language, LanguageError> load(const AssetSource& source,
                                                                     std::string_view code);

    /// The translation, or the key itself when there is none. Returning the key
    /// is what vanilla does, and it is far more useful than an empty string:
    /// a missing translation shows up as `block.minecraft.something` on screen
    /// rather than as a blank line nobody can trace.
    [[nodiscard]] std::string_view translate(std::string_view key) const noexcept;

    [[nodiscard]] bool contains(std::string_view key) const noexcept;

    /// The display name of an item by registry name, e.g. `minecraft:oak_log`.
    ///
    /// A block's item takes the *block*'s key, everything else the item's, and
    /// there is no way to tell which from the name alone — so both are tried,
    /// block first. That order matters: `minecraft:cake` has both an
    /// `item.minecraft.cake` and a `block.minecraft.cake`, and the game shows
    /// the block's.
    [[nodiscard]] std::string_view item_name(std::string_view item) const noexcept;

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

private:
    std::map<std::string, std::string, std::less<>> entries_;
};

}  // namespace ov::render
