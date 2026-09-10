// What the running 1.20.1 client says about each creative stack: its tooltip,
// the tint of each layer, its durability.
//
// None of this reaches the client over the wire, and none of it is in the
// server jar's data: a tooltip is assembled by client code (a potion lists its
// effects, a smithing template says what it applies to, an enchanted book
// names its enchantment), a spawn egg's two colours come from the client's
// item colour handlers. So it is *measured*, by running the user's own
// vanilla client — scripts/measure_creative_screen.py drives it and asks, for
// every stack of the search tab, what it would draw — and written to
// `data/vanilla/1.20.1/creative_items.json`, gitignored like every other datum
// derived from Mojang's files. See docs/provenance/inventaire-creatif.md.
//
// Absent is not fatal: the screen then shows the translated name alone and
// searches on it, and says so once in the log.
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::render {

class Language;

enum class CreativeItemsError : u8 {
    NotFound,
    Malformed,
};

[[nodiscard]] std::string_view to_string(CreativeItemsError error) noexcept;

struct CreativeItemInfo {
    /// The tooltip on a category page, one `§`-coded line each, already
    /// translated. Line 0 is the name, in its rarity colour.
    std::vector<std::string> tooltip;
    /// The tooltip on the search page: the same lines plus, at the end, the
    /// name of every tab that holds the item, in blue.
    std::vector<std::string> search_tooltip;
    /// The category tooltip, formatting stripped, lower-cased, one line per
    /// '\n'. What the search compares against.
    std::string search_text;
    /// Layer tints as 0xRRGGBB, or -1 for "not tinted". Three layers is more
    /// than any 1.20.1 item uses.
    std::array<i32, 3> tints{-1, -1, -1};
    i32                max_damage{0};
    /// The window-0 armour slot the running client would wear it in — 5 head,
    /// 6 chest, 7 legs, 8 feet — or −1. Measured: 15 head items (the helmets,
    /// the carved pumpkin, every skull and head), 7 chest (elytra included),
    /// 6 legs, 6 feet.
    i32 armour_slot{-1};
};

/// Item tags from the data generator's `data/<ns>/tags/items/*.json`, nested
/// `#tags` resolved: tag id → item ids. What a creative search that starts
/// with '#' filters on. An absent directory is an empty map.
[[nodiscard]] std::unordered_map<std::string, std::unordered_set<std::string>> load_item_tags(
    const std::filesystem::path& data_directory);

/// One query the oracle typed, and what the real client showed, in order.
struct CreativeQuery {
    std::string query;
    /// Item id and occurrence index among stacks of that id in the search tab.
    std::vector<std::pair<std::string, u32>> results;
};

class CreativeItems {
public:
    /// Tooltips are flattened with `language` at load time: a tooltip is text,
    /// and translating 1587 of them in a frame would be a stall.
    [[nodiscard]] static std::expected<CreativeItems, CreativeItemsError> load(
        const std::filesystem::path& file, const Language& language);

    [[nodiscard]] static std::expected<CreativeItems, CreativeItemsError> parse(
        std::string_view text, const Language& language);

    /// The info for the `occurrence`-th stack of `item` in the search tab's
    /// order, or null. Keyed that way rather than by NBT because both lists
    /// come from the same game code in the same order, and comparing SNBT
    /// against binary NBT would be a second parser to get wrong.
    [[nodiscard]] const CreativeItemInfo* find(std::string_view item,
                                               u32              occurrence) const noexcept;

    /// The first stack of `item`: what a stack in the player's own inventory
    /// is described by when its NBT is not one of the catalogue's.
    [[nodiscard]] const CreativeItemInfo* first(std::string_view item) const noexcept {
        return find(item, 0);
    }

    [[nodiscard]] usize size() const noexcept { return count_; }

    [[nodiscard]] const std::vector<CreativeQuery>& queries() const noexcept { return queries_; }

    /// The running client's search tab, in its order, as (item, occurrence):
    /// what the parity check holds our search page against.
    [[nodiscard]] const std::vector<std::pair<std::string, u32>>& order() const noexcept {
        return order_;
    }

    /// The saved-hotbar page as the real client showed it with nothing
    /// saved: for each of its 81 cells, the tooltip of the hint it holds
    /// (empty when the cell is empty).
    [[nodiscard]] const std::vector<std::vector<std::string>>& hotbar_hints() const noexcept {
        return hotbar_hints_;
    }

private:
    CreativeItems() = default;

    std::unordered_map<std::string, std::vector<CreativeItemInfo>> items_;
    std::vector<CreativeQuery>                                    queries_;
    std::vector<std::pair<std::string, u32>>                      order_;
    std::vector<std::vector<std::string>>                         hotbar_hints_;
    usize                                                         count_{0};
};

}  // namespace ov::render
