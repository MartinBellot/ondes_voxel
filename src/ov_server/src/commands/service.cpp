#include "service.hpp"

#include "names.hpp"
#include "snbt.hpp"

#include "ov/gameplay/experience.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/survival.hpp"

#include <algorithm>
#include <cmath>

namespace ov::server::cmd {

CommandService::CommandService(ServiceConfig config)
    : config_{std::move(config)},
      env_{config_.blocks, config_.registries},
      dispatcher_{env_},
      ops_{config_.ops_file} {
    if (!config_.ops_file.empty() && !ops_.load()) {
        // Kept empty rather than half-read. The console says so on the first
        // command; a server that de-ops everyone quietly is the failure to avoid.
        ops_ = OpList{config_.ops_file};
    }
    if (!config_.lang_file.empty()) {
        lang_ = Lang::load(config_.lang_file);
    }
    register_commands();
    for (usize level = 0; level < graphs_.size(); ++level) {
        graphs_[level] = net::encode_commands(dispatcher_.wire_graph(static_cast<i32>(level)));
    }
}

const std::vector<u8>& CommandService::commands_packet(i32 permission) const {
    return graphs_[static_cast<usize>(std::clamp(permission, 0, 4))];
}

i32 join_permission(bool integrated, bool allow_commands, bool is_host,
                    std::optional<i32> ops_level) noexcept {
    if (!integrated) {
        return std::clamp(ops_level.value_or(0), 0, kPermissionOwner);
    }
    return is_host && allow_commands ? kPermissionOwner : 0;
}

i32 CommandService::permission_for(const PlayerRef& p) const {
    const bool is_host = !config_.host_player.empty() && p.name == config_.host_player;
    return join_permission(config_.integrated, allow_commands_, is_host,
                           config_.integrated ? std::nullopt : ops_.level_of(p.uuid));
}

void CommandService::load_world(const world::LevelSettings& settings) {
    allow_commands_ = settings.allow_commands;  // ── allow-commands ──
    world_.load(settings);
    default_mode_.store(world_.default_game_mode, std::memory_order_relaxed);
}

void CommandService::enqueue(i32 player_entity_id, std::string command, i64 timestamp, i64 salt) {
    const std::scoped_lock lock{queue_mutex_};
    queue_.push_back(Pending{player_entity_id, std::move(command), timestamp, salt});
}

void CommandService::enqueue_console(std::string command) {
    // A console line may carry the slash a player would type; vanilla strips
    // it, so a pasted `/time set day` works there too.
    if (!command.empty() && command.front() == '/') {
        command.erase(command.begin());
    }
    const std::scoped_lock lock{queue_mutex_};
    queue_.push_back(Pending{-1, std::move(command), 0, 0});
}

void CommandService::update_player_criteria(i32 entity_id, std::string_view name,
                                            const SurvivalSession& s) {
    criteria_names_[entity_id] = std::string{name};
    if (s.awaiting_respawn || s.health.dead) {
        if (dead_.insert(entity_id).second) {
            scoreboard_.on_death(name);
        }
    } else {
        dead_.erase(entity_id);
    }
    PlayerCriteria now;
    now.health = static_cast<i32>(std::ceil(s.health.health + s.health.absorption));
    now.food   = s.food.food;
    now.air    = s.health.air;
    now.armor  = static_cast<i32>(s.mitigation.armour);  // the worn pieces' points
    now.xp     = s.experience_total;
    now.level  = s.experience_level;
    scoreboard_.update_player(name, now);
}

void CommandService::enqueue_kill(std::string killer, std::string victim, bool victim_is_player) {
    const std::scoped_lock lock{queue_mutex_};
    kills_.push_back(Kill{std::move(killer), std::move(victim), victim_is_player});
}

void CommandService::enqueue_chat(i32 player_entity_id, std::string message, i64 timestamp, i64 salt) {
    const std::scoped_lock lock{queue_mutex_};
    queue_.push_back(Pending{player_entity_id, std::move(message), timestamp, salt, true});
}

net::SuggestionsResponse CommandService::suggest(const CommandSource& source, i32 transaction,
                                                 std::string_view             text,
                                                 std::span<const std::string> player_names) const {
    const MergedSuggestions merged = dispatcher_.suggest(text, source, player_names);
    net::SuggestionsResponse out;
    out.transaction = transaction;
    out.start       = static_cast<i32>(merged.start);
    out.length      = static_cast<i32>(merged.length);
    for (const auto& [match, tooltip] : merged.matches) {
        net::Suggestion suggestion;
        suggestion.text = match;
        if (tooltip) {
            suggestion.tooltip_json = to_json(*tooltip);
        }
        out.matches.push_back(std::move(suggestion));
    }
    return out;
}

std::vector<u8> CommandService::chat_packet(std::string_view name, const net::Uuid& uuid,
                                            std::string_view message, i64 timestamp, i64 salt,
                                            i32 chat_type, const std::optional<Text>& target) const {
    net::PlayerChat chat;
    chat.sender    = uuid;
    chat.index     = 0;
    chat.body      = std::string{message};
    chat.timestamp = timestamp;
    chat.salt      = salt;
    chat.chat_type = chat_type;
    chat.name_json = to_json(decorate(player_display_name(name, uuid)));  // ── scoreboard ──
    if (target) {
        chat.target_json = to_json(decorate(*target));
    }
    return net::encode_player_chat(chat);
}

std::vector<u8> CommandService::abilities_for(u8 game_mode) {
    // Measured flags: creative 0x0D (invulnerable, may fly, instant build),
    // spectator 0x07 (invulnerable, flying, may fly), the others none.
    const bool creative  = game_mode == 1;
    const bool spectator = game_mode == 3;
    return net::encode_player_abilities(creative || spectator, spectator, creative || spectator,
                                        creative, 0.05F, 0.1F);
}

void CommandService::tick_world() { world_.tick(outbox_); }

void CommandService::refresh_players() {
    players_.clear();
    if (host_ != nullptr && host_->for_each_player) {
        host_->for_each_player([&](PlayerRef& p) { players_.push_back(p); });
    }
}

PlayerRef* CommandService::player(i32 entity_id) {
    for (PlayerRef& p : players_) {
        if (p.entity_id == entity_id) {
            return &p;
        }
    }
    return nullptr;
}

CommandSource CommandService::source_for(const PlayerRef& p) const {
    CommandSource source;
    source.kind       = CommandSource::Kind::Player;
    source.entity_id  = p.entity_id;
    source.name       = std::string{p.name};
    source.uuid       = p.uuid;
    source.position   = Vec3d{*p.x, *p.y, *p.z};
    source.yaw        = *p.yaw;
    source.pitch      = *p.pitch;
    source.permission = *p.permission;
    return source;
}

void CommandService::welcome(PlayerRef& p) {
    const i32 level = permission_for(p);  // ── allow-commands ──
    *p.permission   = level;
    p.send(net::clientbound::kChangeDifficulty,
           net::encode_change_difficulty(world_.difficulty, world_.difficulty_locked));
    p.send(net::clientbound::kServerData,
           net::encode_server_data(to_json(Text::literal(config_.motd)), {}, false));
    p.send(net::clientbound::kEntityEvent,
           net::encode_entity_event(p.entity_id, static_cast<i8>(24 + level)));
    p.send(net::clientbound::kCommands, commands_packet(level));
    std::vector<Broadcast> weather;
    world_.join_packets(weather);
    for (const Broadcast& packet : weather) {
        p.send(packet.id, packet.payload);
    }
    if (world_.rules.flag("doImmediateRespawn")) {
        p.send(net::clientbound::kGameEvent, net::encode_game_event(11, 1.0F));
    }
    if (world_.rules.flag("reducedDebugInfo")) {
        p.send(net::clientbound::kEntityEvent, net::encode_entity_event(p.entity_id, 22));
    }
    p.send(net::clientbound::kUpdateTime, world_.update_time_payload());
    // ── scoreboard ── every team, and whatever objective a slot shows
    std::vector<ScoreboardPacket> board;
    scoreboard_.arrival_packets(board);
    for (const ScoreboardPacket& packet : board) {
        p.send(packet.id, packet.payload);
    }
}

void CommandService::set_permission(PlayerRef& p, i32 level) {
    *p.permission = level;
    p.send(net::clientbound::kEntityEvent,
           net::encode_entity_event(p.entity_id, static_cast<i8>(24 + level)));
    p.send(net::clientbound::kCommands, commands_packet(level));
}

void CommandService::set_game_mode(PlayerRef& p, u8 mode) {
    *p.game_mode = mode;
    // The order the capture shows: abilities, the tab list, the game event,
    // abilities again.
    const std::vector<u8> abilities = abilities_for(mode);
    p.send(net::clientbound::kPlayerAbilities, abilities);
    if (host_ != nullptr && host_->broadcast) {
        host_->broadcast(net::clientbound::kPlayerInfoUpdate,
                         net::encode_player_info_game_mode(p.uuid, mode));
    }
    p.send(net::clientbound::kGameEvent, net::encode_game_event(3, static_cast<f32>(mode)));
    p.send(net::clientbound::kPlayerAbilities, abilities);
}

std::vector<EntityInfo> CommandService::snapshot() const {
    std::vector<EntityInfo> out;
    for (const PlayerRef& p : players_) {
        EntityInfo info;
        info.id               = p.entity_id;
        info.player           = true;
        info.type             = "minecraft:player";
        info.uuid             = p.uuid;
        info.name             = std::string{p.name};
        info.position         = Vec3d{*p.x, *p.y, *p.z};
        info.yaw              = *p.yaw;
        info.pitch            = *p.pitch;
        info.game_mode        = *p.game_mode;
        info.experience_level = p.survival != nullptr ? p.survival->experience_level : 0;
        info.alive = p.survival == nullptr ||
                     (!p.survival->awaiting_respawn && !p.survival->health.dead);
        out.push_back(std::move(info));
    }
    if (host_ != nullptr && host_->entities) {
        host_->entities(out);
    }
    for (EntityInfo& e : out) {  // ── scoreboard ── what `team=` and `scores=` read
        const std::string holder = e.player ? e.name : e.uuid.to_string();
        const Team*       team   = scoreboard_.team_of(holder);
        e.team                   = team != nullptr ? team->name : std::string{};
        e.scores.clear();
        for (const auto& [objective, score] : scoreboard_.scores_of(holder)) {
            e.scores.emplace_back(objective->name, score.value);
        }
    }
    return out;
}

void CommandService::run(CommandHost& host) {
    host_ = &host;
    refresh_players();
    std::erase_if(welcomed_, [&](i32 id) { return player(id) == nullptr; });
    for (PlayerRef& p : players_) {
        if (!welcomed_.contains(p.entity_id)) {
            welcome(p);
            welcomed_.insert(p.entity_id);
        }
    }
    // ── scoreboard ── The kills other threads saw; each player's step into
    // death (deathCount counts the edge, not the ticks spent dead); and the
    // numbers the player criteria read — all of them on a player's first
    // tick, then only what changed, as the capture's newcomer shows.
    {
        std::vector<Kill> kills;
        {
            const std::scoped_lock lock{queue_mutex_};
            kills.swap(kills_);
        }
        for (const Kill& kill : kills) {
            scoreboard_.on_kill(kill.killer, kill.victim, kill.victim_is_player);
        }
        for (auto it = criteria_names_.begin(); it != criteria_names_.end();) {
            if (player(it->first) == nullptr) {
                scoreboard_.forget_player(it->second);
                dead_.erase(it->first);
                it = criteria_names_.erase(it);
            } else {
                ++it;
            }
        }
        // The player criteria are fed by update_player_criteria, right after
        // each player's survival tick — not here, a phase too early.
    }
    if (host.broadcast) {
        for (const Broadcast& packet : outbox_) {
            host.broadcast(packet.id, packet.payload);
        }
    }
    outbox_.clear();
    {
        const std::scoped_lock lock{queue_mutex_};
        running_.swap(queue_);
    }
    for (const Pending& pending : running_) {
        if (pending.chat) {  // ── scoreboard ──
            refresh_players();
            if (const PlayerRef* p = player(pending.player); p != nullptr && host.broadcast) {
                host.broadcast(net::clientbound::kPlayerChat,
                               chat_packet(p->name, p->uuid, pending.command, pending.timestamp,
                                           pending.salt, kChatTypeChat));
            }
            continue;
        }
        if (pending.player < 0) {
            (void)execute(CommandSource{}, pending.command, host);
            continue;
        }
        refresh_players();
        const PlayerRef* p = player(pending.player);
        if (p == nullptr) {
            continue;  // left before the tick came round
        }
        (void)execute(source_for(*p), pending.command, host, pending.timestamp, pending.salt);
    }
    running_.clear();
    flush_scoreboard();  // ── scoreboard ──
}

Parsed<i32> CommandService::execute(const CommandSource& source, std::string_view command,
                                    CommandHost& host, i64 timestamp, i64 salt) {
    host_      = &host;
    timestamp_ = timestamp;
    salt_      = salt;
    refresh_players();
    world_snapshot_ = snapshot();
    source_shown_   = decorate(source_name(source));  // ── scoreboard ──
    const ParseResults parsed = dispatcher_.parse(command, 0, source);
    auto               result = dispatcher_.execute(parsed, command, source);
    if (!result) {
        failure(source, result.error());
    }
    flush_scoreboard();  // ── scoreboard ── a command that changed and said nothing
    return result;
}

std::optional<PersonalSpawn> CommandService::personal_spawn(const net::Uuid& uuid) const {
    const auto it = spawns_.find(uuid.to_string());
    if (it == spawns_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// ── Feedback ────────────────────────────────────────────────────────────────

void CommandService::reply(const CommandSource& source, const Text& text) {
    // ── scoreboard ── What a command changed reaches the clients before what
    // it says, as vanilla's packets do; and a player it names wears its team.
    flush_scoreboard();
    const Text shown = decorate(text);
    if (source.is_player()) {
        if (PlayerRef* p = player(source.entity_id)) {
            p->send(net::clientbound::kSystemChat, net::encode_system_chat(to_json(shown), false));
        }
        return;
    }
    if (console) {
        console(plain(shown, lang()));
    }
}

Text CommandService::source_name(const CommandSource& source) const {
    return source.is_player() ? player_display_name(source.name, source.uuid)
                              : Text::literal("Server");
}

void CommandService::success(const CommandSource& source, const Text& text, bool broadcast_to_ops) {
    const bool feedback = world_.rules.flag("sendCommandFeedback");
    if (!source.is_player() || feedback) {
        reply(source, text);
    }
    if (!broadcast_to_ops) {
        return;
    }
    // ── scoreboard ── The source as it was named when the command began: the
    // capture logs `team leave @s` under the team just left, and `team join`
    // under no team yet.
    Text admin = Text::translatable("chat.type.admin", {source_shown_, decorate(text)});
    admin.style.italic = true;
    admin.color("gray");
    if (feedback) {
        const std::string json = to_json(admin);
        for (PlayerRef& p : players_) {
            if (source.is_player() && p.entity_id == source.entity_id) {
                continue;
            }
            // ── allow-commands ── in singleplayer, the host with cheats only.
            const bool op = config_.integrated ? permission_for(p) > 0
                                               : ops_.level_of(p.uuid).has_value();
            if (op) {
                p.send(net::clientbound::kSystemChat, net::encode_system_chat(json, false));
            }
        }
    }
    if (source.is_player() && world_.rules.flag("logAdminCommands") && console) {
        console(plain(admin, lang()));
    }
}

void CommandService::failure(const CommandSource& source, const CommandError& error) {
    for (const Text& line : error_lines(error)) {
        reply(source, line);
    }
}

// ── Arguments that need the world ───────────────────────────────────────────

Parsed<std::vector<const EntityInfo*>> CommandService::entities(const CommandContext& ctx,
                                                                std::string_view     name) {
    const EntitySelector* selector = ctx.find<EntitySelector>(name);
    if (selector == nullptr) {
        return std::unexpected{
            CommandError::plain(Text::translatable("argument.entity.notfound.entity"))};
    }
    auto found = find_entities(*selector, ctx.source(), world_snapshot_,
                               [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); },
                               &env_);
    if (!found) {
        return found;
    }
    if (found->empty()) {
        return std::unexpected{
            CommandError::plain(Text::translatable("argument.entity.notfound.entity"))};
    }
    return found;
}

Parsed<std::vector<PlayerRef*>> CommandService::players(const CommandContext& ctx,
                                                        std::string_view     name) {
    const EntitySelector* selector = ctx.find<EntitySelector>(name);
    std::vector<PlayerRef*> out;
    if (selector != nullptr) {
        auto found = find_entities(
            *selector, ctx.source(), world_snapshot_,
            [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); }, &env_);
        if (!found) {
            return std::unexpected{found.error()};
        }
        for (const EntityInfo* e : *found) {
            if (e->player) {
                if (PlayerRef* p = player(e->id)) {
                    out.push_back(p);
                }
            }
        }
    }
    if (out.empty()) {
        return std::unexpected{
            CommandError::plain(Text::translatable("argument.entity.notfound.player"))};
    }
    return out;
}

Parsed<PlayerRef*> CommandService::source_player(const CommandSource& source) {
    PlayerRef* p = source.is_player() ? player(source.entity_id) : nullptr;
    if (p == nullptr) {
        return std::unexpected{
            CommandError::plain(Text::translatable("permissions.requires.player"))};
    }
    return p;
}

Text CommandService::display(const EntityInfo& entity) const {
    return entity_display_name(entity, env_, lang());
}

Text CommandService::resolve(const Text& text, const CommandSource& source) {
    Text out = text;
    if (out.kind == Text::Kind::Selector) {
        StringReader reader{out.text};
        Text         names = Text::literal("");
        if (auto selector = parse_entity_selector(reader, env_)) {
            auto found = find_entities(
                *selector, source, world_snapshot_,
                [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); },
                &env_);
            if (found) {
                for (usize i = 0; i < found->size(); ++i) {
                    if (i > 0) {
                        names.append(Text::literal(", ").color("gray"));
                    }
                    names.append(display(*(*found)[i]));
                }
            }
        }
        if (names.extra.size() == 1) {
            names = std::move(names.extra.front());
        }
        names.style = out.style.empty() ? names.style : out.style;
        for (const Text& child : out.extra) {
            names.extra.push_back(resolve(child, source));
        }
        return names;
    }
    if (out.kind == Text::Kind::Score) {
        // ── scoreboard ── The value, as text; nothing when there is none.
        // `*` is whoever reads it; a selector names its first match.
        std::string holder = out.text;
        if (holder == "*") {
            holder = source.name;
        } else if (!holder.empty() && holder.front() == '@') {
            const std::string written = holder;  // the reader keeps a view: not of `holder`
            StringReader      reader{written};
            holder.clear();
            if (auto selector = parse_entity_selector(reader, env_)) {
                auto found = find_entities(
                    *selector, source, world_snapshot_,
                    [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); },
                    &env_);
                if (found && !found->empty()) {
                    const EntityInfo& e = *found->front();
                    holder              = e.player ? e.name : e.uuid.to_string();
                }
            }
        }
        const auto score = holder.empty() ? std::nullopt : scoreboard_.score(holder, out.key);
        out.kind         = Text::Kind::Literal;
        out.text         = score ? std::to_string(score->value) : std::string{};
        out.key.clear();
    }
    if (out.kind == Text::Kind::Nbt) {
        out.kind = Text::Kind::Literal;
        out.text.clear();
    }
    for (Text& child : out.extra) {
        child = resolve(child, source);
    }
    for (Text& argument : out.with) {
        argument = resolve(argument, source);
    }
    for (HoverEvent& hover : out.style.hover) {
        for (Text& shown : hover.text) {
            shown = resolve(shown, source);
        }
    }
    return out;
}

std::string CommandService::resolve(const MessageArg& message, const CommandSource& source) {
    if (message.selectors.empty() || !source.has_permission(kPermissionGameMaster)) {
        return message.text;
    }
    std::string out;
    usize       at = 0;
    for (const MessageArg::Part& part : message.selectors) {
        out += message.text.substr(at, part.start - at);
        auto found = find_entities(
            part.selector, source, world_snapshot_,
            [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); }, &env_);
        if (found) {
            for (usize i = 0; i < found->size(); ++i) {
                out += i == 0 ? "" : ", ";
                const EntityInfo& e = *(*found)[i];
                out += e.player ? e.name : plain(display(e), lang());
            }
        }
        at = part.end;
    }
    out += message.text.substr(std::min(at, message.text.size()));
    return out;
}

}  // namespace ov::server::cmd
