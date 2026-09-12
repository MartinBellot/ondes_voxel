// whitelist.json, banned-players.json, banned-ips.json — vanilla's files, in
// vanilla's shape, beside the server.
//
//     [
//       {
//         "uuid": "…",
//         "name": "Steve",
//         "created": "2026-09-11 18:40:12 +0200",
//         "source": "Server",
//         "expires": "forever",
//         "reason": "Banned by an operator."
//       }
//     ]
//
// A file written by the real server is read here, and one written here is
// read by it: same keys in the same order, Gson's escaping, the entries in
// the order the real server's HashMap would put them (java_compat.hpp).
//
// Dates are the only wall-clock time on this server, and they are metadata
// for people, never game logic: the clock and the time zone are passed in, so
// the tests run on a fixed instant.
#pragma once

#include "java_compat.hpp"

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server::admin {

/// Where "now" and "which zone" come from. The server fills it with the
/// system's; tests with constants.
struct WallClock {
    std::function<i64()>            now;   // seconds since the epoch
    std::function<LocalZone(i64)>   zone;  // at that instant

    [[nodiscard]] static WallClock system();
    [[nodiscard]] static WallClock fixed(i64 now, i32 offset_seconds, std::string abbreviation);
};

/// Vanilla's default reason, written when a ban names none.
inline constexpr std::string_view kDefaultBanReason = "Banned by an operator.";

struct BanEntry {
    /// A player's uuid (hyphenated) or an IP address: what the file is keyed by.
    std::string        key;
    /// Players only.
    std::string        name;
    i64                created{0};
    std::string        source{"(Unknown)"};
    std::optional<i64> expires;  // none: "forever"
    std::string        reason{kDefaultBanReason};

    [[nodiscard]] bool expired(i64 now) const noexcept { return expires && *expires < now; }
};

struct WhiteEntry {
    net::Uuid   uuid{};
    std::string name;
};

/// One JSON list file. `Kind` decides the keys an entry is written with.
class BanList {
public:
    enum class Kind : u8 { Players, Ips };

    BanList(Kind kind, std::filesystem::path path, WallClock clock);

    /// Read the file. A missing file is an empty list; a malformed one leaves
    /// the list empty and returns false, so the caller can say so.
    bool load();
    bool save() const;

    [[nodiscard]] std::string to_json() const;
    bool                      from_json(std::string_view text);

    /// The live entry for a key; an expired one is dropped on the way, as
    /// vanilla's list does when it is asked.
    [[nodiscard]] const BanEntry* get(std::string_view key);
    [[nodiscard]] bool            contains(std::string_view key) { return get(key) != nullptr; }

    /// Replaces an entry with the same key, keeping its place.
    void add(BanEntry entry);
    bool remove(std::string_view key);

    [[nodiscard]] const std::vector<BanEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const WallClock&             clock() const noexcept { return clock_; }

private:
    void remove_expired();

    Kind                  kind_;
    std::filesystem::path path_;
    WallClock             clock_;
    std::vector<BanEntry> entries_;
    usize                 high_water_{0};
};

class WhiteList {
public:
    explicit WhiteList(std::filesystem::path path) : path_{std::move(path)} {}

    bool load();
    bool save() const;

    [[nodiscard]] std::string to_json() const;
    bool                      from_json(std::string_view text);

    [[nodiscard]] bool contains(const net::Uuid& uuid) const;
    /// False when already there.
    bool add(WhiteEntry entry);
    bool remove(const net::Uuid& uuid);

    /// Names in the file's order, which is the order `whitelist list` prints.
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] const std::vector<WhiteEntry>& entries() const noexcept { return entries_; }

private:
    std::filesystem::path   path_;
    std::vector<WhiteEntry> entries_;
    usize                   high_water_{0};
};

/// The order a list keyed by these strings is saved in.
[[nodiscard]] std::vector<usize> saved_order(std::span<const std::string> keys, usize high_water);

}  // namespace ov::server::admin
