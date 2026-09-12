#include "server_properties.hpp"

#include "ov/io/file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>

namespace ov::server::admin {
namespace {

// 1.20.1's keys and defaults, in the order its dedicated server reads them.
// The set and the defaults are the file the real server wrote on its first
// start (scripts/capture_admin.py properties); the order is only used to
// break ties between keys that share a bucket of the JDK map.
// (log-ips and resource-pack-id came later: the 1.20.1 jar writes neither.)
constexpr std::array<PropertyDefault, 56> kDefaults{{
    {"online-mode", "true"},
    {"prevent-proxy-connections", "false"},
    {"server-ip", ""},
    {"spawn-animals", "true"},
    {"spawn-npcs", "true"},
    {"pvp", "true"},
    {"allow-flight", "false"},
    {"motd", "A Minecraft Server"},
    {"force-gamemode", "false"},
    {"enforce-whitelist", "false"},
    {"difficulty", "easy"},
    {"gamemode", "survival"},
    {"level-name", "world"},
    {"server-port", "25565"},
    {"enable-query", "false"},
    {"query.port", "25565"},
    {"enable-rcon", "false"},
    {"rcon.port", "25575"},
    {"rcon.password", ""},
    {"hardcore", "false"},
    {"allow-nether", "true"},
    {"spawn-monsters", "true"},
    {"use-native-transport", "true"},
    {"enable-command-block", "false"},
    {"spawn-protection", "16"},
    {"op-permission-level", "4"},
    {"function-permission-level", "2"},
    {"max-tick-time", "60000"},
    {"max-chained-neighbor-updates", "1000000"},
    {"rate-limit", "0"},
    {"view-distance", "10"},
    {"simulation-distance", "10"},
    {"max-players", "20"},
    {"network-compression-threshold", "256"},
    {"broadcast-rcon-to-ops", "true"},
    {"broadcast-console-to-ops", "true"},
    {"max-world-size", "29999984"},
    {"sync-chunk-writes", "true"},
    {"enable-jmx-monitoring", "false"},
    {"enable-status", "true"},
    {"hide-online-players", "false"},
    {"entity-broadcast-range-percentage", "100"},
    {"text-filtering-config", ""},
    {"player-idle-timeout", "0"},
    {"white-list", "false"},
    {"enforce-secure-profile", "true"},
    {"level-seed", ""},
    {"generator-settings", "{}"},
    {"level-type", "minecraft\\:normal"},
    {"generate-structures", "true"},
    {"initial-enabled-packs", "vanilla"},
    {"initial-disabled-packs", ""},
    // The server resource pack is read last, after the world's settings —
    // the order that puts every line of the jar's file in place (57/57,
    // scripts/jdk_order_oracle.java against the captured file).
    {"resource-pack", ""},
    {"require-resource-pack", "false"},
    {"resource-pack-prompt", ""},
    {"resource-pack-sha1", ""},
}};

constexpr std::array<std::string_view, 4> kGameModes{"survival", "creative", "adventure",
                                                     "spectator"};
constexpr std::array<std::string_view, 4> kDifficulties{"peaceful", "easy", "normal", "hard"};

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\f';
}

void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// loadConvert: backslash escapes, `\uXXXX` as UTF-16 (surrogate pairs
/// joined).
[[nodiscard]] std::string unescape(std::string_view in) {
    std::string out;
    u32         high = 0;  // a pending high surrogate
    const auto  put  = [&](u32 unit) {
        if (unit >= 0xD800 && unit < 0xDC00) {
            if (high != 0) {
                append_utf8(out, 0xFFFD);
            }
            high = unit;
            return;
        }
        if (unit >= 0xDC00 && unit < 0xE000 && high != 0) {
            append_utf8(out, 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00));
            high = 0;
            return;
        }
        if (high != 0) {
            append_utf8(out, 0xFFFD);
            high = 0;
        }
        append_utf8(out, unit);
    };
    for (usize i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c != '\\' || i + 1 >= in.size()) {
            if (high != 0) {
                append_utf8(out, 0xFFFD);
                high = 0;
            }
            out.push_back(c);
            continue;
        }
        c = in[++i];
        if (c == 'u') {
            u32 unit = 0;
            usize k  = 0;
            for (; k < 4 && i + 1 < in.size(); ++k) {
                const char h = in[i + 1];
                u32        v = 0;
                if (h >= '0' && h <= '9') {
                    v = static_cast<u32>(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    v = static_cast<u32>(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                    v = static_cast<u32>(h - 'A' + 10);
                } else {
                    break;
                }
                unit = unit * 16 + v;
                ++i;
            }
            // Java throws on a malformed escape and the server starts from
            // defaults; keeping what could be read is kinder and says the same.
            put(unit);
            continue;
        }
        if (high != 0) {
            append_utf8(out, 0xFFFD);
            high = 0;
        }
        switch (c) {
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 'f': out.push_back('\f'); break;
        default: out.push_back(c); break;
        }
    }
    if (high != 0) {
        append_utf8(out, 0xFFFD);
    }
    return out;
}

[[nodiscard]] std::optional<i64> parse_java_long(std::string_view text) {
    // Long.parseLong: an optional sign, decimal digits, nothing else.
    if (text.empty()) {
        return std::nullopt;
    }
    std::string_view digits = text;
    if (digits.front() == '+') {
        digits.remove_prefix(1);
        if (digits.empty() || digits.front() == '-' || digits.front() == '+') {
            return std::nullopt;
        }
    }
    i64 value = 0;
    const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{} || p != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               const auto lx = static_cast<char>(x >= 'A' && x <= 'Z' ? x + 32 : x);
               const auto ly = static_cast<char>(y >= 'A' && y <= 'Z' ? y + 32 : y);
               return lx == ly;
           });
}

}  // namespace

std::span<const PropertyDefault> ServerProperties::vanilla_defaults() { return kDefaults; }

std::string_view game_mode_name(u8 mode) noexcept { return kGameModes[mode & 3U]; }
std::string_view difficulty_name(u8 difficulty) noexcept { return kDifficulties[difficulty & 3U]; }

std::vector<std::pair<std::string, std::string>> parse_properties(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> out;
    usize                                            pos = 0;
    while (pos < text.size()) {
        // One natural line.
        usize end = text.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos                   = end;
        const auto skip_break = [&] {
            if (pos < text.size() && text[pos] == '\r') {
                ++pos;
            }
            if (pos < text.size() && text[pos] == '\n') {
                ++pos;
            }
        };
        skip_break();
        usize lead = 0;
        while (lead < line.size() && is_space(line[lead])) {
            ++lead;
        }
        line.remove_prefix(lead);
        if (line.empty() || line.front() == '#' || line.front() == '!') {
            continue;
        }
        // A logical line continues while it ends in an odd run of backslashes.
        std::string logical{line};
        const auto  continues = [](std::string_view s) {
            usize n = 0;
            while (n < s.size() && s[s.size() - 1 - n] == '\\') {
                ++n;
            }
            return n % 2 == 1;
        };
        while (continues(logical) && pos <= text.size()) {
            logical.pop_back();
            if (pos >= text.size()) {
                break;
            }
            usize next = text.find_first_of("\r\n", pos);
            if (next == std::string_view::npos) {
                next = text.size();
            }
            std::string_view more = text.substr(pos, next - pos);
            pos                   = next;
            skip_break();
            while (!more.empty() && is_space(more.front())) {
                more.remove_prefix(1);
            }
            logical.append(more);
        }
        // Key: up to the first unescaped '=', ':' or blank.
        usize key_end   = 0;
        bool  escaped   = false;
        bool  separator = false;
        for (; key_end < logical.size(); ++key_end) {
            const char c = logical[key_end];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '=' || c == ':') {
                separator = true;
                break;
            }
            if (is_space(c)) {
                break;
            }
        }
        usize value_start = key_end + (separator ? usize{1} : usize{0});
        while (value_start < logical.size() && is_space(logical[value_start])) {
            ++value_start;
        }
        if (!separator && value_start < logical.size() &&
            (logical[value_start] == '=' || logical[value_start] == ':')) {
            ++value_start;
            while (value_start < logical.size() && is_space(logical[value_start])) {
                ++value_start;
            }
        }
        std::string key   = unescape(std::string_view{logical}.substr(0, key_end));
        std::string value = unescape(std::string_view{logical}.substr(
            std::min(value_start, logical.size())));
        // A repeated key: the last one wins, in the first one's place.
        const auto it = std::ranges::find(out, key, &std::pair<std::string, std::string>::first);
        if (it != out.end()) {
            it->second = std::move(value);
        } else {
            out.emplace_back(std::move(key), std::move(value));
        }
    }
    return out;
}

std::string escape_property(std::string_view text, bool is_key, bool unicode) {
    std::string out;
    usize       index = 0;
    for (usize i = 0; i < text.size(); ++i, ++index) {
        const auto c = static_cast<u8>(text[i]);
        switch (c) {
        case ' ':
            if (index == 0 || is_key) {
                out += '\\';
            }
            out += ' ';
            continue;
        case '\t': out += "\\t"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\f': out += "\\f"; continue;
        case '=':
        case ':':
        case '#':
        case '!':
        case '\\':
            out += '\\';
            out += static_cast<char>(c);
            continue;
        default: break;
        }
        if ((c >= 0x20 && c <= 0x7E) || !unicode) {
            // The Writer form of store() — the one the 1.20.1 jar uses, measured:
            // it writes "motd=Café" — escapes only the characters above and
            // writes every other one as it is, UTF-8 included.
            out += static_cast<char>(c);
            continue;
        }
        // \uXXXX per UTF-16 unit, upper-case hex.
        u32   cp  = c;
        usize len = 1;
        if (c >= 0xF0 && i + 3 < text.size()) {
            cp  = ((c & 0x07U) << 18) | ((static_cast<u8>(text[i + 1]) & 0x3FU) << 12) |
                 ((static_cast<u8>(text[i + 2]) & 0x3FU) << 6) | (static_cast<u8>(text[i + 3]) & 0x3FU);
            len = 4;
        } else if (c >= 0xE0 && i + 2 < text.size()) {
            cp  = ((c & 0x0FU) << 12) | ((static_cast<u8>(text[i + 1]) & 0x3FU) << 6) |
                 (static_cast<u8>(text[i + 2]) & 0x3FU);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            cp  = ((c & 0x1FU) << 6) | (static_cast<u8>(text[i + 1]) & 0x3FU);
            len = 2;
        }
        const auto unit = [&](u32 u) {
            std::array<char, 8> buf{};
            std::snprintf(buf.data(), buf.size(), "\\u%04X", u);
            out += buf.data();
        };
        if (cp >= 0x10000) {
            unit(0xD800 + ((cp - 0x10000) >> 10));
            unit(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            unit(cp);
        }
        i += len - 1;
    }
    return out;
}

std::string decode_properties_bytes(std::string_view bytes) {
    // Strict UTF-8 check; on any error the whole file is ISO-8859-1.
    const auto continuation_count = [](u8 lead) -> std::optional<usize> {
        if (lead < 0x80) {
            return usize{0};
        }
        if ((lead >> 5) == 0x06) {
            return usize{1};
        }
        if ((lead >> 4) == 0x0E) {
            return usize{2};
        }
        if ((lead >> 3) == 0x1E) {
            return usize{3};
        }
        return std::nullopt;
    };
    usize i     = 0;
    bool  valid = true;
    while (i < bytes.size() && valid) {
        const auto count = continuation_count(static_cast<u8>(bytes[i]));
        if (!count || i + *count >= bytes.size() + usize{*count == 0 ? 1U : 0U}) {
            valid = count.has_value() && *count == 0;
            break;
        }
        for (usize k = 1; k <= *count && valid; ++k) {
            valid = (static_cast<u8>(bytes[i + k]) & 0xC0) == 0x80;
        }
        i += *count + 1;
    }
    if (valid) {
        return std::string{bytes};
    }
    std::string out;
    for (const char ch : bytes) {
        append_utf8(out, static_cast<u8>(ch));
    }
    return out;
}

ServerProperties ServerProperties::from_text(std::string_view text) {
    ServerProperties out;
    out.entries_   = parse_properties(text);
    out.from_file_ = out.entries_.size();
    return out;
}

std::optional<std::string> ServerProperties::get(std::string_view key) const {
    const auto it = std::ranges::find(entries_, key, &std::pair<std::string, std::string>::first);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void ServerProperties::set(std::string_view key, std::string value) {
    const auto it = std::ranges::find(entries_, key, &std::pair<std::string, std::string>::first);
    if (it != entries_.end()) {
        it->second = std::move(value);
        return;
    }
    entries_.emplace_back(std::string{key}, std::move(value));
}

void ServerProperties::fill_defaults() {
    for (const PropertyDefault& d : kDefaults) {
        if (!get(d.key)) {
            // The defaults are written in the file's escaped form above only
            // where vanilla's value has a colon; store them unescaped.
            set(d.key, unescape(d.value));
        }
    }
}

std::vector<std::string> ServerProperties::write_order() const {
    // The Properties object: filled by load() in file order, then each
    // setting's get() puts its key — and stored as it is. Measured: the
    // 1.20.1 jar's file is the iteration of that very table, grown by the
    // puts to 128 buckets (53/57 lines at once, the rest settled by the
    // order the settings are read in); a putAll copy, presized to 256, put 1
    // line of 57 in place.
    ConcurrentHashMapOrder map{8};
    for (usize i = 0; i < from_file_ && i < entries_.size(); ++i) {
        map.put(entries_[i].first);
    }
    for (const PropertyDefault& d : kDefaults) {
        map.put(std::string{d.key});
    }
    for (const auto& [key, value] : entries_) {
        map.put(key);
    }
    return map.keys();
}

std::string ServerProperties::render(std::string_view date_line) const {
#if defined(_WIN32)
    constexpr std::string_view kEol = "\r\n";
#else
    constexpr std::string_view kEol = "\n";
#endif
    std::string out = "#Minecraft server properties";
    out += kEol;
    out += '#';
    out += date_line;
    out += kEol;
    // The Writer form of Properties.store, the jar's: backslash escapes only,
    // every other character as it is (measured: "motd=Café \: \= x").
    for (const std::string& key : write_order()) {
        out += escape_property(key, true, false);
        out += '=';
        out += escape_property(*get(key), false, false);
        out += kEol;
    }
    return out;
}

// ── DedicatedSettings ───────────────────────────────────────────────────────

DedicatedSettings DedicatedSettings::read(ServerProperties& p) {
    DedicatedSettings s;
    const auto text = [&](std::string_view key, std::string fallback) {
        auto value = p.get(key);
        if (!value) {
            p.set(key, fallback);
            return fallback;
        }
        return *value;
    };
    const auto number = [&](std::string_view key, i64 fallback, i64 lo, i64 hi) {
        const auto value  = p.get(key);
        const auto parsed = value ? parse_java_long(*value) : std::nullopt;
        i64        v      = parsed ? *parsed : fallback;
        v                 = std::clamp(v, lo, hi);
        p.set(key, std::to_string(v));
        return v;
    };
    const auto integer = [&](std::string_view key, i32 fallback) {
        return static_cast<i32>(number(key, fallback, INT32_MIN, INT32_MAX));
    };
    const auto flag = [&](std::string_view key, bool fallback) {
        const auto value = p.get(key);
        // Boolean.valueOf: "true" in any case is true, anything else false.
        const bool v = value ? equals_ignore_case(*value, "true") : fallback;
        p.set(key, v ? "true" : "false");
        return v;
    };
    // Game mode and difficulty: by name, or by number (an unknown number is
    // the first one), else the default.
    const auto named = [&](std::string_view key, const std::array<std::string_view, 4>& names,
                           u8 fallback) {
        const auto value = p.get(key);
        u8         v     = fallback;
        if (value) {
            bool found = false;
            for (u8 i = 0; i < 4; ++i) {
                if (*value == names[i]) {
                    v     = i;
                    found = true;
                }
            }
            if (!found) {
                if (const auto n = parse_java_long(*value)) {
                    v = *n >= 0 && *n < 4 ? static_cast<u8>(*n) : u8{0};
                }
            }
        }
        p.set(key, std::string{names[v]});
        return v;
    };

    s.online_mode               = flag("online-mode", true);
    s.prevent_proxy_connections = flag("prevent-proxy-connections", false);
    s.server_ip                 = text("server-ip", "");
    s.spawn_animals             = flag("spawn-animals", true);
    s.spawn_npcs                = flag("spawn-npcs", true);
    s.pvp                       = flag("pvp", true);
    s.allow_flight              = flag("allow-flight", false);
    s.motd                      = text("motd", "A Minecraft Server");
    s.force_gamemode            = flag("force-gamemode", false);
    s.enforce_whitelist         = flag("enforce-whitelist", false);
    s.difficulty                = named("difficulty", kDifficulties, 1);
    s.gamemode                  = named("gamemode", kGameModes, 0);
    s.level_name                = text("level-name", "world");
    s.server_port               = integer("server-port", 25565);
    s.enable_query              = flag("enable-query", false);
    s.query_port                = integer("query.port", 25565);
    s.enable_rcon               = flag("enable-rcon", false);
    s.rcon_port                 = integer("rcon.port", 25575);
    s.rcon_password             = text("rcon.password", "");
    s.hardcore                  = flag("hardcore", false);
    s.allow_nether              = flag("allow-nether", true);
    s.spawn_monsters            = flag("spawn-monsters", true);
    s.spawn_protection          = integer("spawn-protection", 16);
    s.op_permission_level       = integer("op-permission-level", 4);
    s.function_permission_level = static_cast<i32>(number("function-permission-level", 2, 1, 4));
    s.max_tick_time             = number("max-tick-time", 60000, INT64_MIN, INT64_MAX);
    s.rate_limit                = integer("rate-limit", 0);
    s.view_distance             = integer("view-distance", 10);
    s.simulation_distance       = integer("simulation-distance", 10);
    s.max_players               = integer("max-players", 20);
    s.network_compression_threshold = integer("network-compression-threshold", 256);
    s.broadcast_rcon_to_ops         = flag("broadcast-rcon-to-ops", true);
    s.broadcast_console_to_ops      = flag("broadcast-console-to-ops", true);
    s.max_world_size = static_cast<i32>(number("max-world-size", 29999984, 1, 29999984));
    s.sync_chunk_writes = flag("sync-chunk-writes", true);
    s.enable_status     = flag("enable-status", true);
    s.hide_online_players = flag("hide-online-players", false);
    s.entity_broadcast_range_percentage =
        static_cast<i32>(number("entity-broadcast-range-percentage", 100, 10, 1000));
    s.player_idle_timeout    = integer("player-idle-timeout", 0);
    s.white_list             = flag("white-list", false);
    s.enforce_secure_profile = flag("enforce-secure-profile", true);
    s.level_seed             = text("level-seed", "");
    s.level_type             = text("level-type", "minecraft:normal");
    s.generate_structures    = flag("generate-structures", true);
    // The rest are kept as written and filled in by default.
    p.fill_defaults();
    return s;
}

PropertiesFile load_properties_file(const std::filesystem::path& path,
                                    std::string_view             date_line) {
    PropertiesFile out;
    if (const auto bytes = io::read_file(path)) {
        out.existed    = true;
        out.properties = ServerProperties::from_text(decode_properties_bytes(
            std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()}));
    }
    out.settings = DedicatedSettings::read(out.properties);
    (void)save_properties_file(path, out.properties, date_line);
    return out;
}

bool save_properties_file(const std::filesystem::path& path, const ServerProperties& properties,
                          std::string_view date_line) {
    const std::string text = properties.render(date_line);
    return io::write_file_atomic(
               path, std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()})
        .has_value();
}

}  // namespace ov::server::admin
