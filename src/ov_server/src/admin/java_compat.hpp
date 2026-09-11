// What the dedicated server's files owe to the Java library they were first
// written with.
//
// `server.properties`, `ops.json`, `whitelist.json` and the two ban lists are
// read by people and by the real server alike, and each carries a detail that
// is not in any format document but in how a JDK class behaves:
//
//   * the **order** of the entries. The lists are a `java.util.HashMap` keyed
//     by a string, saved by walking the map; server.properties is a
//     `java.util.Properties`, which on the JDK 1.20.1 ships with (17) is a
//     `ConcurrentHashMap`. Neither is sorted: the order is bucket order, and a
//     bucket is a function of `String.hashCode`. Both are re-specified here
//     from the JDK's documented behaviour — hash spreading, power-of-two
//     tables, the resize thresholds — and checked against files the real
//     server wrote (docs/provenance/serveur-dedie.md);
//   * Gson's string escaping, which is HTML-safe by default: `=` is written
//     `=`, `'` `'`;
//   * `SimpleDateFormat`'s `yyyy-MM-dd HH:mm:ss Z` and `Date.toString()`.
//
// Nothing here reads a clock or a time zone by itself: the caller passes the
// instant and the offset, so every function is a pure function with tests.
#pragma once

#include "ov/base/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server::admin {

/// `String.hashCode()` of a UTF-8 string, over its UTF-16 code units.
[[nodiscard]] i32 java_string_hash(std::string_view utf8) noexcept;

/// The order a `java.util.HashMap<String, …>` iterates its keys in, given
/// the keys in insertion order and the largest size the map ever reached
/// (the table grows and never shrinks; `clear()` keeps it). Returns indices
/// into `keys`.
[[nodiscard]] std::vector<usize> hash_map_order(std::span<const std::string> keys,
                                                usize                        high_water);

/// A `java.util.concurrent.ConcurrentHashMap<String, …>`, only as far as its
/// iteration order goes: puts, the resizes they trigger, and `putAll` into a
/// fresh map (which presizes). JDK 17's algorithm.
class ConcurrentHashMapOrder {
public:
    /// `new ConcurrentHashMap<>(initial_capacity)`.
    explicit ConcurrentHashMapOrder(u32 initial_capacity = 16);

    /// `put(key, …)`: a new key goes to the end of its bin; an existing one
    /// keeps its place.
    void put(const std::string& key);

    /// The keys, in iteration order.
    [[nodiscard]] std::vector<std::string> keys() const;

    /// `new ConcurrentHashMap<>(capacity)` then `putAll(this)`.
    [[nodiscard]] ConcurrentHashMapOrder copied(u32 initial_capacity) const;

private:
    void presize(usize size);
    void resize();

    struct Node {
        std::string key;
        i32         hash{0};
    };
    std::vector<std::vector<Node>> table_;
    usize                          size_{0};
    i64                            size_ctl_{0};
};

/// Gson's `toJson` of a string, quotes included, HTML-safe as its default.
void append_gson_string(std::string& out, std::string_view text);

/// `yyyy-MM-dd HH:mm:ss Z` for an instant (seconds since the epoch) at an
/// offset from UTC in seconds: "2026-09-11 18:40:12 +0200".
[[nodiscard]] std::string format_ban_date(i64 epoch_seconds, i32 utc_offset_seconds);

/// The same pattern read back; nullopt when it does not parse.
[[nodiscard]] std::optional<i64> parse_ban_date(std::string_view text);

/// `yyyy-MM-dd HH:mm:ss z` — the form a login refusal shows an expiry in —
/// with `zone` the abbreviation ("CEST").
[[nodiscard]] std::string format_zone_date(i64 epoch_seconds, i32 utc_offset_seconds,
                                           std::string_view zone);

/// `Date.toString()`: "Thu Sep 11 18:40:12 CEST 2026".
[[nodiscard]] std::string format_java_date(i64 epoch_seconds, i32 utc_offset_seconds,
                                           std::string_view zone);

/// The host's offset from UTC and zone abbreviation at an instant. The only
/// function here that asks the system; used by the server, never by tests.
struct LocalZone {
    i32         offset_seconds{0};
    std::string abbreviation{"UTC"};
};
[[nodiscard]] LocalZone local_zone_at(i64 epoch_seconds);

}  // namespace ov::server::admin
