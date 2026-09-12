#include "user_lists.hpp"

#include "../commands/json.hpp"

#include "ov/io/file.hpp"

#include <algorithm>
#include <chrono>

namespace ov::server::admin {
namespace {

[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path) {
    const auto bytes = io::read_file(path);
    if (!bytes) {
        return std::nullopt;
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

[[nodiscard]] bool write_text(const std::filesystem::path& path, const std::string& text) {
    if (path.empty()) {
        return true;
    }
    return io::write_file_atomic(
               path, std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()})
        .has_value();
}

[[nodiscard]] const cmd::JsonValue* string_member(const cmd::JsonValue& object,
                                                  std::string_view      key) {
    const cmd::JsonValue* value = object.find(key);
    return value != nullptr && value->type == cmd::JsonValue::Type::String ? value : nullptr;
}

/// Gson's pretty printer: two spaces, `": "`, one member per line, no final
/// newline; an empty array is `[]`.
class Pretty {
public:
    void begin_entry() {
        out_ += first_ ? "[\n  {" : ",\n  {";
        first_  = false;
        member_ = true;
    }
    void member(std::string_view key, std::string_view value) {
        out_ += member_ ? "\n    " : ",\n    ";
        member_ = false;
        append_gson_string(out_, key);
        out_ += ": ";
        append_gson_string(out_, value);
    }
    void end_entry() { out_ += "\n  }"; }
    [[nodiscard]] std::string finish() && {
        if (first_) {
            return "[]";
        }
        out_ += "\n]";
        return std::move(out_);
    }

private:
    std::string out_;
    bool        first_{true};
    bool        member_{true};
};

}  // namespace

WallClock WallClock::system() {
    return WallClock{
        [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        },
        [](i64 at) { return local_zone_at(at); }};
}

WallClock WallClock::fixed(i64 now, i32 offset_seconds, std::string abbreviation) {
    return WallClock{[now] { return now; },
                     [offset_seconds, abbreviation](i64) {
                         return LocalZone{offset_seconds, abbreviation};
                     }};
}

std::vector<usize> saved_order(std::span<const std::string> keys, usize high_water) {
    return hash_map_order(keys, high_water);
}

// ── BanList ─────────────────────────────────────────────────────────────────

BanList::BanList(Kind kind, std::filesystem::path path, WallClock clock)
    : kind_{kind}, path_{std::move(path)}, clock_{std::move(clock)} {}

std::string BanList::to_json() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const BanEntry& e : entries_) {
        keys.push_back(e.key);
    }
    const auto date = [&](i64 at) { return format_ban_date(at, clock_.zone(at).offset_seconds); };
    Pretty out;
    for (const usize i : saved_order(keys, high_water_)) {
        const BanEntry& e = entries_[i];
        out.begin_entry();
        if (kind_ == Kind::Players) {
            out.member("uuid", e.key);
            out.member("name", e.name);
        } else {
            out.member("ip", e.key);
        }
        out.member("created", date(e.created));
        out.member("source", e.source);
        out.member("expires", e.expires ? date(*e.expires) : std::string{"forever"});
        out.member("reason", e.reason);
        out.end_entry();
    }
    return std::move(out).finish();
}

bool BanList::from_json(std::string_view text) {
    entries_.clear();
    usize      consumed = 0;
    const auto parsed   = cmd::parse_json(text, consumed);
    if (!parsed || parsed->type != cmd::JsonValue::Type::Array) {
        return false;
    }
    for (const cmd::JsonValue& item : parsed->array) {
        if (item.type != cmd::JsonValue::Type::Object) {
            continue;
        }
        BanEntry entry;
        if (kind_ == Kind::Players) {
            const cmd::JsonValue* uuid = string_member(item, "uuid");
            if (uuid == nullptr) {
                continue;  // vanilla skips an entry without a profile id
            }
            const auto id = net::Uuid::parse(uuid->string);
            if (!id) {
                continue;
            }
            entry.key = id->to_string();
            if (const cmd::JsonValue* name = string_member(item, "name")) {
                entry.name = name->string;
            }
        } else {
            const cmd::JsonValue* ip = string_member(item, "ip");
            if (ip == nullptr) {
                continue;
            }
            entry.key = ip->string;
        }
        // An unreadable date is "now" for `created` and "forever" for
        // `expires` — vanilla's fallbacks.
        const cmd::JsonValue* created = string_member(item, "created");
        entry.created = created != nullptr ? parse_ban_date(created->string).value_or(clock_.now())
                                           : clock_.now();
        if (const cmd::JsonValue* source = string_member(item, "source")) {
            entry.source = source->string;
        }
        if (const cmd::JsonValue* expires = string_member(item, "expires")) {
            entry.expires = parse_ban_date(expires->string);
        }
        if (const cmd::JsonValue* reason = string_member(item, "reason")) {
            entry.reason = reason->string;
        }
        add(std::move(entry));
    }
    return true;
}

bool BanList::load() {
    const auto text = read_text(path_);
    if (!text) {
        entries_.clear();
        return true;
    }
    return from_json(*text);
}

bool BanList::save() const { return write_text(path_, to_json()); }

void BanList::remove_expired() {
    const i64 now = clock_.now();
    std::erase_if(entries_, [&](const BanEntry& e) { return e.expired(now); });
}

const BanEntry* BanList::get(std::string_view key) {
    remove_expired();
    const auto it = std::ranges::find(entries_, key, &BanEntry::key);
    return it == entries_.end() ? nullptr : &*it;
}

void BanList::add(BanEntry entry) {
    if (const auto it = std::ranges::find(entries_, entry.key, &BanEntry::key);
        it != entries_.end()) {
        *it = std::move(entry);
        return;
    }
    entries_.push_back(std::move(entry));
    high_water_ = std::max(high_water_, entries_.size());
}

bool BanList::remove(std::string_view key) {
    const auto before = entries_.size();
    std::erase_if(entries_, [&](const BanEntry& e) { return e.key == key; });
    return entries_.size() != before;
}

// ── WhiteList ───────────────────────────────────────────────────────────────

std::string WhiteList::to_json() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const WhiteEntry& e : entries_) {
        keys.push_back(e.uuid.to_string());
    }
    Pretty out;
    for (const usize i : saved_order(keys, high_water_)) {
        out.begin_entry();
        out.member("uuid", keys[i]);
        out.member("name", entries_[i].name);
        out.end_entry();
    }
    return std::move(out).finish();
}

bool WhiteList::from_json(std::string_view text) {
    entries_.clear();
    usize      consumed = 0;
    const auto parsed   = cmd::parse_json(text, consumed);
    if (!parsed || parsed->type != cmd::JsonValue::Type::Array) {
        return false;
    }
    for (const cmd::JsonValue& item : parsed->array) {
        const cmd::JsonValue* uuid = item.type == cmd::JsonValue::Type::Object
                                         ? string_member(item, "uuid")
                                         : nullptr;
        if (uuid == nullptr) {
            continue;
        }
        const auto id = net::Uuid::parse(uuid->string);
        if (!id) {
            continue;
        }
        const cmd::JsonValue* name = string_member(item, "name");
        (void)add(WhiteEntry{*id, name != nullptr ? name->string : std::string{}});
    }
    return true;
}

bool WhiteList::load() {
    const auto text = read_text(path_);
    if (!text) {
        entries_.clear();
        return true;
    }
    return from_json(*text);
}

bool WhiteList::save() const { return write_text(path_, to_json()); }

bool WhiteList::contains(const net::Uuid& uuid) const {
    return std::ranges::any_of(entries_, [&](const WhiteEntry& e) { return e.uuid == uuid; });
}

bool WhiteList::add(WhiteEntry entry) {
    if (contains(entry.uuid)) {
        return false;
    }
    entries_.push_back(std::move(entry));
    high_water_ = std::max(high_water_, entries_.size());
    return true;
}

bool WhiteList::remove(const net::Uuid& uuid) {
    const auto before = entries_.size();
    std::erase_if(entries_, [&](const WhiteEntry& e) { return e.uuid == uuid; });
    return entries_.size() != before;
}

std::vector<std::string> WhiteList::names() const {
    std::vector<std::string> keys;
    for (const WhiteEntry& e : entries_) {
        keys.push_back(e.uuid.to_string());
    }
    std::vector<std::string> out;
    for (const usize i : saved_order(keys, high_water_)) {
        out.push_back(entries_[i].name);
    }
    return out;
}

}  // namespace ov::server::admin
