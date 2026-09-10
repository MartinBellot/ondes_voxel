// The creative inventory's tabs, and what is in each of them.
//
// This table is *not* in the source. In 1.20.1 the tabs are built by the
// game's own `CreativeModeTabs`, the vanilla client never receives them over
// the network, and an order written from memory would be wrong in a way that
// looks right — which is precisely what this repository refuses. So the list
// is measured, by running the shipped server jar and asking it:
//
//     scripts/measure_creative_tabs.py --contents
//
// writes `data/vanilla/1.20.1/creative_tabs.json`, which is gitignored like
// every other datum derived from Mojang's files; only its SHA-256 is
// committed, in the script. This class reads that file, and refuses to invent
// anything when it is absent.
//
// See docs/provenance/inventaire-creatif.md.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render {

enum class CreativeTabsError : u8 {
    /// No such file. The caller must say so rather than draw empty tabs.
    NotFound,
    /// Not the JSON this loader expects.
    Malformed,
};

[[nodiscard]] std::string_view to_string(CreativeTabsError error) noexcept;

/// Where a tab sits in the two rows of seven along the panel's edges.
enum class CreativeTabRow : u8 { Top, Bottom };

/// What a tab *is*. The three that are not a plain list of items each need a
/// different page, and drawing one as another would put the search field where
/// an item goes.
enum class CreativeTabType : u8 {
    /// A page of items: the usual case.
    Category,
    /// The magnifying glass: every item, filtered by a text field.
    Search,
    /// The saved hotbars.
    Hotbar,
    /// The survival inventory, with armour, off hand and the destroy slot.
    Inventory,
};

/// One cell of a tab.
///
/// `nbt` is the exact bytes a Slot carries after the item id and the count —
/// TAG_Compound, an empty name, the payload — kept unparsed. 762 of the 1689
/// cells have one: the 42 potions of each kind, the 42 tipped arrows, the 39
/// enchanted books, the 8 goat horns, the 31 paintings, the 9 suspicious
/// stews. Without it a potion cell is "a potion" and not "a potion of Night
/// Vision".
struct CreativeStack {
    /// Borrowed from the owning CreativeTabs.
    std::string_view item;
    i32              count{1};
    std::span<const u8> nbt;
};

struct CreativeTab {
    std::string_view id;
    /// `itemGroup.buildingBlocks` and the like: the key ov_render::Language
    /// translates for the label.
    std::string_view translation_key;
    /// The item whose model is drawn on the tab button.
    std::string_view icon;
    CreativeTabRow   row{CreativeTabRow::Top};
    /// 0..6 along the row.
    i32             column{0};
    CreativeTabType type{CreativeTabType::Category};
    /// Vanilla draws the label of a right-aligned tab from its right edge.
    bool aligned_right{false};

    std::span<const CreativeStack> stacks;
};

class CreativeTabs {
public:
    [[nodiscard]] static std::expected<CreativeTabs, CreativeTabsError> load(
        const std::filesystem::path& file);

    /// Parse an in-memory document. Exists so the tests can state a fixture
    /// inline rather than depend on a file nobody may commit.
    [[nodiscard]] static std::expected<CreativeTabs, CreativeTabsError> parse(
        std::string_view text);

    [[nodiscard]] const std::vector<CreativeTab>& tabs() const noexcept { return tabs_; }

    /// The tabs of one row, in column order. Vanilla's own layout: seven
    /// buttons above the panel and seven below it.
    [[nodiscard]] std::vector<const CreativeTab*> row(CreativeTabRow which) const;

    /// The tab with this id, or null.
    [[nodiscard]] const CreativeTab* find(std::string_view id) const noexcept;

    /// Total cells across the category tabs. The number the provenance quotes.
    [[nodiscard]] usize cell_count() const noexcept;

    [[nodiscard]] std::string_view version() const noexcept { return version_; }

    /// True when the file was generated with operator permissions, which is
    /// the only case where the operator tab has contents.
    [[nodiscard]] bool op_permissions() const noexcept { return op_permissions_; }

private:
    CreativeTabs() = default;

    /// Every string the views above point into, in one block. Interning the
    /// 1689 item names into one arena is what keeps the whole table two
    /// allocations rather than three thousand.
    std::string          strings_;
    std::vector<u8>      blobs_;
    std::vector<CreativeStack> stacks_;
    std::vector<CreativeTab>   tabs_;
    std::string_view           version_;
    bool                       op_permissions_{false};
};

}  // namespace ov::render
