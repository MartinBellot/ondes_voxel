// /scoreboard, /team, /trigger, /teammsg and /tm — and what a team does to a
// player's name.
//
// Every tree is the one the real server's Commands packet declares (same
// literals, argument names, parsers and suggestion sources), registered at
// vanilla's place in the root: `scoreboard` after `say`, `team` / `teammsg` /
// `tm` after `summon`, `trigger` after `title`. Every feedback key and every
// argument — which are bare strings (`"with":["ovprobe","5"]`) and which are
// components — was read off the capture of scripts/capture_scoreboard.py,
// and tests/test_scoreboard.cpp holds this file to it.
//
// The rule of vanilla's feedback, as the console's admin lines show it:
// what changes something is announced to operators; what only reads
// (`list`, `get`) is not.
#include "service.hpp"

#include "json.hpp"
#include "names.hpp"

#include "ov/protocol/chat.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

namespace ov::server::cmd {
namespace {

/// `minecraft:chat_type`'s team messages, as the capture numbers them.
constexpr i32 kChatTypeTeamIncoming = 5;
constexpr i32 kChatTypeTeamOutgoing = 6;

[[nodiscard]] Text raw(i64 value) { return Text::raw(std::to_string(value)); }
[[nodiscard]] Text raw(std::string_view value) { return Text::raw(std::string{value}); }

[[nodiscard]] CommandError error(std::string key, std::vector<Text> with = {}) {
    return CommandError::plain(Text::translatable(std::move(key), std::move(with)));
}

/// A stored component read back; the plain `fallback` if it does not read.
[[nodiscard]] Text component(std::string_view json, std::string_view fallback) {
    usize consumed = 0;
    if (auto value = parse_json(json, consumed)) {
        if (auto text = text_from_json(*value)) {
            return std::move(*text);
        }
    }
    return Text::literal(std::string{fallback});
}

[[nodiscard]] HoverEvent hover_text(Text shown) {
    HoverEvent hover;
    hover.action = HoverEvent::Action::ShowText;
    hover.text.push_back(std::move(shown));
    return hover;
}

/// The display name, in brackets, hovering the objective's name.
[[nodiscard]] Text objective_text(const Objective& objective) {
    Text inner        = component(objective.display_json, objective.name);
    inner.style.hover = {hover_text(Text::literal(objective.name))};
    return Text::translatable("chat.square_brackets", {std::move(inner)});
}

/// The display name, in brackets, inserting and hovering the team's name, the
/// whole in the team's colour.
[[nodiscard]] Text team_text(const Team& team) {
    Text inner            = component(team.display_json, team.name);
    inner.style.insertion = team.name;
    inner.style.hover     = {hover_text(Text::literal(team.name))};
    Text out              = Text::translatable("chat.square_brackets", {std::move(inner)});
    if (team.color < kColorCount) {
        out.color(std::string{color_name(team.color)});
    }
    return out;
}

/// One element as itself; several in an empty component, joined by a gray
/// ", " — the capture's lists.
[[nodiscard]] Text format_list(std::vector<Text> items) {
    if (items.size() == 1) {
        return std::move(items.front());
    }
    Text out = Text::literal("");
    for (usize i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.append(Text::literal(", ").color("gray"));
        }
        out.append(std::move(items[i]));
    }
    return out;
}

/// Names in green, sorted — `players list` and a team's members.
[[nodiscard]] Text green_names(std::vector<std::string> names) {
    std::ranges::sort(names);
    std::vector<Text> items;
    for (std::string& name : names) {
        items.push_back(Text::literal(std::move(name)).color("green"));
    }
    return format_list(std::move(items));
}

[[nodiscard]] i32 wrap(i64 value) { return static_cast<i32>(static_cast<u32>(static_cast<u64>(value))); }

/// Math.floorDiv / floorMod, with Java's overflow (MIN / -1 is MIN).
[[nodiscard]] i32 floor_div(i32 a, i32 b) {
    if (a == std::numeric_limits<i32>::min() && b == -1) {
        return a;
    }
    i32 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}
[[nodiscard]] i32 floor_mod(i32 a, i32 b) {
    if (b == -1) {
        return 0;
    }
    return a - floor_div(a, b) * b;
}

}  // namespace

// ── Names and packets ───────────────────────────────────────────────────────

Text CommandService::decorate(Text text) const {
    const bool player_name =
        text.kind == Text::Kind::Literal && text.extra.empty() && text.style.hover.size() == 1 &&
        text.style.hover.front().action == HoverEvent::Action::ShowEntity &&
        text.style.hover.front().entity_type == "minecraft:player" && text.style.insertion == text.text;
    if (player_name) {
        if (const Team* team = scoreboard_.team_of(text.text)) {
            // PlayerTeam.formatNameForTeam: prefix, name, suffix, in the
            // team's colour — on the component that already carries the
            // click, the hover and the insertion.
            std::string name = std::move(text.text);
            text.text.clear();
            if (team->color < kColorCount) {
                text.style.color = std::string{color_name(team->color)};
            }
            text.extra.push_back(component(team->prefix_json, ""));
            text.extra.push_back(Text::literal(std::move(name)));
            text.extra.push_back(component(team->suffix_json, ""));
        }
        return text;
    }
    for (Text& argument : text.with) {
        argument = decorate(std::move(argument));
    }
    for (Text& child : text.extra) {
        child = decorate(std::move(child));
    }
    return text;
}

Text CommandService::death_message(std::string_view key, std::string_view victim,
                                   const net::Uuid& victim_uuid, std::string_view killer,
                                   const net::Uuid& killer_uuid) const {
    std::vector<Text> with{player_display_name(victim, victim_uuid)};
    if (!killer.empty()) {
        with.push_back(player_display_name(killer, killer_uuid));
    }
    return decorate(Text::translatable(std::string{key}, std::move(with)));
}

void CommandService::flush_scoreboard(const std::function<void(i32, std::span<const u8>)>& send) {
    if (!scoreboard_.has_packets() || !send) {
        return;
    }
    for (const ScoreboardPacket& packet : scoreboard_.take_packets()) {
        send(packet.id, packet.payload);
    }
}

void CommandService::flush_scoreboard() {
    if (host_ != nullptr && host_->broadcast) {
        flush_scoreboard(host_->broadcast);
    }
}

Parsed<std::vector<std::string>> CommandService::score_holders(const CommandContext& ctx,
                                                               std::string_view name, bool single) {
    const GameProfileArg* arg = ctx.find<GameProfileArg>(name);
    std::vector<std::string> out;
    if (arg == nullptr) {
        return std::unexpected{error("argument.scoreHolder.empty")};
    }
    if (arg->selector) {
        auto found = find_entities(
            *arg->selector, ctx.source(), world_snapshot_,
            [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); }, &env_);
        if (!found) {
            return std::unexpected{found.error()};
        }
        for (const EntityInfo* e : *found) {
            out.push_back(e->player ? e->name : e->uuid.to_string());
        }
    } else if (arg->name == "*") {
        if (!single) {
            out = scoreboard_.holders();
        }
    } else {
        out.push_back(arg->name);
    }
    if (out.empty()) {
        return std::unexpected{error("argument.scoreHolder.empty")};
    }
    return out;
}

// ── /scoreboard ─────────────────────────────────────────────────────────────

void CommandService::register_scoreboard_command() {
    Dispatcher& d = dispatcher_;

    const auto objective_of = [this](const CommandContext& ctx, std::string_view arg,
                                     bool writable) -> Parsed<const Objective*> {
        const std::string& name      = *ctx.find<std::string>(arg);
        const Objective*   objective = scoreboard_.objective(name);
        if (objective == nullptr) {
            return std::unexpected{error("arguments.objective.notFound", {raw(name)})};
        }
        if (writable && objective->criterion.read_only()) {
            return std::unexpected{error("arguments.objective.readonly", {raw(name)})};
        }
        return objective;
    };

    const u32 scoreboard = d.literal(d.root(), "scoreboard");
    d.requires_permission(scoreboard, kPermissionGameMaster);

    // ── objectives ──
    const u32 objectives = d.literal(scoreboard, "objectives");
    d.literal(objectives, "list", [this](const CommandContext& ctx) -> Parsed<i32> {
        const auto all = scoreboard_.objectives();
        if (all.empty()) {
            success(ctx.source(), Text::translatable("commands.scoreboard.objectives.list.empty"), false);
            return 0;
        }
        std::vector<Text> names;
        for (const Objective* o : all) {
            names.push_back(objective_text(*o));
        }
        success(ctx.source(),
                Text::translatable("commands.scoreboard.objectives.list.success",
                                   {raw(static_cast<i64>(all.size())), format_list(std::move(names))}),
                false);
        return static_cast<i32>(all.size());
    });
    {
        const Executor add = [this](const CommandContext& ctx) -> Parsed<i32> {
            const std::string& name = *ctx.find<std::string>("objective");
            const auto criterion = parse_criterion(*ctx.find<std::string>("criteria"), env_.registries());
            if (!criterion) {
                return std::unexpected{
                    error("argument.criteria.invalid", {raw(*ctx.find<std::string>("criteria"))})};
            }
            if (scoreboard_.objective(name) != nullptr) {
                return std::unexpected{error("commands.scoreboard.objectives.add.duplicate")};
            }
            const Text* given   = ctx.find<Text>("displayName");
            std::string display = to_json(given != nullptr ? *given : Text::literal(name));
            const Objective* o  = scoreboard_.add_objective(name, *criterion, std::move(display),
                                                            criterion->default_render());
            success(ctx.source(),
                    Text::translatable("commands.scoreboard.objectives.add.success", {objective_text(*o)}),
                    true);
            return static_cast<i32>(scoreboard_.objectives().size());
        };
        const u32 node      = d.literal(objectives, "add");
        const u32 objective = d.argument(node, "objective", ArgumentType::word());
        const u32 criteria  = d.argument(objective, "criteria", ArgumentType::objective_criteria(), add);
        d.argument(criteria, "displayName", ArgumentType::component(), add);
    }
    {
        const u32 modify    = d.literal(objectives, "modify");
        const u32 objective = d.argument(modify, "objective", ArgumentType::objective());
        d.argument(d.literal(objective, "displayname"), "displayName", ArgumentType::component(),
                   [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
                       auto o = objective_of(ctx, "objective", false);
                       if (!o) {
                           return std::unexpected{o.error()};
                       }
                       scoreboard_.set_display_name((*o)->name, to_json(*ctx.find<Text>("displayName")));
                       success(ctx.source(),
                               Text::translatable("commands.scoreboard.objectives.modify.displayname",
                                                  {raw((*o)->name), objective_text(**o)}),
                               true);
                       return 0;
                   });
        const u32 render = d.literal(objective, "rendertype");
        for (const RenderType type : {RenderType::Integer, RenderType::Hearts}) {
            d.literal(render, std::string{render_type_name(type)},
                      [this, objective_of, type](const CommandContext& ctx) -> Parsed<i32> {
                          auto o = objective_of(ctx, "objective", false);
                          if (!o) {
                              return std::unexpected{o.error()};
                          }
                          scoreboard_.set_render_type((*o)->name, type);
                          success(ctx.source(),
                                  Text::translatable("commands.scoreboard.objectives.modify.rendertype",
                                                     {objective_text(**o)}),
                                  true);
                          return 0;
                      });
        }
    }
    d.argument(d.literal(objectives, "remove"), "objective", ArgumentType::objective(),
               [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
                   auto o = objective_of(ctx, "objective", false);
                   if (!o) {
                       return std::unexpected{o.error()};
                   }
                   const Text shown = objective_text(**o);
                   (void)scoreboard_.remove_objective((*o)->name);
                   success(ctx.source(),
                           Text::translatable("commands.scoreboard.objectives.remove.success", {shown}),
                           true);
                   return static_cast<i32>(scoreboard_.objectives().size());
               });
    {
        const Executor setdisplay = [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
            const std::string& slot_name = *ctx.find<std::string>("slot");
            const u8           slot      = *display_slot_from_name(slot_name);
            if (!ctx.has("objective")) {
                if (scoreboard_.displayed(slot) == nullptr) {
                    return std::unexpected{error("commands.scoreboard.objectives.display.alreadyEmpty")};
                }
                scoreboard_.set_displayed(slot, {});
                success(ctx.source(),
                        Text::translatable("commands.scoreboard.objectives.display.cleared", {raw(slot_name)}),
                        true);
                return 0;
            }
            auto o = objective_of(ctx, "objective", false);
            if (!o) {
                return std::unexpected{o.error()};
            }
            if (scoreboard_.displayed(slot) == *o) {
                return std::unexpected{error("commands.scoreboard.objectives.display.alreadySet")};
            }
            scoreboard_.set_displayed(slot, (*o)->name);
            success(ctx.source(),
                    Text::translatable("commands.scoreboard.objectives.display.set",
                                       {raw(slot_name), component((*o)->display_json, (*o)->name)}),
                    true);
            return 0;
        };
        const u32 slot = d.argument(d.literal(objectives, "setdisplay"), "slot",
                                    ArgumentType::scoreboard_slot(), setdisplay);
        d.argument(slot, "objective", ArgumentType::objective(), setdisplay);
    }

    // ── players ──
    const u32  players_node = d.literal(scoreboard, "players");
    const auto holder_arg   = [&d](u32 parent, std::string name, bool multiple, Executor run = {}) {
        const u32 node = d.argument(parent, std::move(name), ArgumentType::score_holder(multiple), std::move(run));
        d.suggests(node, {}, "minecraft:ask_server");
        return node;
    };
    {
        const Executor list_all = [this](const CommandContext& ctx) -> Parsed<i32> {
            std::vector<std::string> holders = scoreboard_.holders();
            if (holders.empty()) {
                success(ctx.source(), Text::translatable("commands.scoreboard.players.list.empty"), false);
                return 0;
            }
            const auto count = static_cast<i64>(holders.size());
            success(ctx.source(),
                    Text::translatable("commands.scoreboard.players.list.success",
                                       {raw(count), green_names(std::move(holders))}),
                    false);
            return static_cast<i32>(count);
        };
        const Executor list_one = [this](const CommandContext& ctx) -> Parsed<i32> {
            auto holders = score_holders(ctx, "target", true);
            if (!holders) {
                return std::unexpected{holders.error()};
            }
            const std::string& name   = holders->front();
            const auto         scores = scoreboard_.scores_of(name);
            if (scores.empty()) {
                success(ctx.source(),
                        Text::translatable("commands.scoreboard.players.list.entity.empty", {raw(name)}),
                        false);
                return 0;
            }
            success(ctx.source(),
                    Text::translatable("commands.scoreboard.players.list.entity.success",
                                       {raw(name), raw(static_cast<i64>(scores.size()))}),
                    false);
            for (const auto& [objective, score] : scores) {
                success(ctx.source(),
                        Text::translatable("commands.scoreboard.players.list.entity.entry",
                                           {objective_text(*objective), raw(score.value)}),
                        false);
            }
            return static_cast<i32>(scores.size());
        };
        holder_arg(d.literal(players_node, "list", list_all), "target", false, list_one);
    }
    // set / add / remove
    {
        enum class Change : u8 { Set, Add, Remove };
        const auto change = [this, objective_of](Change how) {
            return [this, objective_of, how](const CommandContext& ctx) -> Parsed<i32> {
                auto holders = score_holders(ctx, "targets");
                if (!holders) {
                    return std::unexpected{holders.error()};
                }
                auto o = objective_of(ctx, "objective", true);
                if (!o) {
                    return std::unexpected{o.error()};
                }
                const i32 amount = *ctx.find<i32>("score");
                i64       total  = 0;
                i32       last   = 0;
                for (const std::string& holder : *holders) {
                    const i32 before = *scoreboard_.get_or_create(holder, (*o)->name);
                    last = how == Change::Set   ? amount
                           : how == Change::Add ? wrap(static_cast<i64>(before) + amount)
                                                : wrap(static_cast<i64>(before) - amount);
                    (void)scoreboard_.set_score(holder, (*o)->name, last);
                    total += last;
                }
                const bool  single = holders->size() == 1;
                const Text  shown  = objective_text(**o);
                const i64   count  = static_cast<i64>(holders->size());
                const auto  base   = std::string{"commands.scoreboard.players."} +
                                  (how == Change::Set ? "set" : how == Change::Add ? "add" : "remove") +
                                  (single ? ".success.single" : ".success.multiple");
                std::vector<Text> with;
                if (how == Change::Set) {
                    with = single ? std::vector<Text>{shown, raw(holders->front()), raw(amount)}
                                  : std::vector<Text>{shown, raw(count), raw(amount)};
                } else {
                    with = single ? std::vector<Text>{raw(amount), shown, raw(holders->front()), raw(last)}
                                  : std::vector<Text>{raw(amount), shown, raw(count)};
                }
                success(ctx.source(), Text::translatable(base, std::move(with)), true);
                return wrap(total);
            };
        };
        for (const auto& [name, how, min] :
             std::array<std::tuple<std::string_view, Change, bool>, 3>{
                 {{"set", Change::Set, false}, {"add", Change::Add, true}, {"remove", Change::Remove, true}}}) {
            const u32 targets   = holder_arg(d.literal(players_node, std::string{name}), "targets", true);
            const u32 objective = d.argument(targets, "objective", ArgumentType::objective());
            d.argument(objective, "score", min ? ArgumentType::integer_at_least(0) : ArgumentType::integer(),
                       change(how));
        }
    }
    // get — between set and add, vanilla's order
    {
        const u32 target = holder_arg(d.literal(players_node, "get"), "target", false);
        d.argument(target, "objective", ArgumentType::objective(),
                   [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
                       auto holders = score_holders(ctx, "target", true);
                       if (!holders) {
                           return std::unexpected{holders.error()};
                       }
                       auto o = objective_of(ctx, "objective", false);
                       if (!o) {
                           return std::unexpected{o.error()};
                       }
                       const std::string& name  = holders->front();
                       const auto         score = scoreboard_.score(name, (*o)->name);
                       if (!score) {
                           return std::unexpected{
                               error("commands.scoreboard.players.get.null", {raw((*o)->name), raw(name)})};
                       }
                       success(ctx.source(),
                               Text::translatable("commands.scoreboard.players.get.success",
                                                  {raw(name), raw(score->value), objective_text(**o)}),
                               false);
                       return score->value;
                   });
    }
    // reset
    {
        const Executor reset = [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
            auto holders = score_holders(ctx, "targets");
            if (!holders) {
                return std::unexpected{holders.error()};
            }
            const bool single = holders->size() == 1;
            const i64  count  = static_cast<i64>(holders->size());
            if (!ctx.has("objective")) {
                for (const std::string& holder : *holders) {
                    (void)scoreboard_.reset(holder);
                }
                success(ctx.source(),
                        single ? Text::translatable("commands.scoreboard.players.reset.all.single",
                                                    {raw(holders->front())})
                               : Text::translatable("commands.scoreboard.players.reset.all.multiple", {raw(count)}),
                        true);
                return static_cast<i32>(count);
            }
            auto o = objective_of(ctx, "objective", false);
            if (!o) {
                return std::unexpected{o.error()};
            }
            const Text shown = objective_text(**o);
            for (const std::string& holder : *holders) {
                (void)scoreboard_.reset(holder, (*o)->name);
            }
            success(ctx.source(),
                    single ? Text::translatable("commands.scoreboard.players.reset.specific.single",
                                                {shown, raw(holders->front())})
                           : Text::translatable("commands.scoreboard.players.reset.specific.multiple",
                                                {shown, raw(count)}),
                    true);
            return static_cast<i32>(count);
        };
        const u32 targets = holder_arg(d.literal(players_node, "reset"), "targets", true, reset);
        d.argument(targets, "objective", ArgumentType::objective(), reset);
    }
    // enable
    {
        const u32 targets   = holder_arg(d.literal(players_node, "enable"), "targets", true);
        const u32 objective = d.argument(
            targets, "objective", ArgumentType::objective(),
            [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
                auto holders = score_holders(ctx, "targets");
                if (!holders) {
                    return std::unexpected{holders.error()};
                }
                auto o = objective_of(ctx, "objective", false);
                if (!o) {
                    return std::unexpected{o.error()};
                }
                if ((*o)->criterion.kind != CriterionKind::Trigger) {
                    return std::unexpected{error("commands.scoreboard.players.enable.invalid")};
                }
                i32 enabled = 0;
                for (const std::string& holder : *holders) {
                    (void)scoreboard_.get_or_create(holder, (*o)->name);
                    if (scoreboard_.score(holder, (*o)->name)->locked) {
                        (void)scoreboard_.set_locked(holder, (*o)->name, false);
                        ++enabled;
                    }
                }
                if (enabled == 0) {
                    return std::unexpected{error("commands.scoreboard.players.enable.failed")};
                }
                const Text shown = objective_text(**o);
                success(ctx.source(),
                        holders->size() == 1
                            ? Text::translatable("commands.scoreboard.players.enable.success.single",
                                                 {shown, raw(holders->front())})
                            : Text::translatable("commands.scoreboard.players.enable.success.multiple",
                                                 {shown, raw(static_cast<i64>(holders->size()))}),
                        true);
                return enabled;
            });
        d.suggests(objective, {}, "minecraft:ask_server");
    }
    // operation
    {
        const Executor operation = [this, objective_of](const CommandContext& ctx) -> Parsed<i32> {
            auto targets = score_holders(ctx, "targets");
            if (!targets) {
                return std::unexpected{targets.error()};
            }
            auto target_objective = objective_of(ctx, "targetObjective", true);
            if (!target_objective) {
                return std::unexpected{target_objective.error()};
            }
            auto sources = score_holders(ctx, "source");
            if (!sources) {
                return std::unexpected{sources.error()};
            }
            auto source_objective = objective_of(ctx, "sourceObjective", false);
            if (!source_objective) {
                return std::unexpected{source_objective.error()};
            }
            const std::string& op     = *ctx.find<std::string>("operation");
            const std::string& to     = (*target_objective)->name;
            const std::string& from   = (*source_objective)->name;
            i64                total  = 0;
            i32                last   = 0;
            for (const std::string& target : *targets) {
                for (const std::string& source : *sources) {
                    const i32 a = *scoreboard_.get_or_create(target, to);
                    const i32 b = *scoreboard_.get_or_create(source, from);
                    i32       r = a;
                    if (op == "=") {
                        r = b;
                    } else if (op == "+=") {
                        r = wrap(static_cast<i64>(a) + b);
                    } else if (op == "-=") {
                        r = wrap(static_cast<i64>(a) - b);
                    } else if (op == "*=") {
                        r = wrap(static_cast<i64>(a) * b);
                    } else if (op == "/=" || op == "%=") {
                        if (b == 0) {
                            return std::unexpected{error("arguments.operation.div0")};
                        }
                        r = op == "/=" ? floor_div(a, b) : floor_mod(a, b);
                    } else if (op == "<") {
                        r = std::min(a, b);
                    } else if (op == ">") {
                        r = std::max(a, b);
                    } else if (op == "><") {
                        r = b;
                        (void)scoreboard_.set_score(target, to, r);
                        (void)scoreboard_.set_score(source, from, a);
                        continue;
                    }
                    (void)scoreboard_.set_score(target, to, r);
                }
                last = scoreboard_.score(target, to)->value;
                total += last;
            }
            const Text shown = objective_text(**target_objective);
            success(ctx.source(),
                    targets->size() == 1
                        ? Text::translatable("commands.scoreboard.players.operation.success.single",
                                             {shown, raw(targets->front()), raw(last)})
                        : Text::translatable("commands.scoreboard.players.operation.success.multiple",
                                             {shown, raw(static_cast<i64>(targets->size()))}),
                    true);
            return wrap(total);
        };
        const u32 targets   = holder_arg(d.literal(players_node, "operation"), "targets", true);
        const u32 target_o  = d.argument(targets, "targetObjective", ArgumentType::objective());
        const u32 op        = d.argument(target_o, "operation", ArgumentType::operation());
        const u32 source    = holder_arg(op, "source", true);
        d.argument(source, "sourceObjective", ArgumentType::objective(), operation);
    }
}

// ── /team, /teammsg, /tm ────────────────────────────────────────────────────

void CommandService::register_team_commands() {
    Dispatcher& d = dispatcher_;

    const auto team_of_arg = [this](const CommandContext& ctx) -> Parsed<const Team*> {
        const std::string& name = *ctx.find<std::string>("team");
        const Team*        team = scoreboard_.team(name);
        if (team == nullptr) {
            return std::unexpected{error("team.notFound", {raw(name)})};
        }
        return team;
    };
    const auto holder_arg = [&d](u32 parent, std::string name, Executor run) {
        const u32 node = d.argument(parent, std::move(name), ArgumentType::score_holder(true), std::move(run));
        d.suggests(node, {}, "minecraft:ask_server");
        return node;
    };

    const u32 team = d.literal(d.root(), "team");
    d.requires_permission(team, kPermissionGameMaster);

    // list
    {
        const Executor list = [this, team_of_arg](const CommandContext& ctx) -> Parsed<i32> {
            if (!ctx.has("team")) {
                const auto all = scoreboard_.teams();
                if (all.empty()) {
                    success(ctx.source(), Text::translatable("commands.team.list.teams.empty"), false);
                    return 0;
                }
                std::vector<Text> names;
                for (const Team* t : all) {
                    names.push_back(team_text(*t));
                }
                success(ctx.source(),
                        Text::translatable("commands.team.list.teams.success",
                                           {raw(static_cast<i64>(all.size())), format_list(std::move(names))}),
                        false);
                return static_cast<i32>(all.size());
            }
            auto t = team_of_arg(ctx);
            if (!t) {
                return std::unexpected{t.error()};
            }
            std::vector<std::string> members = (*t)->members();
            if (members.empty()) {
                success(ctx.source(),
                        Text::translatable("commands.team.list.members.empty", {team_text(**t)}), false);
                return 0;
            }
            const auto count = static_cast<i64>(members.size());
            success(ctx.source(),
                    Text::translatable("commands.team.list.members.success",
                                       {team_text(**t), raw(count), green_names(std::move(members))}),
                    false);
            return static_cast<i32>(count);
        };
        d.argument(d.literal(team, "list", list), "team", ArgumentType::team(), list);
    }
    // add
    {
        const Executor add = [this](const CommandContext& ctx) -> Parsed<i32> {
            const std::string& name = *ctx.find<std::string>("team");
            if (scoreboard_.team(name) != nullptr) {
                return std::unexpected{error("commands.team.add.duplicate")};
            }
            const Text* given = ctx.find<Text>("displayName");
            const Team* t     = scoreboard_.add_team(name, to_json(given != nullptr ? *given : Text::literal(name)));
            success(ctx.source(), Text::translatable("commands.team.add.success", {team_text(*t)}), true);
            return static_cast<i32>(scoreboard_.teams().size());
        };
        const u32 name = d.argument(d.literal(team, "add"), "team", ArgumentType::word(), add);
        d.argument(name, "displayName", ArgumentType::component(), add);
    }
    // remove
    d.argument(d.literal(team, "remove"), "team", ArgumentType::team(),
               [this, team_of_arg](const CommandContext& ctx) -> Parsed<i32> {
                   auto t = team_of_arg(ctx);
                   if (!t) {
                       return std::unexpected{t.error()};
                   }
                   const Text shown = team_text(**t);
                   (void)scoreboard_.remove_team((*t)->name);
                   success(ctx.source(), Text::translatable("commands.team.remove.success", {shown}), true);
                   return static_cast<i32>(scoreboard_.teams().size());
               });
    // empty
    d.argument(d.literal(team, "empty"), "team", ArgumentType::team(),
               [this, team_of_arg](const CommandContext& ctx) -> Parsed<i32> {
                   auto t = team_of_arg(ctx);
                   if (!t) {
                       return std::unexpected{t.error()};
                   }
                   if ((*t)->member_count() == 0) {
                       return std::unexpected{error("commands.team.empty.unchanged")};
                   }
                   const std::string name  = (*t)->name;
                   const usize       count = scoreboard_.empty_team(name);
                   success(ctx.source(),
                           Text::translatable("commands.team.empty.success",
                                              {raw(static_cast<i64>(count)), team_text(*scoreboard_.team(name))}),
                           true);
                   return static_cast<i32>(count);
               });
    // join
    {
        const Executor join = [this, team_of_arg](const CommandContext& ctx) -> Parsed<i32> {
            auto t = team_of_arg(ctx);
            if (!t) {
                return std::unexpected{t.error()};
            }
            std::vector<std::string> members;
            if (ctx.has("members")) {
                auto found = score_holders(ctx, "members");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                members = std::move(*found);
            } else {
                if (!ctx.source().is_player()) {
                    return std::unexpected{error("permissions.requires.entity")};
                }
                members.push_back(ctx.source().name);
            }
            const std::string name = (*t)->name;
            for (const std::string& member : members) {
                scoreboard_.join(name, member);
            }
            const Text shown = team_text(*scoreboard_.team(name));
            success(ctx.source(),
                    members.size() == 1
                        ? Text::translatable("commands.team.join.success.single", {raw(members.front()), shown})
                        : Text::translatable("commands.team.join.success.multiple",
                                             {raw(static_cast<i64>(members.size())), shown}),
                    true);
            return static_cast<i32>(members.size());
        };
        const u32 name = d.argument(d.literal(team, "join"), "team", ArgumentType::team(), join);
        holder_arg(name, "members", join);
    }
    // leave
    holder_arg(d.literal(team, "leave"), "members", [this](const CommandContext& ctx) -> Parsed<i32> {
        auto members = score_holders(ctx, "members");
        if (!members) {
            return std::unexpected{members.error()};
        }
        for (const std::string& member : *members) {
            (void)scoreboard_.leave(member);
        }
        success(ctx.source(),
                members->size() == 1
                    ? Text::translatable("commands.team.leave.success.single", {raw(members->front())})
                    : Text::translatable("commands.team.leave.success.multiple",
                                         {raw(static_cast<i64>(members->size()))}),
                true);
        return static_cast<i32>(members->size());
    });
    // modify
    {
        const u32 modify = d.argument(d.literal(team, "modify"), "team", ArgumentType::team());

        // Change one option through `apply`, which returns false when nothing
        // would change; then the Update, then the line.
        using Apply   = std::function<bool(Team&, const CommandContext&)>;
        using Message = std::function<Text(const Team&, const CommandContext&)>;
        // `announce`: whether operators see it — not for a prefix or a suffix,
        // which the capture's console never logged.
        const auto option = [this, team_of_arg](std::string unchanged, Apply apply, Message message,
                                                bool announce = true) {
            return [this, team_of_arg, unchanged = std::move(unchanged), apply = std::move(apply),
                    message = std::move(message), announce](const CommandContext& ctx) -> Parsed<i32> {
                auto t = team_of_arg(ctx);
                if (!t) {
                    return std::unexpected{t.error()};
                }
                Team& edited = *scoreboard_.edit_team((*t)->name);
                if (!apply(edited, ctx)) {
                    return std::unexpected{error(unchanged)};
                }
                scoreboard_.team_changed(edited.name);
                success(ctx.source(), message(edited, ctx), announce);
                return 0;
            };
        };

        d.argument(d.literal(modify, "displayName"), "displayName", ArgumentType::component(),
                   option("commands.team.option.name.unchanged",
                          [](Team& t, const CommandContext& ctx) {
                              std::string json = to_json(*ctx.find<Text>("displayName"));
                              if (json == t.display_json) {
                                  return false;
                              }
                              t.display_json = std::move(json);
                              return true;
                          },
                          [](const Team& t, const CommandContext&) {
                              return Text::translatable("commands.team.option.name.success", {team_text(t)});
                          }));
        d.argument(d.literal(modify, "color"), "value", ArgumentType::color(),
                   option("commands.team.option.color.unchanged",
                          [](Team& t, const CommandContext& ctx) {
                              const u8 color = *color_from_name(*ctx.find<std::string>("value"));
                              if (color == t.color) {
                                  return false;
                              }
                              t.color = color;
                              return true;
                          },
                          [](const Team& t, const CommandContext& ctx) {
                              return Text::translatable("commands.team.option.color.success",
                                                        {team_text(t), raw(*ctx.find<std::string>("value"))});
                          }));
        for (const bool see : {false, true}) {
            const std::string key = see ? "seeFriendlyInvisibles" : "friendlyfire";
            // Only the first option's unchanged key depends on the value, so
            // the check is done by hand here.
            d.argument(
                d.literal(modify, see ? "seeFriendlyInvisibles" : "friendlyFire"), "allowed",
                ArgumentType::boolean(),
                [this, team_of_arg, see, key](const CommandContext& ctx) -> Parsed<i32> {
                    auto t = team_of_arg(ctx);
                    if (!t) {
                        return std::unexpected{t.error()};
                    }
                    const bool allowed = *ctx.find<bool>("allowed");
                    Team&      edited  = *scoreboard_.edit_team((*t)->name);
                    bool&      field   = see ? edited.see_friendly_invisibles : edited.friendly_fire;
                    if (field == allowed) {
                        return std::unexpected{error("commands.team.option." + key +
                                                     (allowed ? ".alreadyEnabled" : ".alreadyDisabled"))};
                    }
                    field = allowed;
                    scoreboard_.team_changed(edited.name);
                    success(ctx.source(),
                            Text::translatable("commands.team.option." + key +
                                                   (allowed ? ".enabled" : ".disabled"),
                                               {team_text(edited)}),
                            true);
                    return 0;
                });
        }
        for (const bool death : {false, true}) {
            const std::string key  = death ? "deathMessageVisibility" : "nametagVisibility";
            const u32         node = d.literal(modify, key);
            for (const Visibility v : {Visibility::Never, Visibility::HideForOtherTeams,
                                       Visibility::HideForOwnTeam, Visibility::Always}) {
                const std::string name{visibility_name(v)};
                d.literal(node, name,
                          option("commands.team.option." + key + ".unchanged",
                                 [death, v](Team& t, const CommandContext&) {
                                     Visibility& field = death ? t.death_message : t.name_tag;
                                     if (field == v) {
                                         return false;
                                     }
                                     field = v;
                                     return true;
                                 },
                                 [key, name](const Team& t, const CommandContext&) {
                                     return Text::translatable("commands.team.option." + key + ".success",
                                                               {team_text(t),
                                                                Text::translatable("team.visibility." + name)});
                                 }));
            }
        }
        {
            const u32 node = d.literal(modify, "collisionRule");
            for (const CollisionRule rule : {CollisionRule::Never, CollisionRule::PushOwnTeam,
                                             CollisionRule::PushOtherTeams, CollisionRule::Always}) {
                const std::string name{collision_rule_name(rule)};
                d.literal(node, name,
                          option("commands.team.option.collisionRule.unchanged",
                                 [rule](Team& t, const CommandContext&) {
                                     if (t.collision == rule) {
                                         return false;
                                     }
                                     t.collision = rule;
                                     return true;
                                 },
                                 [name](const Team& t, const CommandContext&) {
                                     return Text::translatable("commands.team.option.collisionRule.success",
                                                               {team_text(t),
                                                                Text::translatable("team.collision." + name)});
                                 }));
            }
        }
        for (const bool suffix : {false, true}) {
            const std::string arg = suffix ? "suffix" : "prefix";
            d.argument(d.literal(modify, arg), arg, ArgumentType::component(),
                       option("", // never unchanged: vanilla sets and answers every time
                              [suffix, arg](Team& t, const CommandContext& ctx) {
                                  (suffix ? t.suffix_json : t.prefix_json) = to_json(*ctx.find<Text>(arg));
                                  return true;
                              },
                              [arg](const Team&, const CommandContext& ctx) {
                                  return Text::translatable("commands.team.option." + arg + ".success",
                                                            {*ctx.find<Text>(arg)});
                              },
                              false));
        }
    }

    // ── teammsg / tm ──
    const u32 teammsg = d.literal(d.root(), "teammsg");
    d.requires_permission(teammsg, kPermissionAll);
    d.argument(teammsg, "message", ArgumentType::message(), [this](const CommandContext& ctx) -> Parsed<i32> {
        const CommandSource& src = ctx.source();
        if (!src.is_player()) {
            return std::unexpected{error("permissions.requires.entity")};
        }
        const Team* team = scoreboard_.team_of(src.name);
        if (team == nullptr) {
            return std::unexpected{error("commands.teammsg.failed.noteam")};
        }
        const std::string text   = resolve(*ctx.find<MessageArg>("message"), src);
        Text              target = team_text(*team);
        target.style.click       = ClickEvent{"suggest_command", "/teammsg "};
        target.style.hover       = {hover_text(Text::translatable("chat.type.team.hover"))};
        i32 reached              = 0;
        for (PlayerRef& p : players_) {
            if (scoreboard_.team_of(p.name) != team) {
                continue;
            }
            const bool self = p.entity_id == src.entity_id;
            p.send(net::clientbound::kPlayerChat,
                   chat_packet(src.name, src.uuid, text, timestamp_, salt_,
                               self ? kChatTypeTeamOutgoing : kChatTypeTeamIncoming, target));
            ++reached;
        }
        return reached;
    });
    const u32 tm = d.literal(d.root(), "tm");
    d.requires_permission(tm, kPermissionAll);
    d.redirect(tm, teammsg);
}

// ── /trigger ────────────────────────────────────────────────────────────────

void CommandService::register_trigger_command() {
    Dispatcher& d = dispatcher_;
    enum class How : u8 { Simple, Add, Set };
    const auto trigger = [this](How how) {
        return [this, how](const CommandContext& ctx) -> Parsed<i32> {
            auto self = source_player(ctx.source());
            if (!self) {
                return std::unexpected{self.error()};
            }
            const std::string& name      = *ctx.find<std::string>("objective");
            const Objective*   objective = scoreboard_.objective(name);
            if (objective == nullptr) {
                return std::unexpected{error("arguments.objective.notFound", {raw(name)})};
            }
            if (objective->criterion.kind != CriterionKind::Trigger) {
                return std::unexpected{error("commands.trigger.failed.invalid")};
            }
            const std::string holder{(*self)->name};
            const auto        score = scoreboard_.score(holder, name);
            if (!score || score->locked) {
                return std::unexpected{error("commands.trigger.failed.unprimed")};
            }
            (void)scoreboard_.set_locked(holder, name, true);
            const i32* given = ctx.find<i32>("value");
            const i32  value = how == How::Simple ? wrap(static_cast<i64>(score->value) + 1)
                               : how == How::Add  ? wrap(static_cast<i64>(score->value) + *given)
                                                  : *given;
            (void)scoreboard_.set_score(holder, name, value);
            const Text shown = objective_text(*objective);
            success(ctx.source(),
                    how == How::Simple ? Text::translatable("commands.trigger.simple.success", {shown})
                    : how == How::Add  ? Text::translatable("commands.trigger.add.success", {shown, raw(*given)})
                                       : Text::translatable("commands.trigger.set.success", {shown, raw(*given)}),
                    true);
            return value;
        };
    };
    const u32 node = d.literal(d.root(), "trigger");
    d.requires_permission(node, kPermissionAll);
    const u32 objective = d.argument(node, "objective", ArgumentType::objective(), trigger(How::Simple));
    d.suggests(objective, {}, "minecraft:ask_server");
    d.argument(d.literal(objective, "add"), "value", ArgumentType::integer(), trigger(How::Add));
    d.argument(d.literal(objective, "set"), "value", ArgumentType::integer(), trigger(How::Set));
}

}  // namespace ov::server::cmd
