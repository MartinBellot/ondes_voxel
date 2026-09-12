#include "java_compat.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <numeric>

namespace ov::server::admin {
namespace {

/// UTF-8 to UTF-16 code units, lenient: a malformed byte stands for itself.
template <typename Fn>
void for_each_utf16(std::string_view utf8, Fn&& fn) {
    usize i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<u8>(utf8[i]);
        u32        cp = c;
        usize      len = 1;
        if (c >= 0xF0 && i + 3 < utf8.size()) {
            cp  = ((c & 0x07U) << 18) | ((static_cast<u8>(utf8[i + 1]) & 0x3FU) << 12) |
                 ((static_cast<u8>(utf8[i + 2]) & 0x3FU) << 6) |
                 (static_cast<u8>(utf8[i + 3]) & 0x3FU);
            len = 4;
        } else if (c >= 0xE0 && i + 2 < utf8.size()) {
            cp  = ((c & 0x0FU) << 12) | ((static_cast<u8>(utf8[i + 1]) & 0x3FU) << 6) |
                 (static_cast<u8>(utf8[i + 2]) & 0x3FU);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < utf8.size()) {
            cp  = ((c & 0x1FU) << 6) | (static_cast<u8>(utf8[i + 1]) & 0x3FU);
            len = 2;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            fn(static_cast<u16>(0xD800 + (cp >> 10)));
            fn(static_cast<u16>(0xDC00 + (cp & 0x3FF)));
        } else {
            fn(static_cast<u16>(cp));
        }
        i += len;
    }
}

/// HashMap.hash and ConcurrentHashMap.spread share the xor-shift; the
/// concurrent map also clears the sign bit.
[[nodiscard]] u32 spread(i32 h) noexcept {
    const auto u = static_cast<u32>(h);
    return u ^ (u >> 16);
}

[[nodiscard]] u32 table_size_for(u32 c) noexcept {
    u32 n = 1;
    while (n < c) {
        n <<= 1;
    }
    return n;
}

struct Civil {
    i64 year;
    i32 month;
    i32 day;
    i32 hour;
    i32 minute;
    i32 second;
    i32 weekday;  // 0 Sunday
};

/// Howard Hinnant's days-from-civil, both directions.
[[nodiscard]] i64 days_from_civil(i64 y, i32 m, i32 d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const i64 yoe = y - era * 400;
    const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

[[nodiscard]] Civil civil_from_epoch(i64 t) noexcept {
    i64 days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    i64 secs = t - days * 86400;
    Civil out{};
    out.hour    = static_cast<i32>(secs / 3600);
    out.minute  = static_cast<i32>(secs / 60 % 60);
    out.second  = static_cast<i32>(secs % 60);
    out.weekday = static_cast<i32>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
    days += 719468;
    const i64 era = (days >= 0 ? days : days - 146096) / 146097;
    const i64 doe = days - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp  = (5 * doy + 2) / 153;
    out.day       = static_cast<i32>(doy - (153 * mp + 2) / 5 + 1);
    out.month     = static_cast<i32>(mp < 10 ? mp + 3 : mp - 9);
    out.year      = yoe + era * 400 + (out.month <= 2 ? 1 : 0);
    return out;
}

[[nodiscard]] std::string offset_text(i32 offset) {
    const char sign = offset < 0 ? '-' : '+';
    const i32  a    = offset < 0 ? -offset : offset;
    std::array<char, 8> buf{};
    std::snprintf(buf.data(), buf.size(), "%c%02d%02d", sign, a / 3600, a / 60 % 60);
    return buf.data();
}

[[nodiscard]] std::string ymd_hms(const Civil& c) {
    std::array<char, 40> buf{};
    std::snprintf(buf.data(), buf.size(), "%04lld-%02d-%02d %02d:%02d:%02d",
                  static_cast<long long>(c.year), c.month, c.day, c.hour, c.minute, c.second);
    return buf.data();
}

}  // namespace

i32 java_string_hash(std::string_view utf8) noexcept {
    u32 h = 0;
    for_each_utf16(utf8, [&](u16 unit) { h = h * 31U + unit; });
    return static_cast<i32>(h);
}

std::vector<usize> hash_map_order(std::span<const std::string> keys, usize high_water) {
    // The table is allocated at 16 and doubles whenever the size passes three
    // quarters of it.
    usize capacity = 16;
    while (std::max(high_water, keys.size()) > capacity * 3 / 4) {
        capacity *= 2;
    }
    std::vector<usize> order(keys.size());
    std::iota(order.begin(), order.end(), usize{0});
    std::vector<u32> bucket(keys.size());
    for (usize i = 0; i < keys.size(); ++i) {
        bucket[i] = spread(java_string_hash(keys[i])) & static_cast<u32>(capacity - 1);
    }
    // A resize splits each bin into a low and a high half, each keeping its
    // order, so within a bin the order stays the insertion order.
    std::ranges::stable_sort(order, [&](usize a, usize b) { return bucket[a] < bucket[b]; });
    return order;
}

// ── ConcurrentHashMap ───────────────────────────────────────────────────────

ConcurrentHashMapOrder::ConcurrentHashMapOrder(u32 initial_capacity) {
    // new ConcurrentHashMap(initialCapacity): the table is sized for
    // 1.5 × capacity + 1, rounded up to a power of two.
    size_ctl_ = table_size_for(initial_capacity + (initial_capacity >> 1) + 1);
}

void ConcurrentHashMapOrder::put(const std::string& key) {
    if (table_.empty()) {
        const auto n = static_cast<usize>(size_ctl_ > 0 ? size_ctl_ : 16);
        table_.resize(n);
        size_ctl_ = static_cast<i64>(n - (n >> 2));
    }
    const i32 hash = static_cast<i32>(spread(java_string_hash(key)) & 0x7FFFFFFFU);
    auto&     bin  = table_[static_cast<usize>(hash) & (table_.size() - 1)];
    for (const Node& node : bin) {
        if (node.key == key) {
            return;
        }
    }
    bin.push_back(Node{key, hash});
    ++size_;
    // addCount: a resize whenever the count reaches sizeCtl.
    while (static_cast<i64>(size_) >= size_ctl_) {
        resize();
    }
}

void ConcurrentHashMapOrder::resize() {
    const usize n = table_.size();
    std::vector<std::vector<Node>> next(n * 2);
    // transfer(): bins are moved from the highest index down; within a bin,
    // the run of nodes at its tail that all go the same way ("lastRun") is
    // moved as it is, and each node before it is pushed onto the front of its
    // half — which reverses them.
    for (usize i = n; i-- > 0;) {
        const auto& bin = table_[i];
        if (bin.empty()) {
            continue;
        }
        usize last_run = 0;
        u32   run_bit  = static_cast<u32>(bin[0].hash) & static_cast<u32>(n);
        for (usize k = 1; k < bin.size(); ++k) {
            const u32 b = static_cast<u32>(bin[k].hash) & static_cast<u32>(n);
            if (b != run_bit) {
                run_bit  = b;
                last_run = k;
            }
        }
        std::vector<Node> low;
        std::vector<Node> high;
        auto& tail = run_bit == 0 ? low : high;
        tail.assign(bin.begin() + static_cast<std::ptrdiff_t>(last_run), bin.end());
        for (usize k = 0; k < last_run; ++k) {
            auto& half = (static_cast<u32>(bin[k].hash) & static_cast<u32>(n)) == 0 ? low : high;
            half.insert(half.begin(), bin[k]);
        }
        next[i]     = std::move(low);
        next[i + n] = std::move(high);
    }
    table_    = std::move(next);
    size_ctl_ = static_cast<i64>(table_.size() - (table_.size() >> 2));
}

void ConcurrentHashMapOrder::presize(usize size) {
    // tryPresize(size), as JDK 17 writes it — including its habit of growing
    // one step further than the size needs once the table exists.
    const u32 c = table_size_for(static_cast<u32>(size + (size >> 1) + 1));
    while (true) {
        if (table_.empty()) {
            const auto n = static_cast<usize>(size_ctl_ > static_cast<i64>(c) ? size_ctl_ : c);
            table_.resize(n);
            size_ctl_ = static_cast<i64>(n - (n >> 2));
        } else if (static_cast<i64>(c) <= size_ctl_) {
            break;
        } else {
            resize();
        }
    }
}

std::vector<std::string> ConcurrentHashMapOrder::keys() const {
    std::vector<std::string> out;
    out.reserve(size_);
    for (const auto& bin : table_) {
        for (const Node& node : bin) {
            out.push_back(node.key);
        }
    }
    return out;
}

ConcurrentHashMapOrder ConcurrentHashMapOrder::copied(u32 initial_capacity) const {
    ConcurrentHashMapOrder out{initial_capacity};
    out.presize(size_);
    for (const std::string& key : keys()) {
        out.put(key);
    }
    return out;
}

// ── Gson ────────────────────────────────────────────────────────────────────

void append_gson_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (usize i = 0; i < text.size(); ++i) {
        const auto c = static_cast<u8>(text[i]);
        switch (c) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\t': out += "\\t"; continue;
        case '\b': out += "\\b"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\f': out += "\\f"; continue;
        case '<': out += "\\u003c"; continue;
        case '>': out += "\\u003e"; continue;
        case '&': out += "\\u0026"; continue;
        case '=': out += "\\u003d"; continue;
        case '\'': out += "\\u0027"; continue;
        default: break;
        }
        if (c < 0x20) {
            std::array<char, 8> buf{};
            std::snprintf(buf.data(), buf.size(), "\\u%04x", c);
            out += buf.data();
            continue;
        }
        // U+2028 and U+2029, which JavaScript would read as line breaks.
        if (c == 0xE2 && i + 2 < text.size() && static_cast<u8>(text[i + 1]) == 0x80 &&
            (static_cast<u8>(text[i + 2]) == 0xA8 || static_cast<u8>(text[i + 2]) == 0xA9)) {
            out += static_cast<u8>(text[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
}

// ── Dates ───────────────────────────────────────────────────────────────────

std::string format_ban_date(i64 epoch_seconds, i32 utc_offset_seconds) {
    return ymd_hms(civil_from_epoch(epoch_seconds + utc_offset_seconds)) + " " +
           offset_text(utc_offset_seconds);
}

std::optional<i64> parse_ban_date(std::string_view text) {
    // yyyy-MM-dd HH:mm:ss Z — "2026-09-11 18:40:12 +0200".
    const auto number = [&](usize at, usize len) -> std::optional<i32> {
        if (at + len > text.size()) {
            return std::nullopt;
        }
        i32 v = 0;
        const auto [p, ec] = std::from_chars(text.data() + at, text.data() + at + len, v);
        if (ec != std::errc{} || p != text.data() + at + len) {
            return std::nullopt;
        }
        return v;
    };
    if (text.size() != 25 || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
        text[13] != ':' || text[16] != ':' || text[19] != ' ' ||
        (text[20] != '+' && text[20] != '-')) {
        return std::nullopt;
    }
    const auto y = number(0, 4), mo = number(5, 2), d = number(8, 2), h = number(11, 2),
               mi = number(14, 2), s = number(17, 2), oh = number(21, 2), om = number(23, 2);
    if (!y || !mo || !d || !h || !mi || !s || !oh || !om || *mo < 1 || *mo > 12 || *d < 1 ||
        *d > 31 || *h > 23 || *mi > 59 || *s > 60) {
        return std::nullopt;
    }
    const i64 offset = (static_cast<i64>(*oh) * 3600 + static_cast<i64>(*om) * 60) *
                       (text[20] == '-' ? -1 : 1);
    return days_from_civil(*y, *mo, *d) * 86400 + static_cast<i64>(*h) * 3600 +
           static_cast<i64>(*mi) * 60 + *s - offset;
}

std::string format_zone_date(i64 epoch_seconds, i32 utc_offset_seconds, std::string_view zone) {
    // "yyyy-MM-dd 'at' HH:mm:ss z" — measured: "2099-01-02 at 04:04:05 CET".
    const std::string stamp = ymd_hms(civil_from_epoch(epoch_seconds + utc_offset_seconds));
    return stamp.substr(0, 10) + " at " + stamp.substr(11) + " " + std::string{zone};
}

std::string format_java_date(i64 epoch_seconds, i32 utc_offset_seconds, std::string_view zone) {
    static constexpr std::array<const char*, 7>  kDays{"Sun", "Mon", "Tue", "Wed",
                                                       "Thu", "Fri", "Sat"};
    static constexpr std::array<const char*, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const Civil          c = civil_from_epoch(epoch_seconds + utc_offset_seconds);
    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%s %s %02d %02d:%02d:%02d %.*s %lld",
                  kDays[static_cast<usize>(c.weekday)], kMonths[static_cast<usize>(c.month - 1)],
                  c.day, c.hour, c.minute, c.second, static_cast<int>(zone.size()), zone.data(),
                  static_cast<long long>(c.year));
    return buf.data();
}

LocalZone local_zone_at(i64 epoch_seconds) {
    LocalZone   out;
    std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm     local{};
#if defined(_WIN32)
    if (localtime_s(&local, &t) != 0) {
        return out;
    }
    std::tm utc{};
    gmtime_s(&utc, &t);
    const i64 diff = static_cast<i64>(_mkgmtime(&local)) - static_cast<i64>(t);
    out.offset_seconds = static_cast<i32>(diff);
#else
    if (localtime_r(&t, &local) == nullptr) {
        return out;
    }
    out.offset_seconds = static_cast<i32>(local.tm_gmtoff);
#endif
    std::array<char, 16> zone{};
    if (std::strftime(zone.data(), zone.size(), "%Z", &local) > 0) {
        out.abbreviation = zone.data();
    }
    return out;
}

}  // namespace ov::server::admin
