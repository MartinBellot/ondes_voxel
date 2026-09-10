#include "service.hpp"

#include "names.hpp"
#include "snbt.hpp"

#include "ov/gameplay/experience.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/survival.hpp"

#include <algorithm>

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

void CommandService::load_world(const world::LevelSettings& settings) {
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
                                            i32 chat_type, const std::optional<Text>& target) {
    net::PlayerChat chat;
    chat.sender    = uuid;
    chat.index     = 0;
    chat.body      = std::string{message};
    chat.timestamp = timestamp;
    chat.salt      = salt;
    chat.chat_type = chat_type;
    chat.name_json = to_json(player_display_name(name, uuid));
    if (target) {
        chat.target_json = to_json(*target);
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
    const i32 level = config_.integrated ? kPermissionOwner : ops_.level_of(p.uuid).value_or(0);
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
        out.push_back(std::move(info));
    }
    if (host_ != nullptr && host_->entities) {
        host_->entities(out);
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
}

Parsed<i32> CommandService::execute(const CommandSource& source, std::string_view command,
                                    CommandHost& host, i64 timestamp, i64 salt) {
    host_      = &host;
    timestamp_ = timestamp;
    salt_      = salt;
    refresh_players();
    world_snapshot_ = snapshot();
    const ParseResults parsed = dispatcher_.parse(command, 0, source);
    auto               result = dispatcher_.execute(parsed, command, source);
    if (!result) {
        failure(source, result.error());
    }
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
    if (source.is_player()) {
        if (PlayerRef* p = player(source.entity_id)) {
            p->send(net::clientbound::kSystemChat, net::encode_system_chat(to_json(text), false));
        }
        return;
    }
    if (console) {
        console(plain(text, lang()));
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
    Text admin = Text::translatable("chat.type.admin", {source_name(source), text});
    admin.style.italic = true;
    admin.color("gray");
    if (feedback) {
        const std::string json = to_json(admin);
        for (PlayerRef& p : players_) {
            if (source.is_player() && p.entity_id == source.entity_id) {
                continue;
            }
            const bool op = config_.integrated || ops_.level_of(p.uuid).has_value();
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
    if (out.kind == Text::Kind::Score || out.kind == Text::Kind::Nbt) {
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
