// The dedicated server's own commands: bans, the whitelist, saving, the idle
// timeout, profiling.
//
// Registered by `register_commands()` at their places in vanilla's order (the
// order of its Commands packet: `debug` after `datapack`; `ban-ip`, `banlist`,
// `ban` before `deop`; `pardon`, `pardon-ip` after `op`; `save-off`,
// `save-on`, `setidletimeout` between `save-all` and `stop`; `whitelist`
// last). Each tree is the jar's, node for node; each answer is the jar's,
// byte for byte, as `scripts/capture_admin.py` recorded it
// (docs/provenance/serveur-dedie.md).
//
// The lists themselves are `admin::ServerAdmin`'s, shared with the network
// thread that checks every login, so every touch goes through its lock.
#include "service.hpp"

#include "names.hpp"

#include "../admin/server_admin.hpp"

#include <chrono>
#include <cstdio>

namespace ov::server::cmd {
namespace {

[[nodiscard]] CommandError error(std::string key, std::vector<Text> with = {}) {
    return CommandError::plain(Text::translatable(std::move(key), std::move(with)));
}

[[nodiscard]] Text raw(i64 value) { return Text::raw(std::to_string(value)); }

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
    return out;
}

/// SharedSuggestionProvider.suggest: the candidates that start with what has
/// been typed, compared in lower case.
void suggest_matching(SuggestionsBuilder& builder, const std::vector<std::string>& candidates) {
    const std::string typed = builder.remaining_lower();
    for (const std::string& candidate : candidates) {
        if (lower(candidate).starts_with(typed)) {
            builder.suggest(candidate);
        }
    }
}

[[nodiscard]] i64 steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string fixed2(double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", value);
    return buf;
}

}  // namespace

// ── Profiles ────────────────────────────────────────────────────────────────

Parsed<std::vector<std::pair<std::string, net::Uuid>>> CommandService::game_profiles(
    const CommandContext& ctx, std::string_view name) {
    // A selector's players, or a name: the online player's own profile when
    // one matches, else the name's offline uuid — what an offline server
    // resolves an unknown name to.
    const GameProfileArg& arg = *ctx.find<GameProfileArg>(name);
    std::vector<std::pair<std::string, net::Uuid>> out;
    if (arg.selector) {
        auto found = find_entities(
            *arg.selector, ctx.source(), world_snapshot_,
            [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); },
            &env_);
        if (!found) {
            return std::unexpected{found.error()};
        }
        for (const EntityInfo* e : *found) {
            if (e->player) {
                out.emplace_back(e->name, e->uuid);
            }
        }
        if (out.empty()) {
            return std::unexpected{error("argument.entity.notfound.player")};
        }
        return out;
    }
    for (const PlayerRef& p : players_) {
        if (lower(p.name) == lower(arg.name)) {
            out.emplace_back(std::string{p.name}, p.uuid);
            return out;
        }
    }
    out.emplace_back(arg.name, net::Uuid::offline_player(arg.name));
    return out;
}

void CommandService::sync_admin_ops() {
    if (config_.admin == nullptr) {
        return;
    }
    std::vector<admin::OpPass> passes;
    for (const OpEntry& e : ops_.entries()) {
        passes.push_back(admin::OpPass{e.uuid, e.bypasses_player_limit});
    }
    config_.admin->set_ops(std::move(passes));
}

// ── debug ───────────────────────────────────────────────────────────────────

void CommandService::register_debug(const TopFn& top) {
    Dispatcher& d    = dispatcher_;
    const u32   node = top("debug", kPermissionAdmin, {});
    d.literal(node, "start", [this](const CommandContext& ctx) -> Parsed<i32> {
        if (debug_started_) {
            return std::unexpected{error("commands.debug.alreadyRunning")};
        }
        debug_started_ = std::pair{steady_ms(), host_->tick_count ? host_->tick_count() : 0};
        success(ctx.source(), Text::translatable("commands.debug.started"), true);
        return 0;
    });
    d.literal(node, "stop", [this](const CommandContext& ctx) -> Parsed<i32> {
        if (!debug_started_) {
            return std::unexpected{error("commands.debug.notRunning")};
        }
        const double seconds =
            static_cast<double>(steady_ms() - debug_started_->first) / 1000.0;
        const i64 ticks =
            (host_->tick_count ? host_->tick_count() : debug_started_->second) - debug_started_->second;
        debug_started_.reset();
        const double rate = seconds > 0.0 ? static_cast<double>(ticks) / seconds : 0.0;
        success(ctx.source(),
                Text::translatable("commands.debug.stopped",
                                   {Text::raw(fixed2(seconds)), raw(ticks), Text::raw(fixed2(rate))}),
                true);
        return static_cast<i32>(rate);
    });
    // `debug function <name>` traces a function, and functions are not here
    // yet: its node is left out rather than answered with nothing.
}

// ── ban-ip, banlist, ban ────────────────────────────────────────────────────

void CommandService::register_bans(const TopFn& top) {
    if (config_.admin == nullptr) {
        return;  // an integrated server has no ban lists, as vanilla's has none
    }
    Dispatcher& d = dispatcher_;

    // ── ban-ip ──
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            const std::string& target = *ctx.find<std::string>("target");
            std::string        ip;
            if (admin::is_ip_address(target)) {
                ip = target;
            } else {
                for (const PlayerRef& p : players_) {
                    if (lower(p.name) == lower(target)) {
                        ip = p.address;
                    }
                }
                if (ip.empty()) {
                    return std::unexpected{error("commands.banip.invalid")};
                }
            }
            const MessageArg* message = ctx.find<MessageArg>("reason");
            const std::optional<std::string> reason =
                message != nullptr ? std::optional{resolve(*message, ctx.source())} : std::nullopt;
            const bool added = config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                if (lists.ips.contains(ip)) {
                    return false;
                }
                admin::BanEntry entry;
                entry.key     = ip;
                entry.created = lists.ips.clock().now();
                entry.source  = ctx.source().name;
                entry.reason  = reason.value_or(std::string{admin::kDefaultBanReason});
                lists.ips.add(std::move(entry));
                (void)lists.ips.save();
                return true;
            });
            if (!added) {
                return std::unexpected{error("commands.banip.failed")};
            }
            std::vector<PlayerRef*> affected;
            for (PlayerRef& p : players_) {
                if (p.address == ip) {
                    affected.push_back(&p);
                }
            }
            success(ctx.source(),
                    Text::translatable("commands.banip.success",
                                       {Text::raw(ip), Text::literal(reason.value_or(
                                                           std::string{admin::kDefaultBanReason}))}),
                    true);
            if (!affected.empty()) {
                Text names = Text::literal("");
                for (usize i = 0; i < affected.size(); ++i) {
                    if (i > 0) {
                        names.append(Text::literal(", ").color("gray"));
                    }
                    names.append(player_display_name(affected[i]->name, affected[i]->uuid));
                }
                if (names.extra.size() == 1) {
                    names = std::move(names.extra.front());
                }
                success(ctx.source(),
                        Text::translatable("commands.banip.info",
                                           {raw(static_cast<i64>(affected.size())), std::move(names)}),
                        true);
            }
            const std::string kick =
                to_json(Text::translatable("multiplayer.disconnect.ip_banned"));
            for (PlayerRef* p : affected) {
                host_->kick(p->entity_id, kick);
            }
            return static_cast<i32>(affected.size());
        };
        const u32 node   = top("ban-ip", kPermissionAdmin, {});
        const u32 target = d.argument(node, "target", ArgumentType::word(), run);
        d.argument(target, "reason", ArgumentType::message(), run);
    }

    // ── banlist ──
    {
        enum class Which : u8 { All, Ips, Players };
        const auto list = [this](Which which) {
            return [this, which](const CommandContext& ctx) -> Parsed<i32> {
                struct Line {
                    Text        name;
                    std::string source;
                    std::string reason;
                };
                std::vector<Line> lines;
                config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                    const auto take = [&](admin::BanList& from, bool players) {
                        (void)from.get("");  // drop what has expired, as vanilla's getter does
                        std::vector<std::string> keys;
                        for (const admin::BanEntry& e : from.entries()) {
                            keys.push_back(e.key);
                        }
                        for (const usize i : admin::saved_order(keys, from.entries().size())) {
                            const admin::BanEntry& e = from.entries()[i];
                            lines.push_back(Line{Text::literal(players ? e.name : e.key), e.source,
                                                 e.reason});
                        }
                    };
                    if (which != Which::Ips) {
                        take(lists.players, true);
                    }
                    if (which != Which::Players) {
                        take(lists.ips, false);
                    }
                });
                if (lines.empty()) {
                    success(ctx.source(), Text::translatable("commands.banlist.none"), false);
                    return 0;
                }
                success(ctx.source(),
                        Text::translatable("commands.banlist.list",
                                           {raw(static_cast<i64>(lines.size()))}),
                        false);
                for (Line& line : lines) {
                    success(ctx.source(),
                            Text::translatable("commands.banlist.entry",
                                               {std::move(line.name), Text::raw(line.source),
                                                Text::literal(line.reason)}),
                            false);
                }
                return static_cast<i32>(lines.size());
            };
        };
        const u32 node = top("banlist", kPermissionAdmin, list(Which::All));
        d.literal(node, "ips", list(Which::Ips));
        d.literal(node, "players", list(Which::Players));
    }

    // ── ban ──
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            auto found = game_profiles(ctx, "targets");
            if (!found) {
                return std::unexpected{found.error()};
            }
            const MessageArg* message = ctx.find<MessageArg>("reason");
            const std::optional<std::string> reason =
                message != nullptr ? std::optional{resolve(*message, ctx.source())} : std::nullopt;
            i32 banned = 0;
            for (const auto& [name, uuid] : *found) {
                const bool added = config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                    if (lists.players.contains(uuid.to_string())) {
                        return false;
                    }
                    admin::BanEntry entry;
                    entry.key     = uuid.to_string();
                    entry.name    = name;
                    entry.created = lists.players.clock().now();
                    entry.source  = ctx.source().name;
                    entry.reason  = reason.value_or(std::string{admin::kDefaultBanReason});
                    lists.players.add(std::move(entry));
                    (void)lists.players.save();
                    return true;
                });
                if (!added) {
                    continue;
                }
                ++banned;
                success(ctx.source(),
                        Text::translatable(
                            "commands.ban.success",
                            {Text::literal(name),
                             Text::literal(reason.value_or(std::string{admin::kDefaultBanReason}))}),
                        true);
                for (PlayerRef& p : players_) {
                    if (p.uuid == uuid) {
                        host_->kick(p.entity_id,
                                    to_json(Text::translatable("multiplayer.disconnect.banned")));
                    }
                }
            }
            if (banned == 0) {
                return std::unexpected{error("commands.ban.failed")};
            }
            return banned;
        };
        const u32 node    = top("ban", kPermissionAdmin, {});
        const u32 targets = d.argument(node, "targets", ArgumentType::game_profile(), run);
        d.argument(targets, "reason", ArgumentType::message(), run);
    }
}

// ── pardon, pardon-ip ───────────────────────────────────────────────────────

void CommandService::register_pardons(const TopFn& top) {
    if (config_.admin == nullptr) {
        return;
    }
    Dispatcher&         d     = dispatcher_;
    admin::ServerAdmin* admin = config_.admin;

    {
        const u32 node    = top("pardon", kPermissionAdmin, {});
        const u32 targets = d.argument(
            node, "targets", ArgumentType::game_profile(),
            [this](const CommandContext& ctx) -> Parsed<i32> {
                auto found = game_profiles(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                i32 pardoned = 0;
                for (const auto& [name, uuid] : *found) {
                    const bool removed = config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                        if (!lists.players.contains(uuid.to_string())) {
                            return false;
                        }
                        (void)lists.players.remove(uuid.to_string());
                        (void)lists.players.save();
                        return true;
                    });
                    if (!removed) {
                        continue;
                    }
                    ++pardoned;
                    success(ctx.source(),
                            Text::translatable("commands.pardon.success", {Text::literal(name)}), true);
                }
                if (pardoned == 0) {
                    return std::unexpected{error("commands.pardon.failed")};
                }
                return pardoned;
            });
        // The banned names — read on the network thread, under the lists' lock.
        d.suggests(targets,
                   [admin](const CommandContext&, SuggestionsBuilder& builder) {
                       std::vector<std::string> names;
                       admin->with([&](admin::ServerAdmin::Lists& lists) {
                           for (const admin::BanEntry& e : lists.players.entries()) {
                               names.push_back(e.name);
                           }
                       });
                       suggest_matching(builder, names);
                   },
                   "minecraft:ask_server");
    }
    {
        const u32 node   = top("pardon-ip", kPermissionAdmin, {});
        const u32 target = d.argument(
            node, "target", ArgumentType::word(), [this](const CommandContext& ctx) -> Parsed<i32> {
                const std::string& ip = *ctx.find<std::string>("target");
                if (!admin::is_ip_address(ip)) {
                    return std::unexpected{error("commands.pardonip.invalid")};
                }
                const bool removed = config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                    if (!lists.ips.contains(ip)) {
                        return false;
                    }
                    (void)lists.ips.remove(ip);
                    (void)lists.ips.save();
                    return true;
                });
                if (!removed) {
                    return std::unexpected{error("commands.pardonip.failed")};
                }
                success(ctx.source(), Text::translatable("commands.pardonip.success", {Text::raw(ip)}),
                        true);
                return 1;
            });
        d.suggests(target,
                   [admin](const CommandContext&, SuggestionsBuilder& builder) {
                       std::vector<std::string> ips;
                       admin->with([&](admin::ServerAdmin::Lists& lists) {
                           for (const admin::BanEntry& e : lists.ips.entries()) {
                               ips.push_back(e.key);
                           }
                       });
                       suggest_matching(builder, ips);
                   },
                   "minecraft:ask_server");
    }
}

// ── save-off, save-on, setidletimeout ───────────────────────────────────────

void CommandService::register_save_switches(const TopFn& top) {
    if (config_.admin == nullptr) {
        return;
    }
    Dispatcher& d = dispatcher_;
    top("save-off", kPermissionOwner, [this](const CommandContext& ctx) -> Parsed<i32> {
        if (!autosave_.exchange(false)) {
            return std::unexpected{error("commands.save.alreadyOff")};
        }
        success(ctx.source(), Text::translatable("commands.save.disabled"), true);
        return 1;
    });
    top("save-on", kPermissionOwner, [this](const CommandContext& ctx) -> Parsed<i32> {
        if (autosave_.exchange(true)) {
            return std::unexpected{error("commands.save.alreadyOn")};
        }
        success(ctx.source(), Text::translatable("commands.save.enabled"), true);
        return 1;
    });
    const u32 node = top("setidletimeout", kPermissionAdmin, {});
    d.argument(node, "minutes", ArgumentType::integer_at_least(0),
               [this](const CommandContext& ctx) -> Parsed<i32> {
                   const i32 minutes = *ctx.find<i32>("minutes");
                   set_idle_timeout(minutes);
                   if (host_->set_property) {
                       host_->set_property("player-idle-timeout", std::to_string(minutes));
                   }
                   success(ctx.source(),
                           Text::translatable("commands.setidletimeout.success", {raw(minutes)}),
                           true);
                   return minutes;
               });
}

// ── whitelist ───────────────────────────────────────────────────────────────

void CommandService::register_whitelist(const TopFn& top) {
    if (config_.admin == nullptr) {
        return;
    }
    Dispatcher&         d     = dispatcher_;
    admin::ServerAdmin* admin = config_.admin;

    // DedicatedServer.kickUnlistedPlayers: only with enforce-whitelist.
    const auto kick_unlisted = [this] {
        const bool enforce = config_.admin->with(
            [](admin::ServerAdmin::Lists& lists) { return lists.enforce_whitelist; });
        if (!enforce) {
            return;
        }
        const std::string reason =
            to_json(Text::translatable("multiplayer.disconnect.not_whitelisted"));
        for (PlayerRef& p : players_) {
            if (!config_.admin->is_whitelisted(p.uuid)) {
                host_->kick(p.entity_id, reason);
            }
        }
    };
    const auto set_enabled = [this, kick_unlisted](bool on) {
        return [this, kick_unlisted, on](const CommandContext& ctx) -> Parsed<i32> {
            const bool changed = config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                if (lists.white_list_enabled == on) {
                    return false;
                }
                lists.white_list_enabled = on;
                return true;
            });
            if (!changed) {
                return std::unexpected{
                    error(on ? "commands.whitelist.alreadyOn" : "commands.whitelist.alreadyOff")};
            }
            if (host_->set_property) {
                host_->set_property("white-list", on ? "true" : "false");
            }
            success(ctx.source(),
                    Text::translatable(on ? "commands.whitelist.enabled" : "commands.whitelist.disabled"),
                    true);
            if (on) {
                kick_unlisted();
            }
            return 1;
        };
    };
    const u32 node = top("whitelist", kPermissionAdmin, {});
    d.literal(node, "on", set_enabled(true));
    d.literal(node, "off", set_enabled(false));
    d.literal(node, "list", [this](const CommandContext& ctx) -> Parsed<i32> {
        const std::vector<std::string> names = config_.admin->with(
            [](admin::ServerAdmin::Lists& lists) { return lists.whitelist.names(); });
        if (names.empty()) {
            success(ctx.source(), Text::translatable("commands.whitelist.none"), false);
            return 0;
        }
        std::string joined;
        for (usize i = 0; i < names.size(); ++i) {
            joined += (i > 0 ? ", " : "") + names[i];
        }
        success(ctx.source(),
                Text::translatable("commands.whitelist.list",
                                   {raw(static_cast<i64>(names.size())), Text::raw(joined)}),
                false);
        return static_cast<i32>(names.size());
    });
    const auto change = [this, kick_unlisted](bool add) {
        return [this, kick_unlisted, add](const CommandContext& ctx) -> Parsed<i32> {
            auto found = game_profiles(ctx, "targets");
            if (!found) {
                return std::unexpected{found.error()};
            }
            i32 changed = 0;
            for (const auto& [name, uuid] : *found) {
                const bool done = config_.admin->with([&](admin::ServerAdmin::Lists& lists) {
                    const bool ok = add ? lists.whitelist.add(admin::WhiteEntry{uuid, name})
                                        : lists.whitelist.remove(uuid);
                    if (ok) {
                        (void)lists.whitelist.save();
                    }
                    return ok;
                });
                if (!done) {
                    continue;
                }
                ++changed;
                success(ctx.source(),
                        Text::translatable(add ? "commands.whitelist.add.success"
                                               : "commands.whitelist.remove.success",
                                           {Text::literal(name)}),
                        true);
            }
            if (changed == 0) {
                return std::unexpected{error(add ? "commands.whitelist.add.failed"
                                                 : "commands.whitelist.remove.failed")};
            }
            if (!add) {
                kick_unlisted();
            }
            return changed;
        };
    };
    const u32 add_node = d.literal(node, "add");
    // Online names, as `op` offers them (docs/provenance/commandes.md § 3).
    d.suggests(d.argument(add_node, "targets", ArgumentType::game_profile(), change(true)), {},
               "minecraft:ask_server");
    const u32 remove_node = d.literal(node, "remove");
    d.suggests(d.argument(remove_node, "targets", ArgumentType::game_profile(), change(false)),
               [admin](const CommandContext&, SuggestionsBuilder& builder) {
                   const std::vector<std::string> names = admin->with(
                       [](admin::ServerAdmin::Lists& lists) { return lists.whitelist.names(); });
                   suggest_matching(builder, names);
               },
               "minecraft:ask_server");
    d.literal(node, "reload", [this, kick_unlisted](const CommandContext& ctx) -> Parsed<i32> {
        config_.admin->with([](admin::ServerAdmin::Lists& lists) { (void)lists.whitelist.load(); });
        success(ctx.source(), Text::translatable("commands.whitelist.reloaded"), true);
        kick_unlisted();
        return 1;
    });
}

// ── publish ─────────────────────────────────────────────────────────────────

void CommandService::register_publish(const TopFn& top) {
    // Singleplayer's "Open to LAN" as a command: vanilla registers it on an
    // integrated server only (a dedicated one answers "Unknown or incomplete
    // command"). The tree is the jar's — `publish [allowCommands] [gamemode]
    // [port]` — and the answer is a refusal by name: this integrated server
    // already listens on a socket of its own, but the LAN announcement and
    // the rights it would give the guests are not written.
    if (!config_.integrated) {
        return;
    }
    Dispatcher&    d   = dispatcher_;
    const Executor run = [](const CommandContext&) -> Parsed<i32> {
        return std::unexpected{CommandError::plain(
            Text::literal("Ondes VOXEL does not open a world to LAN yet"))};
    };
    const u32 node     = top("publish", kPermissionOwner, run);
    const u32 allow    = d.argument(node, "allowCommands", ArgumentType::boolean(), run);
    const u32 gamemode = d.argument(allow, "gamemode", ArgumentType::game_mode(), run);
    d.argument(gamemode, "port", ArgumentType::integer_between(0, 65535), run);
}

}  // namespace ov::server::cmd
