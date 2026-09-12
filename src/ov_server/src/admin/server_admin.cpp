#include "server_admin.hpp"

#include "../commands/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace ov::server::admin {

ServerAdmin::ServerAdmin(AdminConfig config)
    : clock_{config.clock},
      players_{BanList::Kind::Players,
               config.directory.empty() ? std::filesystem::path{}
                                        : config.directory / "banned-players.json",
               config.clock},
      ips_{BanList::Kind::Ips,
           config.directory.empty() ? std::filesystem::path{} : config.directory / "banned-ips.json",
           config.clock},
      whitelist_{config.directory.empty() ? std::filesystem::path{}
                                          : config.directory / "whitelist.json"},
      white_list_{config.white_list},
      enforce_whitelist_{config.enforce_whitelist},
      max_players_{config.max_players} {}

std::vector<std::string> ServerAdmin::load() {
    const std::scoped_lock   lock{mutex_};
    std::vector<std::string> problems;
    if (!players_.load()) {
        problems.emplace_back("banned-players.json could not be read; no player is banned");
    }
    if (!ips_.load()) {
        problems.emplace_back("banned-ips.json could not be read; no address is banned");
    }
    if (!whitelist_.load()) {
        problems.emplace_back("whitelist.json could not be read; it is empty");
    }
    return problems;
}

void ServerAdmin::set_ops(std::vector<OpPass> ops) {
    const std::scoped_lock lock{mutex_};
    ops_ = std::move(ops);
}

bool ServerAdmin::whitelisted_locked(const net::Uuid& uuid) const {
    return !white_list_ || whitelist_.contains(uuid) ||
           std::ranges::any_of(ops_, [&](const OpPass& op) { return op.uuid == uuid; });
}

bool ServerAdmin::is_whitelisted(const net::Uuid& uuid) {
    const std::scoped_lock lock{mutex_};
    return whitelisted_locked(uuid);
}

std::string ServerAdmin::ban_refusal(const BanEntry& entry, bool ip) const {
    cmd::Text text = cmd::Text::translatable(
        ip ? "multiplayer.disconnect.banned_ip.reason" : "multiplayer.disconnect.banned.reason",
        {cmd::Text::raw(entry.reason)});
    if (entry.expires) {
        const LocalZone zone = clock_.zone(*entry.expires);
        text.append(cmd::Text::translatable(
            ip ? "multiplayer.disconnect.banned_ip.expiration"
               : "multiplayer.disconnect.banned.expiration",
            {cmd::Text::raw(format_zone_date(*entry.expires, zone.offset_seconds, zone.abbreviation))}));
    }
    return cmd::to_json(text);
}

std::optional<std::string> ServerAdmin::login_refusal(const net::Uuid& uuid,
                                                      std::string_view address,
                                                      i32              online_players) {
    const std::scoped_lock lock{mutex_};
    if (const BanEntry* ban = players_.get(uuid.to_string())) {
        return ban_refusal(*ban, false);
    }
    if (!whitelisted_locked(uuid)) {
        return cmd::to_json(cmd::Text::translatable("multiplayer.disconnect.not_whitelisted"));
    }
    if (const BanEntry* ban = ips_.get(address)) {
        return ban_refusal(*ban, true);
    }
    const bool bypass = std::ranges::any_of(
        ops_, [&](const OpPass& op) { return op.uuid == uuid && op.bypasses_player_limit; });
    if (online_players >= max_players_ && !bypass) {
        return cmd::to_json(cmd::Text::translatable("multiplayer.disconnect.server_full"));
    }
    return std::nullopt;
}

std::string address_without_port(std::string_view peer) {
    if (!peer.empty() && peer.front() == '[') {
        const auto close = peer.find(']');
        return std::string{peer.substr(1, close == std::string_view::npos ? peer.size() - 1
                                                                          : close - 1)};
    }
    const auto colon = peer.rfind(':');
    // One colon: IPv4 with a port. Several: a bare IPv6 address.
    if (colon != std::string_view::npos && peer.find(':') == colon) {
        return std::string{peer.substr(0, colon)};
    }
    return std::string{peer};
}

bool is_ip_address(std::string_view text) {
    // IPv4: four decimal parts 0..255, no sign, at most three digits.
    const auto v4 = [](std::string_view s) {
        i32 parts = 0;
        while (true) {
            const auto dot  = s.find('.');
            const auto part = s.substr(0, dot);
            if (part.empty() || part.size() > 3) {
                return false;
            }
            i32 value = 0;
            const auto [p, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
            if (ec != std::errc{} || p != part.data() + part.size() || value > 255) {
                return false;
            }
            ++parts;
            if (dot == std::string_view::npos) {
                break;
            }
            s.remove_prefix(dot + 1);
        }
        return parts == 4;
    };
    if (text.find(':') == std::string_view::npos) {
        return v4(text);
    }
    // IPv6: hex groups of at most four, one "::" at most, an IPv4 tail allowed.
    std::string_view s = text;
    if (const auto percent = s.find('%'); percent != std::string_view::npos) {
        return false;
    }
    i32  groups      = 0;
    bool compressed  = false;
    usize i          = 0;
    if (s.starts_with("::")) {
        compressed = true;
        i          = 2;
        if (s.size() == 2) {
            return true;
        }
    } else if (s.starts_with(":")) {
        return false;
    }
    while (i < s.size()) {
        const auto next  = s.find(':', i);
        const auto group = s.substr(i, next == std::string_view::npos ? s.npos : next - i);
        if (group.find('.') != std::string_view::npos) {
            if (next != std::string_view::npos || !v4(group)) {
                return false;
            }
            groups += 2;
            break;
        }
        if (group.empty() || group.size() > 4 ||
            !std::ranges::all_of(group, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            })) {
            return false;
        }
        ++groups;
        if (next == std::string_view::npos) {
            break;
        }
        if (next + 1 < s.size() && s[next + 1] == ':') {
            if (compressed) {
                return false;
            }
            compressed = true;
            i          = next + 2;
            if (i == s.size()) {
                break;
            }
            continue;
        }
        if (next + 1 == s.size()) {
            return false;
        }
        i = next + 1;
    }
    return compressed ? groups < 8 : groups == 8;
}

}  // namespace ov::server::admin
