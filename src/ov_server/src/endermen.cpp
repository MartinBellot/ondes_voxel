// ── mobs-5 ── See endermen.hpp.
#include "endermen.hpp"

#include "entity_nbt.hpp"

#include "ov/gameplay/enderman.hpp"
#include "ov/gameplay/goals.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/play.hpp"

#include <cmath>
#include <string_view>

namespace ov::server {
namespace {

[[nodiscard]] BlockPos block_of(const Vec3d& v) noexcept {
    return BlockPos{static_cast<i32>(std::floor(v.x)), static_cast<i32>(std::floor(v.y)),
                    static_cast<i32>(std::floor(v.z))};
}

}  // namespace

Endermen::Endermen(const registry::Registries& registries, const registry::BlockRegistry& blocks,
                   MobCombat* combat)
    : blocks_{&blocks},
      combat_{combat},
      pick_chance_{gameplay::kPickChance},
      place_chance_{gameplay::kPlaceChance} {
    if (const auto types = registries.find("minecraft:entity_type")) {
        enderman_type_ = registries.protocol_id(*types, "minecraft:enderman").value_or(-1);
    }
    if (const auto air = blocks.find_block("minecraft:air")) {
        air_ = blocks.default_state(*air);
    }
    // `#enderman_holdable` by block id, once: the tag holds wire ids, the
    // chunks hold states, and a name lookup per tick per enderman is waste.
    if (const auto block_registry = registries.find("minecraft:block")) {
        if (const auto tag = registries.find_tag(*block_registry, "minecraft:enderman_holdable")) {
            for (const auto member : registries.tag_members(*tag)) {
                const std::string_view name = registries.entry_of(*block_registry, member);
                if (const auto block = blocks.find_block(name)) {
                    if (holdable_.size() <= block->value()) {
                        holdable_.resize(static_cast<usize>(block->value()) + 1, false);
                    }
                    holdable_[block->value()] = true;
                }
            }
        }
    }
    edits_.reserve(16);
    hurts_.reserve(16);
}

bool Endermen::holdable(registry::BlockStateId state) const noexcept {
    const auto block = blocks_->block_of(state);
    return block.value() < holdable_.size() && holdable_[block.value()];
}

void Endermen::anger(Row& row, i32 player, i64 tick) {
    row.target      = player;
    row.anger_until = tick + gameplay::draw_anger_ticks(random_);
    row.screaming   = true;
    row.stared      = true;
}

bool Endermen::wet(const entity::EntityState& state, const EndermenHost& host) const {
    const BlockPos feet = block_of(state.position);
    const BlockPos eyes = block_of(Vec3d{state.position.x,
                                         state.position.y + static_cast<f64>(state.eye_height),
                                         state.position.z});
    const auto in_water = [&](BlockPos at) {
        if (!host.block_at) {
            return false;
        }
        const registry::BlockStateId here = host.block_at(at);
        return blocks_->holds_fluid(here) &&
               blocks_->block_name(blocks_->block_of(here)) != "minecraft:lava";
    };
    const auto rained_on = [&](BlockPos at) { return host.raining_at && host.raining_at(at); };
    return in_water(feet) || in_water(eyes) || rained_on(feet) || rained_on(eyes);
}

void Endermen::teleport(entity::EntityWorld& world, entity::EntityHandle handle,
                        entity::EntityState& state, const gameplay::CollisionWorld& collisions,
                        i32 min_y) {
    const auto landing = gameplay::random_teleport(collisions, state.position,
                                                   static_cast<f64>(state.width),
                                                   static_cast<f64>(state.height), min_y, random_);
    if (!landing) {
        return;
    }
    state.position = *landing;
    state.velocity = Vec3d{};
    // The route it was following starts where it no longer is.
    if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle)); mob != nullptr) {
        mob->mutable_brain().follower.clear();
    }
}

bool Endermen::edited(BlockPos pos) const noexcept {
    for (const EndermanBlockEdit& edit : edits_) {
        if (edit.pos.x == pos.x && edit.pos.y == pos.y && edit.pos.z == pos.z) {
            return true;
        }
    }
    return false;
}

void Endermen::carry(Row& row, const entity::EntityState& state, const EndermenHost& host) {
    if (!host.block_at) {
        return;
    }
    if (!row.has_carried) {
        if (random_.next_float() >= pick_chance_) {
            return;
        }
        const BlockPos               at   = gameplay::pick_target(state.position, random_);
        const registry::BlockStateId here = host.block_at(at);
        if (!holdable(here) || edited(at)) {
            return;
        }
        row.carried     = here;
        row.has_carried = true;
        edits_.push_back(EndermanBlockEdit{at, air_});
        return;
    }
    if (random_.next_float() >= place_chance_) {
        return;
    }
    const BlockPos               at    = gameplay::place_target(state.position, random_);
    const registry::BlockStateId here  = host.block_at(at);
    const registry::BlockStateId below = host.block_at(BlockPos{at.x, at.y - 1, at.z});
    if (!blocks_->is_air(blocks_->block_of(here)) || blocks_->is_air(blocks_->block_of(below)) ||
        !blocks_->face_is_sturdy(below, registry::BlockRegistry::Face::Up) || edited(at)) {
        return;
    }
    edits_.push_back(EndermanBlockEdit{at, row.carried});
    row.has_carried = false;
    row.carried     = {};
}

void Endermen::flush(i32 id, Row& row, const EndermenHost& host) const {
    net::MetadataWriter fields;
    if (!row.sent_valid || row.has_carried != row.sent_has_carried ||
        (row.has_carried && row.carried != row.sent_carried)) {
        fields.optional_block_state_value(
            gameplay::kCarriedBlockIndex,
            row.has_carried ? static_cast<i32>(row.carried.value()) : 0);
    }
    if (row.screaming != row.sent_screaming) {
        fields.boolean_value(gameplay::kScreamingIndex, row.screaming);
    }
    if (row.stared != row.sent_stared) {
        fields.boolean_value(gameplay::kStaredAtIndex, row.stared);
    }
    // The spawn already told the clients what a fresh row holds.
    if (!row.sent_valid && !row.has_carried && !row.screaming && !row.stared) {
        fields = net::MetadataWriter{};
    }
    row.sent_valid       = true;
    row.sent_has_carried = row.has_carried;
    row.sent_carried     = row.carried;
    row.sent_screaming   = row.screaming;
    row.sent_stared      = row.stared;
    if (!fields.empty() && host.broadcast) {
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(id, fields.take()));
    }
}

void Endermen::tick(entity::EntityWorld& world, const gameplay::CollisionWorld& collisions,
                    std::span<const EndermanWatcher> watchers, bool griefing, i64 tick, i32 min_y,
                    const EndermenHost& host) {
    if (enderman_type_ < 0) {
        return;
    }
    for (const entity::EntityHandle handle : world.handles()) {
        entity::EntityState* state = world.mutable_state(handle);
        if (state == nullptr || state->removed || state->health <= 0.0F || !owns(state->type)) {
            continue;
        }
        Row& row = rows_[state->network_id];

        // Wet, or hurt since the last tick: a teleport. Wet also hurts.
        bool jump = row.teleport_pending;
        row.teleport_pending = false;
        if (wet(*state, host)) {
            jump = true;
            if (combat_ != nullptr) {
                const MobHurt hit = combat_->hurt(*state, gameplay::DamageKind::Drown,
                                                  gameplay::kWetDamage, constants_);
                if (hit.applied) {
                    hurts_.push_back(MobEffectHurt{state->network_id, gameplay::DamageKind::Drown,
                                                   hit.killed});
                }
                if (hit.killed) {
                    continue;  // the death is the server's; a body does not teleport
                }
            }
        }
        if (jump) {
            teleport(world, handle, *state, collisions, min_y);
        }

        // Anger ends when its time is up.
        if (row.anger_until != 0 && tick >= row.anger_until) {
            row.anger_until = 0;
            row.target      = 0;
            row.screaming   = false;
            row.stared      = false;
        }

        // A stare, when not already angry.
        if (row.target == 0) {
            const Vec3d eyes{state->position.x,
                             state->position.y + static_cast<f64>(state->eye_height),
                             state->position.z};
            for (const EndermanWatcher& watcher : watchers) {
                if (watcher.masked ||
                    !gameplay::looks_at(watcher.eye, gameplay::view_vector(watcher.yaw, watcher.pitch),
                                        eyes) ||
                    !gameplay::has_clear_line(collisions, watcher.eye, eyes)) {
                    continue;
                }
                anger(row, watcher.player, tick);
                break;
            }
        }

        // The brain chases whoever angered it, for as long as the anger lasts.
        if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle)); mob != nullptr) {
            gameplay::MobBrain& brain = mob->mutable_brain();
            if (row.target != 0) {
                brain.target_player = row.target;
                brain.target        = entity::kNoEntity;
            } else if (brain.target_player != 0) {
                brain.target_player = 0;
            }
        }

        if (griefing) {
            carry(row, *state, host);
        }
        flush(state->network_id, row, host);
    }
}

void Endermen::on_hurt(i32 id, i32 attacker_player, i64 tick) {
    Row& row             = rows_[id];
    row.teleport_pending = true;
    if (attacker_player != 0) {
        anger(row, attacker_player, tick);
    }
}

void Endermen::spawn_metadata(const entity::EntityState& state, net::MetadataWriter& fields) const {
    const auto it = rows_.find(state.network_id);
    if (it == rows_.end()) {
        return;
    }
    const Row& row = it->second;
    if (row.has_carried) {
        fields.optional_block_state_value(gameplay::kCarriedBlockIndex,
                                          static_cast<i32>(row.carried.value()));
    }
    if (row.screaming) {
        fields.boolean_value(gameplay::kScreamingIndex, true);
    }
    if (row.stared) {
        fields.boolean_value(gameplay::kStaredAtIndex, true);
    }
}

void Endermen::write(const entity::EntityState& state, nbt::Tag& out) const {
    if (!owns(state.type)) {
        return;
    }
    (void)out.erase("carriedBlockState");
    if (const auto block = carried(state.network_id)) {
        (void)out.put("carriedBlockState", block_state_tag(*blocks_, *block));
    }
}

void Endermen::read(entity::EntityState& state, const nbt::Tag& compound) {
    if (!owns(state.type)) {
        return;
    }
    const auto block = block_state_from(*blocks_, compound.find("carriedBlockState"));
    if (!block || blocks_->is_air(blocks_->block_of(*block))) {
        return;
    }
    Row& row        = rows_[state.network_id];
    row.carried     = *block;
    row.has_carried = true;
    // The spawn that follows carries it (`spawn_metadata`): nothing is owed.
    row.sent_valid       = true;
    row.sent_has_carried = true;
    row.sent_carried     = *block;
}

void Endermen::forget(i32 id) { rows_.erase(id); }

bool Endermen::angry(i32 id) const noexcept {
    const auto it = rows_.find(id);
    return it != rows_.end() && it->second.target != 0;
}

i32 Endermen::target_of(i32 id) const noexcept {
    const auto it = rows_.find(id);
    return it != rows_.end() ? it->second.target : 0;
}

std::optional<registry::BlockStateId> Endermen::carried(i32 id) const noexcept {
    const auto it = rows_.find(id);
    if (it == rows_.end() || !it->second.has_carried) {
        return std::nullopt;
    }
    return it->second.carried;
}

}  // namespace ov::server
