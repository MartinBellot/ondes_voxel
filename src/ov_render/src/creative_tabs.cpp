#define OV_LOG_CATEGORY "render"

#include "ov/render/creative_tabs.hpp"

#include "ov/base/log.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace ov::render {

namespace {

/// Base64, because the NBT of a cell travels through JSON.
///
/// Refuses rather than skipping a bad character: a tag decoded from a
/// truncated payload is a tag that means something else, and a potion of the
/// wrong effect is exactly the kind of quietly-wrong this repository does not
/// ship.
[[nodiscard]] bool decode_base64(std::string_view text, std::vector<u8>& out) {
    auto value = [](char c) -> i32 {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    u32 accumulator = 0;
    i32 bits        = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const i32 six = value(c);
        if (six < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<u32>(six);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((accumulator >> bits) & 0xFFU));
        }
    }
    return true;
}

[[nodiscard]] CreativeTabRow row_of(std::string_view text) noexcept {
    return text == "BOTTOM" ? CreativeTabRow::Bottom : CreativeTabRow::Top;
}

[[nodiscard]] bool type_of(std::string_view text, CreativeTabType& out) noexcept {
    if (text == "CATEGORY") {
        out = CreativeTabType::Category;
    } else if (text == "SEARCH") {
        out = CreativeTabType::Search;
    } else if (text == "HOTBAR") {
        out = CreativeTabType::Hotbar;
    } else if (text == "INVENTORY") {
        out = CreativeTabType::Inventory;
    } else {
        // Refused and named. A tab kind we do not know how to draw must not
        // become a category by default: it would show a page of nothing under
        // a button that promises something.
        OV_LOG_WARN("creative tab type '{}' is not one this client draws", text);
        return false;
    }
    return true;
}

}  // namespace

std::string_view to_string(CreativeTabsError error) noexcept {
    switch (error) {
        case CreativeTabsError::NotFound:
            return "not found";
        case CreativeTabsError::Malformed:
            return "malformed";
    }
    return "unknown";
}

std::expected<CreativeTabs, CreativeTabsError> CreativeTabs::load(
    const std::filesystem::path& file) {
    std::FILE* handle = std::fopen(file.string().c_str(), "rb");
    if (handle == nullptr) {
        return std::unexpected(CreativeTabsError::NotFound);
    }
    std::string body;
    std::array<char, 1 << 16> buffer{};
    while (const usize read = std::fread(buffer.data(), 1, buffer.size(), handle)) {
        body.append(buffer.data(), read);
    }
    std::fclose(handle);
    return parse(body);
}

std::expected<CreativeTabs, CreativeTabsError> CreativeTabs::parse(std::string_view text) {
    auto document = json::Document::parse(text);
    if (!document) {
        return std::unexpected(CreativeTabsError::Malformed);
    }
    const json::Value root = document->root();
    if (!root.is_object()) {
        return std::unexpected(CreativeTabsError::Malformed);
    }
    const json::Value tabs = root["tabs"];
    if (!tabs.is_array()) {
        return std::unexpected(CreativeTabsError::Malformed);
    }

    CreativeTabs out;
    out.op_permissions_ = root["op_permissions"].as_bool(false);

    // Two passes. The first sizes the arena and the stack list, so that the
    // string views handed out in the second are never invalidated by a
    // reallocation — the one bug this shape of table invites.
    usize characters = root["version"].as_string().size();
    usize cells      = 0;
    for (u32 t = 0; t < tabs.size(); ++t) {
        const json::Value tab = tabs[t];
        characters += tab["id"].as_string().size() + tab["translation_key"].as_string().size()
                    + tab["icon"].as_string().size();
        const json::Value display = tab["display"];
        cells += display.size();
        for (u32 i = 0; i < display.size(); ++i) {
            const json::Value entry = display[i];
            characters += (entry.is_string() ? entry : entry["item"]).as_string().size();
        }
    }
    out.strings_.reserve(characters);
    out.stacks_.reserve(cells);
    out.tabs_.reserve(tabs.size());
    // Three quarters of a base64 payload, rounded generously: one reserve
    // rather than a growth per cell.
    out.blobs_.reserve(cells * 24);

    auto intern = [&out](std::string_view value) -> std::string_view {
        const usize offset = out.strings_.size();
        out.strings_.append(value);
        return std::string_view(out.strings_).substr(offset, value.size());
    };

    out.version_ = intern(root["version"].as_string("unknown"));

    // Offsets first, spans afterwards: blobs_ and stacks_ both grow while the
    // loop runs, and a span taken mid-flight would dangle.
    std::vector<std::pair<usize, usize>> nbt_ranges;
    std::vector<std::pair<usize, usize>> stack_ranges;
    nbt_ranges.reserve(cells);
    stack_ranges.reserve(tabs.size());

    for (u32 t = 0; t < tabs.size(); ++t) {
        const json::Value tab = tabs[t];
        CreativeTabType   type{};
        if (!type_of(tab["type"].as_string(), type)) {
            continue;
        }

        CreativeTab entry;
        entry.id              = intern(tab["id"].as_string());
        entry.translation_key = intern(tab["translation_key"].as_string());
        entry.icon            = intern(tab["icon"].as_string());
        entry.row             = row_of(tab["row"].as_string());
        entry.column          = static_cast<i32>(tab["column"].as_number(0.0));
        entry.type            = type;
        entry.aligned_right   = tab["aligned_right"].as_bool(false);

        const json::Value display = tab["display"];
        const usize       first   = out.stacks_.size();
        for (u32 i = 0; i < display.size(); ++i) {
            const json::Value cell = display[i];
            CreativeStack     stack;
            usize             nbt_first = out.blobs_.size();
            usize             nbt_size  = 0;
            if (cell.is_string()) {
                stack.item = intern(cell.as_string());
            } else {
                stack.item  = intern(cell["item"].as_string());
                stack.count = static_cast<i32>(cell["count"].as_number(1.0));
                const json::Value nbt = cell["nbt"];
                if (nbt.is_string() && !decode_base64(nbt.as_string(), out.blobs_)) {
                    OV_LOG_WARN("creative cell {}: NBT is not base64; refused", stack.item);
                    out.blobs_.resize(nbt_first);
                }
                nbt_size = out.blobs_.size() - nbt_first;
            }
            nbt_ranges.emplace_back(nbt_first, nbt_size);
            out.stacks_.push_back(stack);
        }
        stack_ranges.emplace_back(first, out.stacks_.size() - first);
        out.tabs_.push_back(entry);
    }

    for (usize i = 0; i < out.stacks_.size(); ++i) {
        const auto [first, size] = nbt_ranges[i];
        out.stacks_[i].nbt = std::span<const u8>(out.blobs_).subspan(first, size);
    }
    for (usize i = 0; i < out.tabs_.size(); ++i) {
        const auto [first, size] = stack_ranges[i];
        out.tabs_[i].stacks = std::span<const CreativeStack>(out.stacks_).subspan(first, size);
    }
    return out;
}

std::vector<const CreativeTab*> CreativeTabs::row(CreativeTabRow which) const {
    std::vector<const CreativeTab*> found;
    for (const CreativeTab& tab : tabs_) {
        if (tab.row == which) {
            found.push_back(&tab);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const CreativeTab* a, const CreativeTab* b) { return a->column < b->column; });
    return found;
}

const CreativeTab* CreativeTabs::find(std::string_view id) const noexcept {
    for (const CreativeTab& tab : tabs_) {
        if (tab.id == id) {
            return &tab;
        }
    }
    return nullptr;
}

usize CreativeTabs::cell_count() const noexcept {
    usize total = 0;
    for (const CreativeTab& tab : tabs_) {
        if (tab.type == CreativeTabType::Category) {
            total += tab.stacks.size();
        }
    }
    return total;
}

}  // namespace ov::render
