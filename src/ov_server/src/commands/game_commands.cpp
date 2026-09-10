// The commands, one by one.
//
// Registered in vanilla's own order — the order its Commands packet lists them
// in, which is the order `/help` prints and the client suggests — and each
// built to the tree the vanilla packet declares for it: the same literals,
// the same argument names and parsers, the same executable nodes. The
// feedback keys and their arguments are the ones the capture recorded, down to
// which arguments are bare strings (`"with":["1000"]`) and which are
// components.
//
// Where vanilla would do something this server does not model, the command
// says so by name rather than succeeding at nothing. The list is in
// docs/provenance/commandes.md.
#include "service.hpp"

#include "names.hpp"
#include "snbt.hpp"

#include "ov/gameplay/durability.hpp"
#include "ov/gameplay/experience.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/survival.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ov::server::cmd {
namespace {

constexpr std::array<std::string_view, 4> kGameModeNames{"survival", "creative", "adventure",
                                                         "spectator"};
constexpr std::array<std::string_view, 4> kDifficultyNames{"peaceful", "easy", "normal", "hard"};

[[nodiscard]] Text raw(i64 value) { return Text::raw(std::to_string(value)); }

[[nodiscard]] Text game_mode_text(u8 mode) {
    return Text::translatable("gameMode." + std::string{kGameModeNames[mode & 3U]});
}

[[nodiscard]] CommandError error(std::string key, std::vector<Text> with = {}) {
    return CommandError::plain(Text::translatable(std::move(key), std::move(with)));
}

[[nodiscard]] CommandError not_modelled(std::string_view what) {
    return CommandError::plain(
        Text::literal("Ondes VOXEL does not " + std::string{what} + " yet"));
}

/// Level.isInSpawnableBounds: inside the world border's hard limit, and
/// within ±20 000 000 vertically.
[[nodiscard]] bool spawnable(const Vec3d& p) {
    const auto bx = static_cast<i64>(std::floor(p.x));
    const auto by = static_cast<i64>(std::floor(p.y));
    const auto bz = static_cast<i64>(std::floor(p.z));
    return by >= -20'000'000 && by < 20'000'000 && bx >= -30'000'000 && bx < 30'000'000 &&
           bz >= -30'000'000 && bz < 30'000'000;
}

/// Level.isInWorldBounds for the overworld of 1.20.1: y in [-64, 320).
[[nodiscard]] bool in_world(const BlockPos& p) {
    return p.y >= -64 && p.y < 320 && p.x >= -30'000'000 && p.x < 30'000'000 &&
           p.z >= -30'000'000 && p.z < 30'000'000;
}

[[nodiscard]] std::vector<u8> tag_bytes(const nbt::Tag& tag) {
    nbt::Document document;
    document.name = "";
    document.root = tag;
    return nbt::write(document);
}

void send_slot(PlayerRef& p, i16 slot) {
    p.send(net::clientbound::kContainerSlot,
           net::encode_container_slot(0, 0, slot, (*p.inventory)[static_cast<usize>(slot)]));
}

void send_experience(PlayerRef& p) {
    SurvivalSession& s    = *p.survival;
    const i32        cost = gameplay::experience_to_next_level(s.experience_level, s.curve);
    const f32        bar  = cost > 0 ? static_cast<f32>(s.experience_points) / static_cast<f32>(cost)
                                     : 0.0F;
    p.send(net::clientbound::kSetExperience,
           net::encode_set_experience(bar, s.experience_level, s.experience_total));
    // What the client now has, so the survival tick does not send it again.
    s.broadcast_level  = s.experience_level;
    s.broadcast_points = s.experience_points;
}

/// Vanilla's Inventory.add, over the protocol's slot numbers: first a stack of
/// the same item with room (the held slot, the off hand, then the hotbar and
/// the main inventory), then the first empty slot. Returns what did not fit.
i32 add_to_inventory(PlayerRef& p, const net::ItemStack& stack, i8 max_stack) {
    auto& inventory = *p.inventory;
    i32   left      = stack.count;
    std::vector<i16> order;
    for (i16 s = 36; s < 45; ++s) {
        order.push_back(s);
    }
    for (i16 s = 9; s < 36; ++s) {
        order.push_back(s);
    }
    const auto mergeable = [&](i16 slot) {
        const net::ItemStack& s = inventory[static_cast<usize>(slot)];
        return !s.empty() && s.item_id == stack.item_id && s.nbt == stack.nbt && s.count < max_stack;
    };
    const bool damaged = [&] {
        if (stack.nbt.empty()) {
            return false;
        }
        const auto document = nbt::read(stack.nbt);
        const nbt::Tag* damage = document ? document->root.find("Damage") : nullptr;
        return damage != nullptr && damage->as_i64() > 0;
    }();
    while (left > 0) {
        i16 target = -1;
        if (!damaged) {
            const i16 held = static_cast<i16>(36 + p.held_slot);
            if (mergeable(held)) {
                target = held;
            } else if (mergeable(45)) {
                target = 45;
            } else {
                for (const i16 s : order) {
                    if (mergeable(s)) {
                        target = s;
                        break;
                    }
                }
            }
        }
        if (target < 0) {
            for (const i16 s : order) {
                if (inventory[static_cast<usize>(s)].empty()) {
                    target = s;
                    break;
                }
            }
        }
        if (target < 0) {
            break;
        }
        net::ItemStack& slot  = inventory[static_cast<usize>(target)];
        const i32       held  = slot.empty() ? 0 : slot.count;
        const i32       moved = std::min(left, static_cast<i32>(max_stack) - held);
        if (slot.empty()) {
            slot       = stack;
            slot.count = static_cast<i8>(moved);
        } else {
            slot.count = static_cast<i8>(held + moved);
        }
        left -= moved;
        send_slot(p, target);
    }
    return left;
}

}  // namespace

void CommandService::register_commands() {
    Dispatcher& d    = dispatcher_;
    const u32   root = d.root();
    const auto  top  = [&](std::string name, i32 permission, Executor run = {}) {
        const u32 node = d.literal(root, std::move(name), std::move(run));
        d.requires_permission(node, permission);
        return node;
    };

    // ── clear ───────────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            std::vector<PlayerRef*> targets;
            if (ctx.has("targets")) {
                auto found = players(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                targets = std::move(*found);
            } else {
                auto self = source_player(ctx.source());
                if (!self) {
                    return std::unexpected{self.error()};
                }
                targets.push_back(*self);
            }
            const ItemPredicateArg* item = ctx.find<ItemPredicateArg>("item");
            const i32*              max  = ctx.find<i32>("maxCount");
            const i32               max_count = max != nullptr ? *max : -1;
            const bool              dry       = max_count == 0;
            i32                     total     = 0;
            for (PlayerRef* p : targets) {
                i32        cleared = 0;
                const auto take    = [&](net::ItemStack& stack, i16 slot) {
                    if (stack.empty() || (item != nullptr && !item->test(stack, env_))) {
                        return;
                    }
                    if (dry) {
                        cleared += stack.count;
                        return;
                    }
                    const i32 budget = max_count < 0 ? stack.count : max_count - cleared;
                    const i32 n      = std::min<i32>(budget, stack.count);
                    if (n <= 0) {
                        return;
                    }
                    stack.count = static_cast<i8>(stack.count - n);
                    if (stack.count <= 0) {
                        stack = {};
                    }
                    cleared += n;
                    if (slot >= 0) {
                        send_slot(*p, slot);
                    }
                };
                // Vanilla's inventory order: hotbar, main, armour feet to
                // head, off hand, then the crafting grid and the cursor.
                for (i16 s = 36; s < 45; ++s) {
                    take((*p->inventory)[static_cast<usize>(s)], s);
                }
                for (i16 s = 9; s < 36; ++s) {
                    take((*p->inventory)[static_cast<usize>(s)], s);
                }
                for (const i16 s : {i16{8}, i16{7}, i16{6}, i16{5}, i16{45}, i16{1}, i16{2}, i16{3},
                                    i16{4}}) {
                    take((*p->inventory)[static_cast<usize>(s)], s);
                }
                if (p->carried != nullptr) {
                    take(*p->carried, -1);
                }
                total += cleared;
            }
            if (total == 0) {
                if (targets.size() == 1) {
                    return std::unexpected{error("clear.failed.single",
                                                 {Text::literal(std::string{targets[0]->name})})};
                }
                return std::unexpected{
                    error("clear.failed.multiple", {raw(static_cast<i64>(targets.size()))})};
            }
            const std::string key = std::string{"commands.clear."} + (dry ? "test." : "success.") +
                                    (targets.size() == 1 ? "single" : "multiple");
            if (targets.size() == 1) {
                const EntityInfo* e = nullptr;
                for (const EntityInfo& info : world_snapshot_) {
                    if (info.id == targets[0]->entity_id) {
                        e = &info;
                    }
                }
                success(ctx.source(),
                        Text::translatable(key, {raw(total), e != nullptr ? display(*e)
                                                                          : Text::literal("")}),
                        true);
            } else {
                success(ctx.source(),
                        Text::translatable(key, {raw(total), raw(static_cast<i64>(targets.size()))}),
                        true);
            }
            return total;
        };
        const u32 clear   = top("clear", kPermissionGameMaster, run);
        const u32 targets = d.argument(clear, "targets", ArgumentType::entity(false, true), run);
        const u32 item    = d.argument(targets, "item", ArgumentType::item_predicate(), run);
        d.argument(item, "maxCount", ArgumentType::integer_at_least(0), run);
    }

    // ── defaultgamemode ─────────────────────────────────────────────────────
    {
        const u32 node = top("defaultgamemode", kPermissionGameMaster);
        d.argument(node, "gamemode", ArgumentType::game_mode(),
                   [this](const CommandContext& ctx) -> Parsed<i32> {
                       const u8 mode             = ctx.find<GameModeArg>("gamemode")->mode;
                       world_.default_game_mode = mode;
                       default_mode_.store(mode, std::memory_order_relaxed);
                       success(ctx.source(),
                               Text::translatable("commands.defaultgamemode.success",
                                                  {game_mode_text(mode)}),
                               true);
                       return 0;
                   });
    }

    // ── difficulty ──────────────────────────────────────────────────────────
    {
        const u32 node = top("difficulty", kPermissionGameMaster,
                             [this](const CommandContext& ctx) -> Parsed<i32> {
                                 success(ctx.source(),
                                         Text::translatable(
                                             "commands.difficulty.query",
                                             {Text::translatable(
                                                 "options.difficulty." +
                                                 std::string{kDifficultyNames[world_.difficulty]})}),
                                         false);
                                 return world_.difficulty;
                             });
        for (u8 level = 0; level < 4; ++level) {
            d.literal(node, std::string{kDifficultyNames[level]},
                      [this, level](const CommandContext& ctx) -> Parsed<i32> {
                          const std::string name{kDifficultyNames[level]};
                          if (world_.difficulty == level) {
                              return std::unexpected{
                                  error("commands.difficulty.failure", {Text::raw(name)})};
                          }
                          world_.difficulty = level;
                          host_->broadcast(net::clientbound::kChangeDifficulty,
                                           net::encode_change_difficulty(level, world_.difficulty_locked));
                          success(ctx.source(),
                                  Text::translatable("commands.difficulty.success",
                                                     {Text::translatable("options.difficulty." + name)}),
                                  true);
                          return 0;
                      });
        }
    }

    // `effect` belongs here, between `difficulty` and `me`, in vanilla's order.
    // Another wave writes status effects; its command registers at this line.

    // ── me ──────────────────────────────────────────────────────────────────
    {
        const u32 node = top("me", kPermissionAll);
        d.argument(node, "action", ArgumentType::message(),
                   [this](const CommandContext& ctx) -> Parsed<i32> {
                       const std::string text = resolve(*ctx.find<MessageArg>("action"), ctx.source());
                       const CommandSource& src = ctx.source();
                       if (src.is_player()) {
                           host_->broadcast(net::clientbound::kPlayerChat,
                                            chat_packet(src.name, src.uuid, text, timestamp_, salt_,
                                                        kChatTypeEmote));
                           if (console) {
                               console("[Not Secure] * " + src.name + " " + text);
                           }
                       } else {
                           host_->broadcast(net::clientbound::kDisguisedChat,
                                            net::encode_disguised_chat(
                                                {to_json(Text::literal(text)), kChatTypeEmote,
                                                 to_json(Text::literal("Server")), std::nullopt}));
                           if (console) {
                               console("* Server " + text);
                           }
                       }
                       return 1;
                   });
    }

    // ── experience / xp ─────────────────────────────────────────────────────
    {
        const u32 node = top("experience", kPermissionGameMaster);
        const auto apply = [this](bool set, bool levels) {
            return [this, set, levels](const CommandContext& ctx) -> Parsed<i32> {
                auto targets = players(ctx, "targets");
                if (!targets) {
                    return std::unexpected{targets.error()};
                }
                const i32 amount  = *ctx.find<i32>("amount");
                i32       applied = 0;
                for (PlayerRef* p : *targets) {
                    SurvivalSession& s = *p->survival;
                    if (set && !levels) {
                        const i32 cost = gameplay::experience_to_next_level(s.experience_level, s.curve);
                        if (amount > cost) {
                            continue;
                        }
                        s.experience_points = amount;
                    } else if (set) {
                        s.experience_level = amount;
                    } else if (levels) {
                        s.experience_level += amount;
                        if (s.experience_level < 0) {
                            s.experience_level  = 0;
                            s.experience_points = 0;
                            s.experience_total  = 0;
                        }
                    } else if (amount > 0) {
                        s.award_experience(amount);
                    } else if (amount < 0) {
                        s.spend_experience(-amount);
                    }
                    send_experience(*p);
                    ++applied;
                }
                if (applied == 0) {
                    return std::unexpected{error("commands.experience.set.points.invalid")};
                }
                const std::string key = std::string{"commands.experience."} +
                                        (set ? "set." : "add.") + (levels ? "levels" : "points") +
                                        ".success." + (targets->size() == 1 ? "single" : "multiple");
                if (targets->size() == 1) {
                    success(ctx.source(),
                            Text::translatable(key, {raw(amount),
                                                     player_display_name((*targets)[0]->name,
                                                                         (*targets)[0]->uuid)}),
                            true);
                } else {
                    success(ctx.source(),
                            Text::translatable(key, {raw(amount), raw(static_cast<i64>(targets->size()))}),
                            true);
                }
                return static_cast<i32>(targets->size());
            };
        };
        const auto query = [this](bool levels) {
            return [this, levels](const CommandContext& ctx) -> Parsed<i32> {
                auto targets = players(ctx, "targets");
                if (!targets) {
                    return std::unexpected{targets.error()};
                }
                PlayerRef* p     = targets->front();
                const i32  value = levels ? p->survival->experience_level
                                          : p->survival->experience_points;
                success(ctx.source(),
                        Text::translatable(levels ? "commands.experience.query.levels"
                                                  : "commands.experience.query.points",
                                           {player_display_name(p->name, p->uuid), raw(value)}),
                        false);
                return value;
            };
        };
        const u32 add         = d.literal(node, "add");
        const u32 add_targets = d.argument(add, "targets", ArgumentType::entity(false, true));
        const u32 add_amount  = d.argument(add_targets, "amount", ArgumentType::integer(), apply(false, false));
        d.literal(add_amount, "points", apply(false, false));
        d.literal(add_amount, "levels", apply(false, true));
        const u32 set         = d.literal(node, "set");
        const u32 set_targets = d.argument(set, "targets", ArgumentType::entity(false, true));
        const u32 set_amount  = d.argument(set_targets, "amount", ArgumentType::integer_at_least(0),
                                           apply(true, false));
        d.literal(set_amount, "points", apply(true, false));
        d.literal(set_amount, "levels", apply(true, true));
        const u32 q         = d.literal(node, "query");
        const u32 q_targets = d.argument(q, "targets", ArgumentType::entity(true, true));
        d.literal(q_targets, "points", query(false));
        d.literal(q_targets, "levels", query(true));

        const u32 xp = top("xp", kPermissionGameMaster);
        d.redirect(xp, node);
    }

    // ── fill ────────────────────────────────────────────────────────────────
    {
        enum class Mode : u8 { Replace, Keep, Outline, Hollow, Destroy };
        const auto fill = [this](Mode mode) {
            return [this, mode](const CommandContext& ctx) -> Parsed<i32> {
                const CommandSource& src  = ctx.source();
                const BlockPos       a    = ctx.find<Coordinates>("from")->block(src);
                const BlockPos       b    = ctx.find<Coordinates>("to")->block(src);
                for (const BlockPos& corner : {a, b}) {
                    if (!host_->is_loaded(corner.x >> 4, corner.z >> 4)) {
                        return std::unexpected{error("argument.pos.unloaded")};
                    }
                    if (!in_world(corner)) {
                        return std::unexpected{error("argument.pos.outofworld")};
                    }
                }
                const BlockPos low{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
                const BlockPos high{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
                const i64      volume = static_cast<i64>(high.x - low.x + 1) *
                                   static_cast<i64>(high.y - low.y + 1) *
                                   static_cast<i64>(high.z - low.z + 1);
                const i32 limit = world_.rules.number("commandModificationBlockLimit");
                if (volume > limit) {
                    return std::unexpected{error("commands.fill.toobig", {raw(limit), raw(volume)})};
                }
                for (i32 cx = low.x >> 4; cx <= high.x >> 4; ++cx) {
                    for (i32 cz = low.z >> 4; cz <= high.z >> 4; ++cz) {
                        if (!host_->is_loaded(cx, cz)) {
                            return std::unexpected{error("argument.pos.unloaded")};
                        }
                    }
                }
                const BlockStateArg& block = *ctx.find<BlockStateArg>("block");
                if (block.nbt && !block.nbt->empty()) {
                    return std::unexpected{not_modelled("apply block entity NBT from a command")};
                }
                const BlockPredicateArg* filter = ctx.find<BlockPredicateArg>("filter");
                const auto is_air = [&](registry::BlockStateId s) {
                    return env_.blocks()->is_air(env_.blocks()->block_of(s));
                };
                std::vector<BlockChange> changes;
                for (i32 z = low.z; z <= high.z; ++z) {
                    for (i32 y = low.y; y <= high.y; ++y) {
                        for (i32 x = low.x; x <= high.x; ++x) {
                            const BlockPos pos{x, y, z};
                            registry::BlockStateId current = host_->block_at(pos);
                            const bool edge = x == low.x || x == high.x || y == low.y ||
                                              y == high.y || z == low.z || z == high.z;
                            registry::BlockStateId target = block.state;
                            switch (mode) {
                            case Mode::Replace:
                                if (filter != nullptr && !filter->test(current, env_)) {
                                    continue;
                                }
                                break;
                            case Mode::Keep:
                                if (!is_air(current)) {
                                    continue;
                                }
                                break;
                            case Mode::Outline:
                                if (!edge) {
                                    continue;
                                }
                                break;
                            case Mode::Hollow:
                                if (!edge) {
                                    target = registry::kAirState;
                                }
                                break;
                            case Mode::Destroy:
                                if (!is_air(current)) {
                                    host_->destroy_block(pos);
                                    current = host_->block_at(pos);
                                }
                                break;
                            }
                            // Placing a state over itself changes nothing and
                            // is not counted — which is why vanilla's
                            // `fill … air destroy` answers "No blocks were
                            // filled" after destroying every one of them.
                            if (target == current) {
                                continue;
                            }
                            changes.push_back(BlockChange{pos, target});
                        }
                    }
                }
                if (changes.empty()) {
                    return std::unexpected{error("commands.fill.failed")};
                }
                host_->set_blocks(changes);
                success(src,
                        Text::translatable("commands.fill.success",
                                           {raw(static_cast<i64>(changes.size()))}),
                        true);
                return static_cast<i32>(changes.size());
            };
        };
        const u32 node  = top("fill", kPermissionGameMaster);
        const u32 from  = d.argument(node, "from", ArgumentType::block_pos());
        const u32 to    = d.argument(from, "to", ArgumentType::block_pos());
        const u32 block = d.argument(to, "block", ArgumentType::block_state(), fill(Mode::Replace));
        const u32 replace = d.literal(block, "replace", fill(Mode::Replace));
        d.argument(replace, "filter", ArgumentType::block_predicate(), fill(Mode::Replace));
        d.literal(block, "keep", fill(Mode::Keep));
        d.literal(block, "outline", fill(Mode::Outline));
        d.literal(block, "hollow", fill(Mode::Hollow));
        d.literal(block, "destroy", fill(Mode::Destroy));
    }

    // ── gamemode ────────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            const u8 mode = ctx.find<GameModeArg>("gamemode")->mode;
            std::vector<PlayerRef*> targets;
            if (ctx.has("target")) {
                auto found = players(ctx, "target");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                targets = std::move(*found);
            } else {
                auto self = source_player(ctx.source());
                if (!self) {
                    return std::unexpected{self.error()};
                }
                targets.push_back(*self);
            }
            i32 changed = 0;
            for (PlayerRef* p : targets) {
                if (*p->game_mode == mode) {
                    continue;
                }
                set_game_mode(*p, mode);
                ++changed;
                const CommandSource& src = ctx.source();
                if (src.is_player() && src.entity_id == p->entity_id) {
                    success(src, Text::translatable("commands.gamemode.success.self", {game_mode_text(mode)}),
                            true);
                } else {
                    if (world_.rules.flag("sendCommandFeedback")) {
                        p->send(net::clientbound::kSystemChat,
                                net::encode_system_chat(
                                    to_json(Text::translatable("gameMode.changed", {game_mode_text(mode)})),
                                    false));
                    }
                    success(src,
                            Text::translatable("commands.gamemode.success.other",
                                               {player_display_name(p->name, p->uuid), game_mode_text(mode)}),
                            true);
                }
            }
            return changed;
        };
        const u32 node = top("gamemode", kPermissionGameMaster);
        const u32 mode = d.argument(node, "gamemode", ArgumentType::game_mode(), run);
        d.argument(mode, "target", ArgumentType::entity(false, true), run);
    }

    // ── gamerule ────────────────────────────────────────────────────────────
    {
        const u32 node = top("gamerule", kPermissionGameMaster);
        for (usize index = 0; index < kGameRules.size(); ++index) {
            const GameRuleSpec& spec = kGameRules[index];
            const u32 rule = d.literal(node, std::string{spec.name},
                                       [this, index](const CommandContext& ctx) -> Parsed<i32> {
                                           success(ctx.source(),
                                                   Text::translatable(
                                                       "commands.gamerule.query",
                                                       {Text::raw(std::string{kGameRules[index].name}),
                                                        Text::raw(world_.rules.text(index))}),
                                                   false);
                                           return world_.rules.value(index);
                                       });
            d.argument(rule, "value", spec.integer ? ArgumentType::integer() : ArgumentType::boolean(),
                       [this, index](const CommandContext& ctx) -> Parsed<i32> {
                           const GameRuleSpec& rule = kGameRules[index];
                           const i32 value = rule.integer ? *ctx.find<i32>("value")
                                                          : (*ctx.find<bool>("value") ? 1 : 0);
                           world_.rules.set(index, value);
                           // The two rules a client has to be told about.
                           if (rule.name == "doImmediateRespawn") {
                               host_->broadcast(net::clientbound::kGameEvent,
                                                net::encode_game_event(11, value != 0 ? 1.0F : 0.0F));
                           } else if (rule.name == "reducedDebugInfo") {
                               for (PlayerRef& p : players_) {
                                   p.send(net::clientbound::kEntityEvent,
                                          net::encode_entity_event(p.entity_id, value != 0 ? 22 : 23));
                               }
                           }
                           success(ctx.source(),
                                   Text::translatable("commands.gamerule.set",
                                                      {Text::raw(std::string{rule.name}),
                                                       Text::raw(world_.rules.text(index))}),
                                   true);
                           return value;
                       });
        }
    }

    // ── give ────────────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            auto targets = players(ctx, "targets");
            if (!targets) {
                return std::unexpected{targets.error()};
            }
            const ItemArg& item  = *ctx.find<ItemArg>("item");
            const i32*     given = ctx.find<i32>("count");
            const i32      count = given != nullptr ? *given : 1;
            const i8 max_stack   = env_.registries() != nullptr
                                       ? env_.registries()->max_stack_size(item.item)
                                       : i8{64};
            // A damageable item is created with `Damage:0`, which is why the
            // hover of a given sword reads `{Damage:0}` in the capture.
            std::optional<nbt::Tag> tag = item.nbt;
            if (gameplay::max_damage(item.id).has_value()) {
                if (!tag) {
                    tag = nbt::Tag::make_compound();
                }
                if (tag->find("Damage") == nullptr) {
                    tag->put("Damage", nbt::Tag{i32{0}});
                }
            }
            const nbt::Tag* tag_ptr = tag ? &*tag : nullptr;
            const i32       limit   = static_cast<i32>(max_stack) * 100;
            if (count > limit) {
                return std::unexpected{error(
                    "commands.give.failed.toomanyitems",
                    {raw(limit), item_display(item.id, count, tag_ptr, env_, lang())})};
            }
            const std::vector<u8> bytes = tag ? tag_bytes(*tag) : std::vector<u8>{};
            for (PlayerRef* p : *targets) {
                i32 remaining = count;
                while (remaining > 0) {
                    const i32 n = std::min<i32>(max_stack, remaining);
                    remaining -= n;
                    net::ItemStack stack{item.item, static_cast<i8>(n), bytes};
                    const i32      left = add_to_inventory(*p, stack, max_stack);
                    if (left > 0) {
                        stack.count = static_cast<i8>(left);
                        host_->drop_item(p->entity_id, stack);
                    }
                }
            }
            const Text shown = item_display(item.id, count, tag_ptr, env_, lang());
            if (targets->size() == 1) {
                success(ctx.source(),
                        Text::translatable("commands.give.success.single",
                                           {raw(count), shown,
                                            player_display_name((*targets)[0]->name, (*targets)[0]->uuid)}),
                        true);
            } else {
                success(ctx.source(),
                        Text::translatable("commands.give.success.multiple",
                                           {raw(count), shown, raw(static_cast<i64>(targets->size()))}),
                        true);
            }
            return static_cast<i32>(targets->size());
        };
        const u32 node    = top("give", kPermissionGameMaster);
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, true));
        const u32 item    = d.argument(targets, "item", ArgumentType::item_stack(), run);
        d.argument(item, "count", ArgumentType::integer_at_least(1), run);
    }

    // ── help ────────────────────────────────────────────────────────────────
    {
        const u32 node = top("help", kPermissionAll, [this](const CommandContext& ctx) -> Parsed<i32> {
            const auto usages = dispatcher_.smart_usage(dispatcher_.root(), ctx.source());
            for (const std::string& usage : usages) {
                success(ctx.source(), Text::literal("/" + usage), false);
            }
            return static_cast<i32>(usages.size());
        });
        d.argument(node, "command", ArgumentType::greedy(),
                   [this](const CommandContext& ctx) -> Parsed<i32> {
                       const std::string& text   = *ctx.find<std::string>("command");
                       const ParseResults parsed = dispatcher_.parse(text, 0, ctx.source());
                       if (parsed.context.nodes.empty()) {
                           return std::unexpected{error("commands.help.failed")};
                       }
                       const auto usages =
                           dispatcher_.smart_usage(parsed.context.nodes.back().node, ctx.source());
                       for (const std::string& usage : usages) {
                           success(ctx.source(), Text::literal("/" + text + " " + usage), false);
                       }
                       return static_cast<i32>(usages.size());
                   });
    }

    // ── kick ────────────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            auto targets = players(ctx, "targets");
            if (!targets) {
                return std::unexpected{targets.error()};
            }
            const MessageArg* message = ctx.find<MessageArg>("reason");
            const Text        reason  = message != nullptr
                                            ? Text::literal(resolve(*message, ctx.source()))
                                            : Text::translatable("multiplayer.disconnect.kicked");
            const std::string json = to_json(reason);
            for (PlayerRef* p : *targets) {
                const Text who = player_display_name(p->name, p->uuid);
                host_->kick(p->entity_id, json);
                success(ctx.source(), Text::translatable("commands.kick.success", {who, reason}), true);
            }
            return static_cast<i32>(targets->size());
        };
        const u32 node    = top("kick", kPermissionAdmin);
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, true), run);
        d.argument(targets, "reason", ArgumentType::message(), run);
    }

    // ── kill ────────────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            std::vector<const EntityInfo*> targets;
            if (ctx.has("targets")) {
                auto found = entities(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                targets = std::move(*found);
            } else {
                const EntityInfo* self = nullptr;
                for (const EntityInfo& e : world_snapshot_) {
                    if (e.player && ctx.source().is_player() && e.id == ctx.source().entity_id) {
                        self = &e;
                    }
                }
                if (self == nullptr) {
                    return std::unexpected{error("permissions.requires.entity")};
                }
                targets.push_back(self);
            }
            for (const EntityInfo* e : targets) {
                if (e->player) {
                    if (PlayerRef* p = player(e->id); p != nullptr && p->survival != nullptr) {
                        SurvivalIo io;
                        io.send      = p->send;
                        io.broadcast = [this](i32 id, std::span<const u8> payload) {
                            host_->broadcast(id, payload);
                        };
                        // genericKill bypasses invulnerability and armour; the
                        // survival tick carries out the death on its next pass.
                        (void)p->survival->hurt(gameplay::DamageKind::GenericKill,
                                                std::numeric_limits<f32>::max(), io, p->entity_id);
                    }
                } else {
                    (void)host_->kill_entity(e->id);
                }
            }
            if (targets.size() == 1) {
                success(ctx.source(), Text::translatable("commands.kill.success.single", {display(*targets[0])}),
                        true);
            } else {
                success(ctx.source(),
                        Text::translatable("commands.kill.success.multiple",
                                           {raw(static_cast<i64>(targets.size()))}),
                        true);
            }
            return static_cast<i32>(targets.size());
        };
        const u32 node = top("kill", kPermissionGameMaster, run);
        d.argument(node, "targets", ArgumentType::entity(false, false), run);
    }

    // ── list ────────────────────────────────────────────────────────────────
    {
        const auto list = [this](bool uuids) {
            return [this, uuids](const CommandContext& ctx) -> Parsed<i32> {
                Text names = Text::literal("");
                for (usize i = 0; i < players_.size(); ++i) {
                    const PlayerRef& p = players_[i];
                    if (i > 0) {
                        names.append(Text::literal(", ").color("gray"));
                    }
                    names.append(uuids ? Text::translatable("commands.list.nameAndId",
                                                            {Text::literal(std::string{p.name}),
                                                             Text::raw(p.uuid.to_string())})
                                       : player_display_name(p.name, p.uuid));
                }
                if (names.extra.size() == 1) {
                    names = std::move(names.extra.front());
                }
                success(ctx.source(),
                        Text::translatable("commands.list.players",
                                           {raw(static_cast<i64>(players_.size())),
                                            raw(config_.max_players), std::move(names)}),
                        false);
                return static_cast<i32>(players_.size());
            };
        };
        const u32 node = top("list", kPermissionAll, list(false));
        d.literal(node, "uuids", list(true));
    }

    // ── msg / tell / w ──────────────────────────────────────────────────────
    {
        const u32 node    = top("msg", kPermissionAll);
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, true));
        d.argument(targets, "message", ArgumentType::message(),
                   [this](const CommandContext& ctx) -> Parsed<i32> {
                       auto found = players(ctx, "targets");
                       if (!found) {
                           return std::unexpected{found.error()};
                       }
                       const CommandSource& src  = ctx.source();
                       const std::string    text = resolve(*ctx.find<MessageArg>("message"), src);
                       for (PlayerRef* target : *found) {
                           const Text to = player_display_name(target->name, target->uuid);
                           if (src.is_player()) {
                               if (PlayerRef* self = player(src.entity_id)) {
                                   self->send(net::clientbound::kPlayerChat,
                                              chat_packet(src.name, src.uuid, text, timestamp_, salt_,
                                                          kChatTypeMsgOutgoing, to));
                               }
                               target->send(net::clientbound::kPlayerChat,
                                            chat_packet(src.name, src.uuid, text, timestamp_, salt_,
                                                        kChatTypeMsgIncoming));
                           } else {
                               if (console) {
                                   console("You whisper to " + std::string{target->name} + ": " + text);
                               }
                               target->send(net::clientbound::kDisguisedChat,
                                            net::encode_disguised_chat(
                                                {to_json(Text::literal(text)), kChatTypeMsgIncoming,
                                                 to_json(Text::literal("Server")), std::nullopt}));
                           }
                       }
                       return static_cast<i32>(found->size());
                   });
        d.redirect(top("tell", kPermissionAll), node);
        d.redirect(top("w", kPermissionAll), node);
    }

    // ── say ─────────────────────────────────────────────────────────────────
    {
        const u32 node = top("say", kPermissionGameMaster);
        d.argument(node, "message", ArgumentType::message(),
                   [this](const CommandContext& ctx) -> Parsed<i32> {
                       const CommandSource& src  = ctx.source();
                       const std::string    text = resolve(*ctx.find<MessageArg>("message"), src);
                       if (src.is_player()) {
                           host_->broadcast(net::clientbound::kPlayerChat,
                                            chat_packet(src.name, src.uuid, text, timestamp_, salt_,
                                                        kChatTypeSay));
                           if (console) {
                               console("[Not Secure] [" + src.name + "] " + text);
                           }
                       } else {
                           host_->broadcast(net::clientbound::kDisguisedChat,
                                            net::encode_disguised_chat(
                                                {to_json(Text::literal(text)), kChatTypeSay,
                                                 to_json(Text::literal("Server")), std::nullopt}));
                           if (console) {
                               console("[Server] " + text);
                           }
                       }
                       return 1;
                   });
    }

    // ── seed ────────────────────────────────────────────────────────────────
    top("seed", kPermissionGameMaster, [this](const CommandContext& ctx) -> Parsed<i32> {
        success(ctx.source(),
                Text::translatable("commands.seed.success", {copy_on_click(std::to_string(world_.seed))}),
                false);
        return static_cast<i32>(world_.seed);
    });

    // ── setblock ────────────────────────────────────────────────────────────
    {
        enum class Mode : u8 { Replace, Keep, Destroy };
        const auto setblock = [this](Mode mode) {
            return [this, mode](const CommandContext& ctx) -> Parsed<i32> {
                const BlockPos pos = ctx.find<Coordinates>("pos")->block(ctx.source());
                if (!host_->is_loaded(pos.x >> 4, pos.z >> 4)) {
                    return std::unexpected{error("argument.pos.unloaded")};
                }
                if (!in_world(pos)) {
                    return std::unexpected{error("argument.pos.outofworld")};
                }
                const BlockStateArg& block = *ctx.find<BlockStateArg>("block");
                if (block.nbt && !block.nbt->empty()) {
                    return std::unexpected{not_modelled("apply block entity NBT from a command")};
                }
                registry::BlockStateId current = host_->block_at(pos);
                const bool target_air = env_.blocks()->is_air(block.block);
                if (mode == Mode::Keep && !env_.blocks()->is_air(env_.blocks()->block_of(current))) {
                    return std::unexpected{error("commands.setblock.failed")};
                }
                if (mode == Mode::Destroy) {
                    host_->destroy_block(pos);
                    current = host_->block_at(pos);
                    if (target_air) {
                        // Destroyed, and air was asked for: done, and a success
                        // — the capture's `setblock … air destroy` says so.
                        success(ctx.source(),
                                Text::translatable("commands.setblock.success",
                                                   {raw(pos.x), raw(pos.y), raw(pos.z)}),
                                true);
                        return 1;
                    }
                }
                if (current == block.state) {
                    return std::unexpected{error("commands.setblock.failed")};
                }
                host_->set_block(pos, block.state);
                success(ctx.source(),
                        Text::translatable("commands.setblock.success",
                                           {raw(pos.x), raw(pos.y), raw(pos.z)}),
                        true);
                return 1;
            };
        };
        const u32 node  = top("setblock", kPermissionGameMaster);
        const u32 pos   = d.argument(node, "pos", ArgumentType::block_pos());
        const u32 block = d.argument(pos, "block", ArgumentType::block_state(), setblock(Mode::Replace));
        d.literal(block, "destroy", setblock(Mode::Destroy));
        d.literal(block, "keep", setblock(Mode::Keep));
        d.literal(block, "replace", setblock(Mode::Replace));
    }

    // ── spawnpoint ──────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            const CommandSource&    src = ctx.source();
            std::vector<PlayerRef*> targets;
            if (ctx.has("targets")) {
                auto found = players(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                targets = std::move(*found);
            } else {
                auto self = source_player(src);
                if (!self) {
                    return std::unexpected{self.error()};
                }
                targets.push_back(*self);
            }
            const Coordinates* coords = ctx.find<Coordinates>("pos");
            const BlockPos     pos    = coords != nullptr
                                            ? coords->block(src)
                                            : BlockPos{static_cast<i32>(std::floor(src.position.x)),
                                                       static_cast<i32>(std::floor(src.position.y)),
                                                       static_cast<i32>(std::floor(src.position.z))};
            const AngleArg* angle_arg = ctx.find<AngleArg>("angle");
            const f32       angle     = angle_arg != nullptr ? angle_arg->resolve(src) : 0.0F;
            for (PlayerRef* p : targets) {
                spawns_[p->uuid.to_string()] = PersonalSpawn{pos.x, pos.y, pos.z, angle};
            }
            std::vector<Text> with{raw(pos.x), raw(pos.y), raw(pos.z),
                                   Text::raw(java_float_string(angle)), Text::raw("minecraft:overworld")};
            if (targets.size() == 1) {
                with.push_back(player_display_name(targets[0]->name, targets[0]->uuid));
                success(src, Text::translatable("commands.spawnpoint.success.single", std::move(with)), true);
            } else {
                with.push_back(raw(static_cast<i64>(targets.size())));
                success(src, Text::translatable("commands.spawnpoint.success.multiple", std::move(with)), true);
            }
            return static_cast<i32>(targets.size());
        };
        const u32 node    = top("spawnpoint", kPermissionGameMaster, run);
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, true), run);
        const u32 pos     = d.argument(targets, "pos", ArgumentType::block_pos(), run);
        d.argument(pos, "angle", ArgumentType::angle(), run);
    }

    // ── setworldspawn ───────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            const CommandSource& src    = ctx.source();
            const Coordinates*   coords = ctx.find<Coordinates>("pos");
            const BlockPos pos = coords != nullptr ? coords->block(src)
                                                   : BlockPos{static_cast<i32>(std::floor(src.position.x)),
                                                              static_cast<i32>(std::floor(src.position.y)),
                                                              static_cast<i32>(std::floor(src.position.z))};
            const AngleArg* angle_arg = ctx.find<AngleArg>("angle");
            const f32       angle     = angle_arg != nullptr ? angle_arg->resolve(src) : 0.0F;
            host_->set_world_spawn(pos.x, pos.y, pos.z, angle);
            success(src,
                    Text::translatable("commands.setworldspawn.success",
                                       {raw(pos.x), raw(pos.y), raw(pos.z),
                                        Text::raw(java_float_string(angle))}),
                    true);
            return 1;
        };
        const u32 node = top("setworldspawn", kPermissionGameMaster, run);
        const u32 pos  = d.argument(node, "pos", ArgumentType::block_pos(), run);
        d.argument(pos, "angle", ArgumentType::angle(), run);
    }

    // ── summon ──────────────────────────────────────────────────────────────
    {
        const Executor run = [this](const CommandContext& ctx) -> Parsed<i32> {
            const CommandSource& src    = ctx.source();
            const std::string&   type   = ctx.find<ResourceArg>("entity")->id;
            const Coordinates*   coords = ctx.find<Coordinates>("pos");
            const Vec3d          pos    = coords != nullptr ? coords->position(src) : src.position;
            if (!spawnable(pos)) {
                return std::unexpected{error("commands.summon.invalidPosition")};
            }
            if (!env_.has_entity_type(type)) {
                return std::unexpected{error("argument.resource.not_found",
                                             {Text::raw(type), Text::raw("minecraft:entity_type")})};
            }
            if (const nbt::Tag* tag = ctx.find<nbt::Tag>("nbt"); tag != nullptr && !tag->empty()) {
                return std::unexpected{not_modelled("apply summon NBT")};
            }
            const auto summoned = host_->summon(type, pos);
            if (!summoned) {
                return std::unexpected{error("commands.summon.failed")};
            }
            success(src, Text::translatable("commands.summon.success", {display(*summoned)}), true);
            return 1;
        };
        const u32 node   = top("summon", kPermissionGameMaster);
        const u32 entity = d.argument(node, "entity", ArgumentType::resource("minecraft:entity_type"), run);
        d.suggests(entity, {}, "minecraft:summonable_entities");
        const u32 pos = d.argument(entity, "pos", ArgumentType::vec3(), run);
        d.argument(pos, "nbt", ArgumentType::nbt_compound(), run);
    }

    // ── teleport / tp ───────────────────────────────────────────────────────
    {
        // Teleport `targets` to a position; the message is the position the
        // source resolved, once, as vanilla's is.
        const auto to_location = [this](const CommandContext& ctx, bool self) -> Parsed<i32> {
            const CommandSource& src = ctx.source();
            std::vector<const EntityInfo*> targets;
            if (self) {
                for (const EntityInfo& e : world_snapshot_) {
                    if (e.player && src.is_player() && e.id == src.entity_id) {
                        targets.push_back(&e);
                    }
                }
                if (targets.empty()) {
                    return std::unexpected{error("permissions.requires.entity")};
                }
            } else {
                auto found = entities(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                targets = std::move(*found);
            }
            const Coordinates& coords = *ctx.find<Coordinates>("location");
            const Vec3d        pos    = coords.position(src);
            if (!spawnable(pos)) {
                return std::unexpected{error("commands.teleport.invalidPosition")};
            }
            u8 flags = 0;
            if (coords.local || coords.axes[0].relative) {
                flags |= teleport_flags::kX;
            }
            if (coords.local || coords.axes[1].relative) {
                flags |= teleport_flags::kY;
            }
            if (coords.local || coords.axes[2].relative) {
                flags |= teleport_flags::kZ;
            }
            const RotationArg* rotation = ctx.find<RotationArg>("rotation");
            const Coordinates* facing   = ctx.find<Coordinates>("facingLocation");
            const EntitySelector* facing_entity = ctx.find<EntitySelector>("facingEntity");
            for (const EntityInfo* e : targets) {
                f32 yaw   = e->yaw;
                f32 pitch = e->pitch;
                u8  these = flags;
                if (rotation != nullptr) {
                    yaw   = static_cast<f32>(rotation->axes[0].relative
                                                 ? static_cast<f64>(src.yaw) + rotation->axes[0].value
                                                 : rotation->axes[0].value);
                    pitch = static_cast<f32>(rotation->axes[1].relative
                                                 ? static_cast<f64>(src.pitch) + rotation->axes[1].value
                                                 : rotation->axes[1].value);
                    if (rotation->axes[0].relative) {
                        these |= teleport_flags::kYaw;
                    }
                    if (rotation->axes[1].relative) {
                        these |= teleport_flags::kPitch;
                    }
                } else {
                    these |= teleport_flags::kYaw | teleport_flags::kPitch;
                }
                if (e->player) {
                    host_->teleport_player(e->id, pos, yaw, pitch, these);
                } else {
                    (void)host_->teleport_entity(e->id, pos, yaw, pitch);
                }
                if (e->player && (facing != nullptr || facing_entity != nullptr)) {
                    Vec3d at{};
                    if (facing != nullptr) {
                        at = facing->position(src);
                    } else {
                        auto seen = find_entities(
                            *facing_entity, src, world_snapshot_,
                            [this](u32 n) { return static_cast<u32>(random_.next_int(static_cast<i32>(n))); },
                            &env_);
                        if (!seen || seen->empty()) {
                            return std::unexpected{error("argument.entity.notfound.entity")};
                        }
                        const EntityInfo& target = *seen->front();
                        const AnchorArg*  anchor = ctx.find<AnchorArg>("facingAnchor");
                        at = target.position;
                        if (anchor != nullptr && anchor->eyes) {
                            at.y += static_cast<f64>(target.eye_height);
                        }
                    }
                    if (PlayerRef* p = player(e->id)) {
                        p->send(net::clientbound::kLookAt, net::encode_look_at(false, at.x, at.y, at.z));
                    }
                }
            }
            const Text x = Text::raw(java_fixed6(pos.x));
            const Text y = Text::raw(java_fixed6(pos.y));
            const Text z = Text::raw(java_fixed6(pos.z));
            if (targets.size() == 1) {
                success(src,
                        Text::translatable("commands.teleport.success.location.single",
                                           {display(*targets[0]), x, y, z}),
                        true);
            } else {
                success(src,
                        Text::translatable("commands.teleport.success.location.multiple",
                                           {raw(static_cast<i64>(targets.size())), x, y, z}),
                        true);
            }
            return static_cast<i32>(targets.size());
        };
        const auto to_entity = [this](const CommandContext& ctx, bool self) -> Parsed<i32> {
            const CommandSource& src = ctx.source();
            std::vector<const EntityInfo*> targets;
            if (self) {
                for (const EntityInfo& e : world_snapshot_) {
                    if (e.player && src.is_player() && e.id == src.entity_id) {
                        targets.push_back(&e);
                    }
                }
                if (targets.empty()) {
                    return std::unexpected{error("permissions.requires.entity")};
                }
            } else {
                auto found = entities(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                targets = std::move(*found);
            }
            auto destination = entities(ctx, "destination");
            if (!destination) {
                return std::unexpected{destination.error()};
            }
            const EntityInfo& to = *destination->front();
            for (const EntityInfo* e : targets) {
                if (e->player) {
                    host_->teleport_player(e->id, to.position, to.yaw, to.pitch, 0);
                } else {
                    (void)host_->teleport_entity(e->id, to.position, to.yaw, to.pitch);
                }
            }
            if (targets.size() == 1) {
                success(src,
                        Text::translatable("commands.teleport.success.entity.single",
                                           {display(*targets[0]), display(to)}),
                        true);
            } else {
                success(src,
                        Text::translatable("commands.teleport.success.entity.multiple",
                                           {raw(static_cast<i64>(targets.size())), display(to)}),
                        true);
            }
            return static_cast<i32>(targets.size());
        };
        const auto location = [to_location](bool self) {
            return [to_location, self](const CommandContext& ctx) { return to_location(ctx, self); };
        };
        const auto entity = [to_entity](bool self) {
            return [to_entity, self](const CommandContext& ctx) { return to_entity(ctx, self); };
        };

        const u32 node = top("teleport", kPermissionGameMaster);
        d.argument(node, "location", ArgumentType::vec3(), location(true));
        d.argument(node, "destination", ArgumentType::entity(true, false), entity(true));
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, false));
        const u32 at      = d.argument(targets, "location", ArgumentType::vec3(), location(false));
        d.argument(at, "rotation", ArgumentType::rotation(), location(false));
        const u32 facing        = d.literal(at, "facing");
        const u32 facing_entity = d.literal(facing, "entity");
        const u32 who = d.argument(facing_entity, "facingEntity", ArgumentType::entity(true, false),
                                   location(false));
        d.argument(who, "facingAnchor", ArgumentType::entity_anchor(), location(false));
        d.argument(facing, "facingLocation", ArgumentType::vec3(), location(false));
        d.argument(targets, "destination", ArgumentType::entity(true, false), entity(false));

        d.redirect(top("tp", kPermissionGameMaster), node);
    }

    // ── tellraw ─────────────────────────────────────────────────────────────
    {
        const u32 node    = top("tellraw", kPermissionGameMaster);
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, true));
        d.argument(targets, "message", ArgumentType::component(),
                   [this](const CommandContext& ctx) -> Parsed<i32> {
                       auto found = players(ctx, "targets");
                       if (!found) {
                           return std::unexpected{found.error()};
                       }
                       const Text& message = *ctx.find<Text>("message");
                       for (PlayerRef* p : *found) {
                           p->send(net::clientbound::kSystemChat,
                                   net::encode_system_chat(to_json(resolve(message, ctx.source())), false));
                       }
                       return static_cast<i32>(found->size());
                   });
    }

    // ── time ────────────────────────────────────────────────────────────────
    {
        const u32  node = top("time", kPermissionGameMaster);
        const auto set_to = [this](i64 time) {
            return [this, time](const CommandContext& ctx) -> Parsed<i32> {
                world_.day_time = time;
                success(ctx.source(), Text::translatable("commands.time.set", {raw(time)}), true);
                return static_cast<i32>(world_.day_time % 24000);
            };
        };
        const u32 set = d.literal(node, "set");
        d.literal(set, "day", set_to(1000));
        d.literal(set, "noon", set_to(6000));
        d.literal(set, "night", set_to(13000));
        d.literal(set, "midnight", set_to(18000));
        d.argument(set, "time", ArgumentType::time(0), [this](const CommandContext& ctx) -> Parsed<i32> {
            const i32 time  = ctx.find<TimeArg>("time")->ticks;
            world_.day_time = time;
            success(ctx.source(), Text::translatable("commands.time.set", {raw(time)}), true);
            return static_cast<i32>(world_.day_time % 24000);
        });
        const u32 add = d.literal(node, "add");
        d.argument(add, "time", ArgumentType::time(0), [this](const CommandContext& ctx) -> Parsed<i32> {
            world_.day_time += ctx.find<TimeArg>("time")->ticks;
            const i64 shown = world_.day_time % 24000;
            success(ctx.source(), Text::translatable("commands.time.set", {raw(shown)}), true);
            return static_cast<i32>(shown);
        });
        const auto query = [this](i32 which) {
            return [this, which](const CommandContext& ctx) -> Parsed<i32> {
                constexpr i64 kIntMax = std::numeric_limits<i32>::max();
                const i64     value   = which == 0   ? world_.day_time % 24000
                                        : which == 1 ? world_.game_time % kIntMax
                                                     : (world_.day_time / 24000) % kIntMax;
                success(ctx.source(), Text::translatable("commands.time.query", {raw(value)}), false);
                return static_cast<i32>(value);
            };
        };
        const u32 q = d.literal(node, "query");
        d.literal(q, "daytime", query(0));
        d.literal(q, "gametime", query(1));
        d.literal(q, "day", query(2));
    }

    // ── title ───────────────────────────────────────────────────────────────
    {
        const auto each = [this](std::string key,
                                 std::function<void(PlayerRef&, const CommandContext&)> act) {
            return [this, key, act](const CommandContext& ctx) -> Parsed<i32> {
                auto found = players(ctx, "targets");
                if (!found) {
                    return std::unexpected{found.error()};
                }
                for (PlayerRef* p : *found) {
                    act(*p, ctx);
                }
                if (found->size() == 1) {
                    success(ctx.source(),
                            Text::translatable(key + ".single",
                                               {player_display_name((*found)[0]->name, (*found)[0]->uuid)}),
                            true);
                } else {
                    success(ctx.source(),
                            Text::translatable(key + ".multiple", {raw(static_cast<i64>(found->size()))}),
                            true);
                }
                return static_cast<i32>(found->size());
            };
        };
        const auto show = [this](i32 packet) {
            return [this, packet](PlayerRef& p, const CommandContext& ctx) {
                const Text& title = *ctx.find<Text>("title");
                p.send(packet, net::encode_component_packet(to_json(resolve(title, ctx.source()))));
            };
        };
        const u32 node    = top("title", kPermissionGameMaster);
        const u32 targets = d.argument(node, "targets", ArgumentType::entity(false, true));
        d.literal(targets, "clear", each("commands.title.cleared", [](PlayerRef& p, const CommandContext&) {
                      p.send(net::clientbound::kClearTitles, net::encode_clear_titles(false));
                  }));
        d.literal(targets, "reset", each("commands.title.reset", [](PlayerRef& p, const CommandContext&) {
                      p.send(net::clientbound::kClearTitles, net::encode_clear_titles(true));
                  }));
        d.argument(d.literal(targets, "title"), "title", ArgumentType::component(),
                   each("commands.title.show.title", show(net::clientbound::kSetTitleText)));
        d.argument(d.literal(targets, "subtitle"), "title", ArgumentType::component(),
                   each("commands.title.show.subtitle", show(net::clientbound::kSetSubtitleText)));
        d.argument(d.literal(targets, "actionbar"), "title", ArgumentType::component(),
                   each("commands.title.show.actionbar", show(net::clientbound::kSetActionBarText)));
        const u32 times    = d.literal(targets, "times");
        const u32 fade_in  = d.argument(times, "fadeIn", ArgumentType::time(0));
        const u32 stay     = d.argument(fade_in, "stay", ArgumentType::time(0));
        d.argument(stay, "fadeOut", ArgumentType::time(0),
                   each("commands.title.times", [](PlayerRef& p, const CommandContext& ctx) {
                       p.send(net::clientbound::kSetTitleAnimationTimes,
                              net::encode_title_animation_times(ctx.find<TimeArg>("fadeIn")->ticks,
                                                                ctx.find<TimeArg>("stay")->ticks,
                                                                ctx.find<TimeArg>("fadeOut")->ticks));
                   }));
    }

    // ── weather ─────────────────────────────────────────────────────────────
    {
        enum class Kind : u8 { Clear, Rain, Thunder };
        const auto weather = [this](Kind kind) {
            return [this, kind](const CommandContext& ctx) -> Parsed<i32> {
                const TimeArg* given = ctx.find<TimeArg>("duration");
                i32            duration = 0;
                if (given != nullptr) {
                    duration = given->ticks;
                } else if (kind == Kind::Clear) {
                    duration = world_.random_between(kRainDelayMin, kRainDelayMax);
                } else if (kind == Kind::Rain) {
                    duration = world_.random_between(kRainDurationMin, kRainDurationMax);
                } else {
                    duration = world_.random_between(kThunderDurationMin, kThunderDurationMax);
                }
                switch (kind) {
                case Kind::Clear:
                    world_.set_weather(duration, 0, false, false);
                    success(ctx.source(), Text::translatable("commands.weather.set.clear"), true);
                    break;
                case Kind::Rain:
                    world_.set_weather(0, duration, true, false);
                    success(ctx.source(), Text::translatable("commands.weather.set.rain"), true);
                    break;
                case Kind::Thunder:
                    world_.set_weather(0, duration, true, true);
                    success(ctx.source(), Text::translatable("commands.weather.set.thunder"), true);
                    break;
                }
                return duration;
            };
        };
        const u32 node = top("weather", kPermissionGameMaster);
        for (const auto& [name, kind] :
             std::array<std::pair<std::string_view, Kind>, 3>{
                 {{"clear", Kind::Clear}, {"rain", Kind::Rain}, {"thunder", Kind::Thunder}}}) {
            const u32 branch = d.literal(node, std::string{name}, weather(kind));
            d.argument(branch, "duration", ArgumentType::time(1), weather(kind));
        }
    }

    // ── deop / op ───────────────────────────────────────────────────────────
    {
        // The profiles a `targets` argument names: a selector's players, or a
        // name — the online player's own if one matches, else the offline uuid
        // of the name, which is what an offline vanilla server resolves it to.
        const auto profiles = [this](const CommandContext& ctx)
            -> Parsed<std::vector<std::pair<std::string, net::Uuid>>> {
            const GameProfileArg& arg = *ctx.find<GameProfileArg>("targets");
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
                if (p.name.size() == arg.name.size() &&
                    std::ranges::equal(p.name, arg.name, [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                    })) {
                    out.emplace_back(std::string{p.name}, p.uuid);
                    return out;
                }
            }
            out.emplace_back(arg.name, net::Uuid::offline_player(arg.name));
            return out;
        };
        const auto change = [this, profiles](bool grant) {
            return [this, profiles, grant](const CommandContext& ctx) -> Parsed<i32> {
                auto found = profiles(ctx);
                if (!found) {
                    return std::unexpected{found.error()};
                }
                i32 changed = 0;
                for (const auto& [name, uuid] : *found) {
                    const bool done = grant ? ops_.add(OpEntry{uuid, name, kPermissionOwner, false})
                                            : ops_.remove(uuid);
                    if (!done) {
                        continue;
                    }
                    ++changed;
                    for (PlayerRef& p : players_) {
                        if (p.uuid == uuid && !config_.integrated) {
                            set_permission(p, grant ? kPermissionOwner : 0);
                        }
                    }
                    success(ctx.source(),
                            Text::translatable(grant ? "commands.op.success" : "commands.deop.success",
                                               {Text::raw(name)}),
                            true);
                }
                if (changed == 0) {
                    return std::unexpected{error(grant ? "commands.op.failed" : "commands.deop.failed")};
                }
                (void)ops_.save();
                return changed;
            };
        };
        const auto names_where = [this](bool op) {
            return [this, op](const CommandContext&, SuggestionsBuilder& builder) {
                for (const PlayerRef& p : players_) {
                    if (ops_.level_of(p.uuid).has_value() == op &&
                        std::string_view{p.name}.starts_with(builder.remaining())) {
                        builder.suggest(std::string{p.name});
                    }
                }
            };
        };
        const u32 deop = top("deop", kPermissionAdmin);
        d.suggests(d.argument(deop, "targets", ArgumentType::game_profile(), change(false)),
                   names_where(true), "minecraft:ask_server");
        const u32 op = top("op", kPermissionAdmin);
        d.suggests(d.argument(op, "targets", ArgumentType::game_profile(), change(true)),
                   names_where(false), "minecraft:ask_server");
    }

    // ── save-all / stop ─────────────────────────────────────────────────────
    {
        const Executor save = [this](const CommandContext& ctx) -> Parsed<i32> {
            success(ctx.source(), Text::translatable("commands.save.saving"), false);
            host_->save();
            success(ctx.source(), Text::translatable("commands.save.success"), true);
            return 0;
        };
        const u32 node = top("save-all", kPermissionOwner, save);
        d.literal(node, "flush", save);
        top("stop", kPermissionOwner, [this](const CommandContext& ctx) -> Parsed<i32> {
            success(ctx.source(), Text::translatable("commands.stop.stopping"), true);
            host_->stop();
            return 1;
        });
    }
}

}  // namespace ov::server::cmd
