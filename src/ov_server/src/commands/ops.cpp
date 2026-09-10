#include "ops.hpp"

#include "json.hpp"
#include "selector.hpp"
#include "text.hpp"

#include "ov/io/file.hpp"

#include <algorithm>
#include <cstdlib>

namespace ov::server::cmd {

std::optional<i32> OpList::level_of(const net::Uuid& uuid) const {
    for (const OpEntry& entry : entries_) {
        if (entry.uuid == uuid) {
            return entry.level;
        }
    }
    return std::nullopt;
}

bool OpList::add(OpEntry entry) {
    if (level_of(entry.uuid)) {
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

bool OpList::remove(const net::Uuid& uuid) {
    const auto before = entries_.size();
    std::erase_if(entries_, [&](const OpEntry& e) { return e.uuid == uuid; });
    return entries_.size() != before;
}

std::string OpList::to_json() const {
    if (entries_.empty()) {
        return "[]";
    }
    // Gson's pretty printer: two spaces, ": " after a name, a newline per
    // member. No trailing newline, as vanilla writes none.
    std::string out = "[\n";
    for (usize i = 0; i < entries_.size(); ++i) {
        const OpEntry& e = entries_[i];
        out += "  {\n    \"uuid\": ";
        append_json_string(out, e.uuid.to_string());
        out += ",\n    \"name\": ";
        append_json_string(out, e.name);
        out += ",\n    \"level\": " + std::to_string(e.level);
        out += ",\n    \"bypassesPlayerLimit\": ";
        out += e.bypasses_player_limit ? "true" : "false";
        out += "\n  }";
        out += i + 1 < entries_.size() ? ",\n" : "\n";
    }
    out += "]";
    return out;
}

bool OpList::from_json(std::string_view text) {
    entries_.clear();
    usize      consumed = 0;
    const auto parsed   = parse_json(text, consumed);
    if (!parsed || parsed->type != JsonValue::Type::Array) {
        return false;
    }
    for (const JsonValue& item : parsed->array) {
        const JsonValue* uuid  = item.find("uuid");
        const JsonValue* name  = item.find("name");
        const JsonValue* level = item.find("level");
        if (uuid == nullptr || uuid->type != JsonValue::Type::String) {
            continue;
        }
        const auto id = parse_java_uuid(uuid->string);
        if (!id) {
            continue;
        }
        OpEntry entry;
        entry.uuid = *id;
        entry.name = name != nullptr ? name->string : std::string{};
        if (level != nullptr && level->type == JsonValue::Type::Number) {
            entry.level = std::clamp(static_cast<i32>(std::strtol(level->string.c_str(), nullptr, 10)),
                                     0, 4);
        }
        if (const JsonValue* bypass = item.find("bypassesPlayerLimit");
            bypass != nullptr && bypass->type == JsonValue::Type::Bool) {
            entry.bypasses_player_limit = bypass->boolean;
        }
        entries_.push_back(std::move(entry));
    }
    return true;
}

bool OpList::load() {
    const auto bytes = io::read_file(path_);
    if (!bytes) {
        entries_.clear();
        return true;
    }
    return from_json(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
}

bool OpList::save() const {
    if (path_.empty()) {
        return true;
    }
    const std::string text = to_json();
    return io::write_file_atomic(
               path_, std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()})
        .has_value();
}

}  // namespace ov::server::cmd
