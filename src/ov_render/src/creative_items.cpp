#define OV_LOG_CATEGORY "render"

#include "ov/render/creative_items.hpp"

#include "json.hpp"
#include "ov/base/log.hpp"
#include "ov/render/text_component.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace ov::render {

namespace {

/// Each component of a tooltip, re-serialised: json::Value gives views into a
/// parsed document, and the flattener wants the component's own text. A small
/// writer is cheaper than keeping a second copy of the file around.
void write_json(json::Value value, std::string& out) {
    switch (value.kind()) {
        case json::Kind::Null:
            out += "null";
            return;
        case json::Kind::Bool:
            out += value.as_bool() ? "true" : "false";
            return;
        case json::Kind::Number: {
            std::array<char, 32> buffer{};
            const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value.as_number());
            out.append(buffer.data(), static_cast<usize>(written));
            return;
        }
        case json::Kind::String: {
            out += '"';
            for (const char c : value.as_string()) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                    out += c;
                } else if (static_cast<u8>(c) < 0x20) {
                    std::array<char, 8> buffer{};
                    const int written = std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                                      static_cast<unsigned>(c));
                    out.append(buffer.data(), static_cast<usize>(written));
                } else {
                    out += c;
                }
            }
            out += '"';
            return;
        }
        case json::Kind::Array:
            out += '[';
            for (u32 i = 0; i < value.size(); ++i) {
                if (i != 0) {
                    out += ',';
                }
                write_json(value[i], out);
            }
            out += ']';
            return;
        case json::Kind::Object:
            out += '{';
            for (u32 i = 0; i < value.size(); ++i) {
                if (i != 0) {
                    out += ',';
                }
                out += '"';
                out += value.key_at(i);
                out += "\":";
                write_json(value.value_at(i), out);
            }
            out += '}';
            return;
    }
}

[[nodiscard]] std::vector<std::string> flatten_lines(json::Value lines, const Language& language) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    std::string text;
    for (u32 i = 0; i < lines.size(); ++i) {
        text.clear();
        write_json(lines[i], text);
        out.push_back(flatten_component(text, language));
    }
    return out;
}

/// Vanilla's search folds with the root locale; ASCII is all that changes for
/// the English names, and a non-ASCII byte is left as it is.
[[nodiscard]] std::string fold(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace

std::string_view to_string(CreativeItemsError error) noexcept {
    switch (error) {
        case CreativeItemsError::NotFound:
            return "not found";
        case CreativeItemsError::Malformed:
            return "malformed";
    }
    return "unknown";
}

std::expected<CreativeItems, CreativeItemsError> CreativeItems::load(
    const std::filesystem::path& file, const Language& language) {
    std::FILE* handle = std::fopen(file.string().c_str(), "rb");
    if (handle == nullptr) {
        return std::unexpected(CreativeItemsError::NotFound);
    }
    std::string               body;
    std::array<char, 1 << 16> buffer{};
    while (const usize read = std::fread(buffer.data(), 1, buffer.size(), handle)) {
        body.append(buffer.data(), read);
    }
    std::fclose(handle);
    return parse(body, language);
}

std::expected<CreativeItems, CreativeItemsError> CreativeItems::parse(std::string_view text,
                                                                     const Language& language) {
    auto document = json::Document::parse(text);
    if (!document) {
        return std::unexpected(CreativeItemsError::Malformed);
    }
    const json::Value root   = document->root();
    const json::Value stacks = root["stacks"];
    if (!stacks.is_array()) {
        return std::unexpected(CreativeItemsError::Malformed);
    }

    CreativeItems out;
    for (u32 i = 0; i < stacks.size(); ++i) {
        const json::Value stack = stacks[i];
        const std::string item(stack["item"].as_string());
        if (item.empty()) {
            return std::unexpected(CreativeItemsError::Malformed);
        }
        CreativeItemInfo info;
        // The search page's tooltip is the measured one. The category page's
        // is the same list without the tab names, which the real client puts
        // right after the name, one blue `itemGroup.*` line per tab holding
        // the item (measured: Stone carries both Building Blocks and Natural
        // Blocks). Derived here rather than dumped separately, because a dump
        // taken "on a category page" is only as good as the tab click before
        // it — and the first one was taken on the search page by mistake.
        const json::Value lines = stack["search_tooltip"];
        info.search_tooltip     = flatten_lines(lines, language);
        // The run of tab names starts at line 1 and ends at the first line
        // that is not one. A first version tested `tooltip.size() == line`,
        // which only holds for the *first* tab name: Stone kept "Natural
        // Blocks" in its search text and matched "red" and "e" — the parity
        // check caught it at 14 of 19 queries.
        bool in_tab_run = true;
        for (u32 line = 0; line < lines.size(); ++line) {
            const json::Value component = lines[line];
            const bool tab_name = line >= 1 && component["color"].as_string() == "blue"
                               && component["translate"].as_string().starts_with("itemGroup.");
            if (line >= 1 && in_tab_run && tab_name) {
                continue;
            }
            if (line >= 1) {
                in_tab_run = false;
            }
            info.tooltip.push_back(info.search_tooltip[line]);
        }
        for (usize line = 0; line < info.tooltip.size(); ++line) {
            if (line != 0) {
                info.search_text += '\n';
            }
            info.search_text += fold(strip_formatting(info.tooltip[line]));
        }
        const json::Value tints = stack["tints"];
        for (u32 layer = 0; layer < tints.size() && layer < info.tints.size(); ++layer) {
            // -1 is "not tinted". Anything else is a colour, and it can be
            // negative: the grass block's is -8602261, an ARGB int with the
            // alpha byte set. Treating every negative as "none" drew grass
            // grey — which is the bug this line replaced.
            const auto value = static_cast<i64>(tints[layer].as_number(-1.0));
            info.tints[layer] = value == -1 ? -1 : static_cast<i32>(value & 0xFFFFFF);
        }
        info.max_damage = static_cast<i32>(stack["max_damage"].as_number(0.0));
        const std::string_view equipment = stack["equipment"].as_string();
        info.armour_slot = equipment == "head"    ? 5
                         : equipment == "chest"   ? 6
                         : equipment == "legs"    ? 7
                         : equipment == "feet"    ? 8
                                                  : -1;
        auto& same_item = out.items_[item];
        out.order_.emplace_back(item, static_cast<u32>(same_item.size()));
        same_item.push_back(std::move(info));
        ++out.count_;
    }

    const json::Value queries = root["queries"];
    for (u32 q = 0; q < queries.size(); ++q) {
        CreativeQuery query;
        query.query = std::string(queries[q]["query"].as_string());
        // The results name stacks by id and SNBT; the occurrence is recovered
        // by counting, per id, the stacks of the dump in order until one with
        // the same SNBT is found.
        const json::Value results = queries[q]["results"];
        for (u32 r = 0; r < results.size(); ++r) {
            const std::string_view item = results[r]["item"].as_string();
            const json::Value      snbt = results[r]["snbt"];
            u32                    occurrence = 0;
            u32                    seen       = 0;
            bool                   found      = false;
            for (u32 i = 0; i < stacks.size() && !found; ++i) {
                if (stacks[i]["item"].as_string() != item) {
                    continue;
                }
                const json::Value candidate = stacks[i]["snbt"];
                const bool        same = candidate.is_string() == snbt.is_string()
                                  && candidate.as_string() == snbt.as_string();
                if (same) {
                    occurrence = seen;
                    found      = true;
                }
                ++seen;
            }
            query.results.emplace_back(std::string(item), found ? occurrence : 0xFFFFFFFFU);
        }
        out.queries_.push_back(std::move(query));
    }

    const json::Value hotbars = root["hotbars"];
    for (u32 h = 0; h < hotbars.size(); ++h) {
        out.hotbar_hints_.push_back(flatten_lines(hotbars[h]["tooltip"], language));
    }

    OV_LOG_INFO("creative items: {} stacks, {} queries", out.count_, out.queries_.size());
    return out;
}

std::unordered_map<std::string, std::unordered_set<std::string>> load_item_tags(
    const std::filesystem::path& data_directory) {
    // Raw first — tag id → its values, '#' entries unresolved — then a
    // resolution pass that follows the references, guarding against a cycle.
    std::unordered_map<std::string, std::vector<std::string>> raw;
    std::error_code                                           error;
    if (!std::filesystem::is_directory(data_directory, error)) {
        return {};
    }
    for (const auto& ns : std::filesystem::directory_iterator(data_directory, error)) {
        const std::filesystem::path items = ns.path() / "tags" / "items";
        if (!std::filesystem::is_directory(items, error)) {
            continue;
        }
        const std::string name_space = ns.path().filename().string();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(items, error)) {
            if (entry.path().extension() != ".json") {
                continue;
            }
            std::FILE* handle = std::fopen(entry.path().string().c_str(), "rb");
            if (handle == nullptr) {
                continue;
            }
            std::string               body;
            std::array<char, 1 << 14> buffer{};
            while (const usize read = std::fread(buffer.data(), 1, buffer.size(), handle)) {
                body.append(buffer.data(), read);
            }
            std::fclose(handle);
            auto document = json::Document::parse(body);
            if (!document) {
                continue;
            }
            std::string path = std::filesystem::relative(entry.path(), items, error).string();
            path             = path.substr(0, path.size() - 5);  // ".json"
            std::vector<std::string>& values = raw[name_space + ":" + path];
            const json::Value         list   = document->root()["values"];
            for (u32 i = 0; i < list.size(); ++i) {
                // An entry is an id or {"id": …, "required": …}.
                const json::Value value = list[i];
                values.emplace_back(value.is_string() ? value.as_string() : value["id"].as_string());
            }
        }
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> resolved;
    std::vector<std::string>                                         stack;
    const auto resolve = [&](const auto& self, const std::string& tag) -> const std::unordered_set<std::string>& {
        if (const auto done = resolved.find(tag); done != resolved.end()) {
            return done->second;
        }
        std::unordered_set<std::string> members;
        if (std::find(stack.begin(), stack.end(), tag) == stack.end()) {
            stack.push_back(tag);
            if (const auto found = raw.find(tag); found != raw.end()) {
                for (const std::string& value : found->second) {
                    if (!value.empty() && value.front() == '#') {
                        const auto& inner = self(self, value.substr(1));
                        members.insert(inner.begin(), inner.end());
                    } else {
                        members.insert(value);
                    }
                }
            }
            stack.pop_back();
        }
        return resolved.emplace(tag, std::move(members)).first->second;
    };
    for (const auto& [tag, values] : raw) {
        (void)values;
        (void)resolve(resolve, tag);
    }
    return resolved;
}

const CreativeItemInfo* CreativeItems::find(std::string_view item, u32 occurrence) const noexcept {
    const auto found = items_.find(std::string(item));
    if (found == items_.end() || occurrence >= found->second.size()) {
        return nullptr;
    }
    return &found->second[occurrence];
}

}  // namespace ov::render
