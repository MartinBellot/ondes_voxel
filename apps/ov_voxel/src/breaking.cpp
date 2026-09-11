#define OV_LOG_CATEGORY "breaking"

#include "breaking.hpp"

#include "ov/base/log.hpp"
#include "ov/client/sound_director.hpp"
#include "ov/gameplay/effects.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/breaking.hpp"
#include "ov/protocol/sound.hpp"
#include "ov/render/block_shape.hpp"
#include "ov/render/crack_mesh.hpp"

#include <algorithm>
#include <climits>
#include <string_view>

namespace ov::demo {

namespace {

/// The keys this player's own crack and a scripted one are filed under, which
/// no entity id can take.
constexpr i32 kOwnCrack    = INT_MIN;
constexpr i32 kForcedCrack = INT_MIN + 1;

constexpr usize kHelmetSlot  = 5;
constexpr usize kHotbarFirst = 36;

/// An enchantment's level on a stack, read from its raw NBT.
[[nodiscard]] u8 enchantment_level(const net::ItemStack& stack, std::string_view id) {
    if (stack.nbt.empty()) {
        return 0;
    }
    const auto document = nbt::read(stack.nbt);
    if (!document) {
        return 0;
    }
    const nbt::Tag* list = document->root.find("Enchantments");
    if (list == nullptr || list->list() == nullptr) {
        return 0;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* name  = entry.find("id");
        const nbt::Tag* level = entry.find("lvl");
        if (name != nullptr && level != nullptr && name->as_string() == id) {
            return static_cast<u8>(std::clamp<i64>(level->as_i64(), 0, 255));
        }
    }
    return 0;
}

[[nodiscard]] Vec3d grow_min(Vec3d v, f64 by) noexcept { return Vec3d{v.x - by, v.y - by, v.z - by}; }
[[nodiscard]] Vec3d grow_max(Vec3d v, f64 by) noexcept { return Vec3d{v.x + by, v.y + by, v.z + by}; }

}  // namespace

Breaking::Breaking(const registry::BlockRegistry& blocks, const registry::Registries* registries,
                   render::BlockModelCache& models, const render::TextureAtlas& atlas,
                   u32 foliage_tint, i64 seed)
    : blocks_{&blocks},
      registries_{registries},
      models_{&models},
      atlas_{&atlas},
      foliage_tint_{foliage_tint},
      particles_{seed} {
    if (registries_ != nullptr) {
        rules_.emplace(blocks, *registries_);
        item_registry_ = registries_->find("minecraft:item");
    }
}

// ── What the server said ────────────────────────────────────────────────────

void Breaking::set_crack(i32 key, BlockPos pos, i32 stage) {
    if (!net::is_drawn_stage(stage)) {
        cracks_.erase(key);
        return;
    }
    cracks_[key] = Crack{pos, stage, ticks_};
}

void Breaking::on_events(const netclient::ClientEvents& events) {
    if (events.own_entity_id) {
        own_id_ = *events.own_entity_id;
        effects_.clear();
    }
    for (const auto& effect : events.own_effects) {
        if (effect.amplifier < 0) {
            effects_.erase(effect.effect_id);
        } else {
            effects_[effect.effect_id] = effect.amplifier;
        }
    }
    for (const net::BlockDestroyStage& stage : events.destroy_stages) {
        ++counters_.others_stages;
        set_crack(stage.entity_id, BlockPos{stage.position.x, stage.position.y, stage.position.z},
                  stage.stage);
    }
    for (const net::WorldEvent& event : events.world_events) {
        if (event.event != net::kWorldEventBlockBreak) {
            continue;
        }
        // Someone else's block: its sound is SoundDirector's, its burst is here.
        ++counters_.others_broken;
        burst(BlockPos{event.x, event.y, event.z},
              registry::BlockStateId{static_cast<u16>(event.data)});
    }
}

// ── What is held, and how the player stands ─────────────────────────────────

gameplay::Held Breaking::held(const BreakingTick& input) const {
    gameplay::Held out;
    if (input.inventory == nullptr) {
        return out;
    }
    const usize slot = kHotbarFirst + static_cast<usize>(std::clamp(input.selected, 0, 8));
    if (slot >= input.inventory->size()) {
        return out;
    }
    const net::ItemStack& stack = (*input.inventory)[slot];
    if (stack.item_id != 0 && stack.count > 0) {
        out.item       = stack.item_id;
        out.efficiency = enchantment_level(stack, "minecraft:efficiency");
    }
    return out;
}

gameplay::Stance Breaking::stance(const BreakingTick& input) const {
    gameplay::Stance out;
    out.on_ground     = input.on_ground;
    out.head_in_water = input.head_in_water;
    if (input.inventory != nullptr && kHelmetSlot < input.inventory->size()) {
        out.aqua_affinity =
            enchantment_level((*input.inventory)[kHelmetSlot], "minecraft:aqua_affinity") > 0;
    }
    const auto amplifier = [&](gameplay::Effect effect) -> i32 {
        const auto it = effects_.find(static_cast<i32>(effect));
        return it == effects_.end() ? -1 : it->second;
    };
    const i32 haste = std::max(amplifier(gameplay::Effect::Haste),
                               amplifier(gameplay::Effect::ConduitPower));
    out.haste          = static_cast<i8>(std::clamp(haste, -1, 127));
    out.mining_fatigue = static_cast<i8>(std::clamp(amplifier(gameplay::Effect::MiningFatigue), -1, 127));
    return out;
}

bool Breaking::creative_can_attack(const BreakingTick& input) const {
    const gameplay::Held holding = held(input);
    if (!holding.item || registries_ == nullptr || !item_registry_) {
        return true;
    }
    const std::string_view name = registries_->entry_of(*item_registry_, *holding.item);
    return !(name.ends_with("_sword") || name == "minecraft:trident" ||
             name == "minecraft:debug_stick");
}

i32 Breaking::held_identity(const BreakingTick& input) const {
    if (input.inventory == nullptr) {
        return -1;
    }
    const usize slot = kHotbarFirst + static_cast<usize>(std::clamp(input.selected, 0, 8));
    if (slot >= input.inventory->size()) {
        return -1;
    }
    const net::ItemStack& stack = (*input.inventory)[slot];
    // The item and its tags, as vanilla's "same item, same tags" test. A slot
    // number changes nothing on its own.
    u32 hash = static_cast<u32>(stack.count > 0 ? stack.item_id : 0) * 2654435761U;
    for (const u8 byte : stack.nbt) {
        hash = (hash ^ byte) * 16777619U;
    }
    return static_cast<i32>(hash & 0x7FFFFFFFU);
}

// ── Particles ───────────────────────────────────────────────────────────────

std::optional<client::ParticleSprite> Breaking::sprite_of(registry::BlockStateId state) {
    const render::BlockRender& render = models_->resolve(state);
    if (render.particle_sprite.empty()) {
        return std::nullopt;
    }
    const render::AtlasSprite* sprite = atlas_->find(render.particle_sprite);
    if (sprite == nullptr) {
        return std::nullopt;
    }
    return client::ParticleSprite{sprite->uv.u0, sprite->uv.v0, sprite->uv.u1, sprite->uv.v1};
}

u32 Breaking::tint_of(registry::BlockStateId state) {
    const render::BlockRender& render = models_->resolve(state);
    const registry::BlockId    block  = blocks_->block_of(state);
    // A grass block's particles are its dirt-coloured sprite, untinted.
    if (blocks_->block_name(block) == "minecraft:grass_block") {
        return 0xFFFFFF;
    }
    switch (render.tint) {
        case render::TintChannel::None: return 0xFFFFFF;
        case render::TintChannel::Water: return 0x3F76E4;
        default: return foliage_tint_;
    }
}

void Breaking::shape_of(registry::BlockStateId state, std::vector<AABB>& out) {
    out.clear();
    render::outline_boxes(*blocks_, state, models_->resolve(state).model, out);
}

void Breaking::burst(BlockPos pos, registry::BlockStateId state) {
    const auto sprite = sprite_of(state);
    if (!sprite) {
        ++counters_.no_sprite;
        return;
    }
    shape_of(state, boxes_);
    particles_.destroy(pos, boxes_, *sprite, tint_of(state));
}

void Breaking::chip(BlockPos pos, Direction face, registry::BlockStateId state) {
    const auto sprite = sprite_of(state);
    if (!sprite) {
        ++counters_.no_sprite;
        return;
    }
    shape_of(state, boxes_);
    if (boxes_.empty()) {
        return;
    }
    particles_.crack(pos, face, render::bounds_of(boxes_), *sprite, tint_of(state));
}

// ── The swing ───────────────────────────────────────────────────────────────

i32 Breaking::swing_duration() const noexcept {
    const auto amplifier = [&](gameplay::Effect effect) -> i32 {
        const auto it = effects_.find(static_cast<i32>(effect));
        return it == effects_.end() ? -1 : it->second;
    };
    const i32 haste = std::max(amplifier(gameplay::Effect::Haste),
                               amplifier(gameplay::Effect::ConduitPower));
    if (haste >= 0) {
        return 6 - (1 + haste);
    }
    const i32 fatigue = amplifier(gameplay::Effect::MiningFatigue);
    if (fatigue >= 0) {
        return 6 + (1 + fatigue) * 2;
    }
    return 6;
}

std::optional<f32> Breaking::swing_progress(f32 partial) const noexcept {
    if (!swinging_) {
        return std::nullopt;
    }
    const i32 duration = std::max(1, swing_duration());
    return std::clamp((static_cast<f32>(swing_time_) + partial) / static_cast<f32>(duration), 0.0F,
                      1.0F);
}

void Breaking::swing(netclient::Client& client) {
    // Restarted only when still or past half, so holding the button swings in
    // a rhythm rather than freezing at the start.
    const i32 duration = swing_duration();
    if (!swinging_ || swing_time_ >= duration / 2 || swing_time_ < 0) {
        swing_time_ = -1;
        swinging_   = true;
    }
    client.send_swing();
    ++counters_.swings;
}

// ── One tick ────────────────────────────────────────────────────────────────

void Breaking::tick(const BreakingTick& input, const BlockAt& block_at, netclient::Client& client,
                    client::SoundDirector* sounds, const gameplay::CollisionWorld* world) {
    ++ticks_;

    gameplay::DigInput dig;
    dig.pressed                = input.pressed;
    dig.held                   = input.held;
    dig.creative               = input.creative;
    dig.can_attack_in_creative = creative_can_attack(input);
    dig.held_item              = held_identity(input);
    if (input.aimed) {
        const BlockPos               at    = input.aimed->block;
        const registry::BlockStateId state = block_at(at);
        gameplay::DigTarget target;
        target.pos      = at;
        target.face     = static_cast<u8>(input.aimed->face);
        target.state    = state;
        target.diggable = !blocks_->is_air(blocks_->block_of(state));
        dig.target      = target;
        if (rules_) {
            dig.progress_per_tick = rules_->destroy_progress(state, held(input), stance(input));
        }
    }

    const gameplay::DigOutcome out = dig_.tick(dig);
    for (u8 i = 0; i < out.action_count; ++i) {
        const gameplay::DigAction& action = out.actions[i];
        client.send_dig(action.pos.x, action.pos.y, action.pos.z, static_cast<i32>(action.status),
                        action.face);
        switch (action.status) {
            case gameplay::DigStatus::Start:
                ++counters_.starts;
                start_tick_ = ticks_;
                break;
            case gameplay::DigStatus::Abort: ++counters_.aborts; break;
            case gameplay::DigStatus::Finish:
                ++counters_.finishes;
                // The measure scripts read this line: the claim's tick, the
                // start's own tick counted as the first.
                OV_LOG_INFO("finish ({}, {}, {}) on the {}th tick of its dig", action.pos.x,
                            action.pos.y, action.pos.z, ticks_ - start_tick_ + 1);
                break;
        }
        OV_LOG_DEBUG("tick {}: Player Action {} at ({}, {}, {})", ticks_,
                     static_cast<i32>(action.status), action.pos.x, action.pos.y, action.pos.z);
    }
    if (out.hit_sound && dig.target && sounds != nullptr) {
        ++counters_.hits;
        sounds->hit(dig.target->state, dig.target->pos);
    }
    if (out.crack_particle && dig.target) {
        chip(dig.target->pos, static_cast<Direction>(dig.target->face), dig.target->state);
    }
    if (out.broken) {
        // The breaker's own client plays the break and makes the burst: the
        // server's World Event 2001 goes to everyone else.
        ++counters_.broken;
        if (sounds != nullptr) {
            sounds->broke(out.broken_state, *out.broken);
        }
        burst(*out.broken, out.broken_state);
        OV_LOG_INFO("broke ({}, {}, {}) at tick {}", out.broken->x, out.broken->y, out.broken->z,
                    ticks_);
    }
    if (out.swing) {
        swing(client);
    }

    // The swing's own clock.
    if (swinging_) {
        ++swing_time_;
        if (swing_time_ >= swing_duration()) {
            swing_time_ = 0;
            swinging_   = false;
        }
    }

    // This player's crack, from its own count.
    if (const auto at = dig_.digging()) {
        set_crack(kOwnCrack, *at, dig_.stage());
    } else {
        cracks_.erase(kOwnCrack);
    }
    // Cracks nobody has spoken of for a while go.
    for (auto it = cracks_.begin(); it != cracks_.end();) {
        if (it->first != kOwnCrack && it->first != kForcedCrack &&
            ticks_ - it->second.updated > static_cast<u64>(kStaleCrackTicks)) {
            it = cracks_.erase(it);
        } else {
            ++it;
        }
    }

    particles_.tick(world);
}

void Breaking::force_stage(BlockPos pos, i32 stage) { set_crack(kForcedCrack, pos, stage); }

// ── Drawing ─────────────────────────────────────────────────────────────────

void Breaking::build_cracks(const BlockAt& block_at,
                            std::array<std::vector<render::EntityVertex>, 10>& out) {
    for (auto& stage : out) {
        stage.clear();
    }
    // One crack a block: the furthest along, when two players dig the same one.
    // A handful at most, so a linear search beats any index.
    deepest_.clear();
    for (const auto& [key, crack] : cracks_) {
        (void)key;
        const auto it = std::find_if(deepest_.begin(), deepest_.end(),
                                     [&](const auto& entry) { return entry.first == crack.pos; });
        if (it == deepest_.end()) {
            deepest_.emplace_back(crack.pos, crack.stage);
        } else {
            it->second = std::max(it->second, crack.stage);
        }
    }
    for (const auto& [pos, stage] : deepest_) {
        const render::BlockRender& render = models_->resolve(block_at(pos));
        if (!render.drawable || !net::is_drawn_stage(stage)) {
            continue;
        }
        render::build_crack_quads(render.model, pos, out[static_cast<usize>(stage)]);
    }
}

void Breaking::build_particles(Vec3f camera_right, Vec3f camera_up, f32 partial,
                               const client::ParticleLight& light,
                               std::vector<render::EntityVertex>& out) const {
    particles_.build(camera_right, camera_up, partial, light, out);
}

void Breaking::outline(BlockPos pos, registry::BlockStateId state,
                       std::vector<std::array<Vec3d, 2>>& out) {
    out.clear();
    shape_of(state, boxes_);
    // Grown by a thousandth, so the lines sit just off the faces they surround.
    constexpr f64 kGrow = 0.002;
    const Vec3d   origin{static_cast<f64>(pos.x), static_cast<f64>(pos.y), static_cast<f64>(pos.z)};
    for (AABB& box : boxes_) {
        box = AABB{grow_min(box.min, kGrow) + origin, grow_max(box.max, kGrow) + origin};
    }
    render::shape_edges(boxes_, out);
}

}  // namespace ov::demo
