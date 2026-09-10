#include "ov/client/sound_director.hpp"

#include "ov/audio/sound_engine.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/chat.hpp"

#include <algorithm>

namespace ov::client {

namespace {

using Sounds = registry::BlockRegistry::BlockSounds;

constexpr i32 kBlock  = 4;
constexpr i32 kPlayer = 7;

[[nodiscard]] Vec3d centre(BlockPos pos) noexcept {
    return Vec3d{static_cast<f64>(pos.x) + 0.5, static_cast<f64>(pos.y) + 0.5,
                 static_cast<f64>(pos.z) + 0.5};
}

}  // namespace

SoundDirector::SoundDirector(audio::SoundEngine& engine, const registry::BlockRegistry& blocks,
                             const registry::Registries& registries, i64 seed)
    : engine_(&engine),
      blocks_(&blocks),
      registries_(&registries),
      random_(seed),
      music_(seed ^ 0x6D75736963LL),
      sound_registry_(registries.find("minecraft:sound_event")) {}

std::string_view SoundDirector::event_name(i32 id) const noexcept {
    if (!sound_registry_ || id < 0) {
        return {};
    }
    return registries_->entry_of(*sound_registry_, id);
}

std::optional<registry::BlockRegistry::BlockSounds> SoundDirector::sounds_of(
    registry::BlockStateId state) const noexcept {
    return blocks_->sounds(blocks_->block_of(state));
}

f32 SoundDirector::pitch_between(f32 lo, f32 hi) {
    if (!(hi > lo)) {
        return lo;
    }
    return lo + random_.next_float() * (hi - lo);
}

void SoundDirector::play(std::string_view event, i32 category, Vec3d at, f32 volume, f32 pitch,
                         i64 seed) {
    // A category outside Mojang's ten is refused rather than folded into one.
    if (event.empty() || category < 0 || category >= static_cast<i32>(audio::kCategoryCount)) {
        ++refused_;
        return;
    }
    audio::PlayRequest request;
    request.event    = event;
    request.category = static_cast<audio::SoundCategory>(category);
    request.position = at;
    request.volume   = volume;
    request.pitch    = pitch;
    request.seed     = seed;
    if (engine_->play(request)) {
        ++played_;
    } else {
        ++refused_;
    }
}

void SoundDirector::on_events(const netclient::ClientEvents& events, const EntityLookup& lookup) {
    for (const net::SoundEffect& sound : events.sounds) {
        const std::string_view name =
            sound.sound.sound_id >= 0 ? event_name(sound.sound.sound_id) : sound.sound.name;
        play(name, sound.category, sound.position(), sound.volume, sound.pitch, sound.seed);
    }
    for (const net::EntitySoundEffect& sound : events.entity_sounds) {
        const auto entity = lookup ? lookup(sound.entity_id) : std::nullopt;
        if (!entity) {
            ++refused_;  // an entity this client never saw
            continue;
        }
        const std::string_view name =
            sound.sound.sound_id >= 0 ? event_name(sound.sound.sound_id) : sound.sound.name;
        play(name, sound.category, entity->position, sound.volume, sound.pitch, sound.seed);
    }
    for (const net::StopSound& stop : events.stop_sounds) {
        std::optional<audio::SoundCategory> category;
        if (stop.category && *stop.category >= 0 &&
            *stop.category < static_cast<i32>(audio::kCategoryCount)) {
            category = static_cast<audio::SoundCategory>(*stop.category);
        }
        engine_->stop(category, stop.sound);
    }
    for (const net::WorldEvent& event : events.world_events) {
        // 2001 alone: a block someone else broke. The other world events —
        // dispensers, portals, anvils — have sounds no capture here has heard.
        if (event.event != net::kWorldEventBlockBreak) {
            continue;
        }
        broke(registry::BlockStateId{static_cast<u16>(event.data)},
              BlockPos{event.x, event.y, event.z});
    }
    for (const net::Explosion& explosion : events.explosions) {
        // The wiki's explosion row: Blocks, 4.0, 0.56 .. 0.84.
        play("minecraft:entity.generic.explode", kBlock, Vec3d{explosion.x, explosion.y, explosion.z},
             4.0F, pitch_between(0.56F, 0.84F), random_.next_long());
    }
    for (const netclient::ClientEvents::Pickup& pickup : events.pickups) {
        const auto entity = lookup ? lookup(pickup.collected) : std::nullopt;
        if (!entity) {
            ++refused_;
            continue;
        }
        if (entity->type == netclient::ClientEvents::kSpawnedAsExperienceOrb) {
            // The wiki's orb row: Players, 0.1, 0.55 .. 1.25.
            play("minecraft:entity.experience_orb.pickup", kPlayer, entity->position, 0.1F,
                 pitch_between(0.55F, 1.25F), random_.next_long());
        } else {
            // The wiki's item row: Players, 0.2, 1.6 .. 3.4.
            play("minecraft:entity.item.pickup", kPlayer, entity->position, 0.2F,
                 pitch_between(1.6F, 3.4F), random_.next_long());
        }
    }
}

void SoundDirector::listen(Vec3d eyes, f32 yaw_degrees) {
    engine_->set_listener(audio::Listener{eyes, yaw_degrees});
}

void SoundDirector::walked(f64 horizontal, bool on_ground, registry::BlockStateId under,
                           Vec3d feet) {
    if (!on_ground || horizontal <= 0.0) {
        return;
    }
    // The server's own stride: six tenths of the distance, a step per unit.
    walked_ += horizontal * 0.6;
    if (walked_ <= next_step_) {
        return;
    }
    next_step_ = walked_ + 1.0;
    const auto sounds = sounds_of(under);
    if (!sounds || sounds->volume < 0.0F) {
        return;
    }
    play(event_name(sounds->event(Sounds::Step)), kPlayer, feet, sounds->volume * 0.15F,
         sounds->pitch, random_.next_long());
}

void SoundDirector::mining(registry::BlockStateId state, BlockPos pos) {
    if (pos != mining_at_) {
        mining_at_    = pos;
        mining_ticks_ = 0;
    }
    if (mining_ticks_++ % 4 != 0) {
        return;
    }
    const auto sounds = sounds_of(state);
    if (!sounds || sounds->volume < 0.0F) {
        return;
    }
    play(event_name(sounds->event(Sounds::Hit)), kBlock, centre(pos), (sounds->volume + 1.0F) / 8.0F,
         sounds->pitch * 0.5F, random_.next_long());
}

void SoundDirector::broke(registry::BlockStateId state, BlockPos pos) {
    const auto sounds = sounds_of(state);
    if (!sounds || sounds->volume < 0.0F) {
        return;
    }
    play(event_name(sounds->event(Sounds::Break)), kBlock, centre(pos),
         (sounds->volume + 1.0F) / 2.0F, sounds->pitch * 0.8F, random_.next_long());
}

void SoundDirector::placed(registry::BlockStateId state, BlockPos pos) {
    const auto sounds = sounds_of(state);
    if (!sounds || sounds->volume < 0.0F) {
        return;
    }
    play(event_name(sounds->event(Sounds::Place)), kBlock, centre(pos),
         (sounds->volume + 1.0F) / 2.0F, sounds->pitch * 0.8F, random_.next_long());
}

void SoundDirector::landed(f32 damage, registry::BlockStateId under, Vec3d feet) {
    if (damage <= 0.0F) {
        return;
    }
    play(damage > 4.0F ? "minecraft:entity.player.big_fall" : "minecraft:entity.player.small_fall",
         kPlayer, feet, 1.0F, 1.0F, random_.next_long());
    if (const auto sounds = sounds_of(under); sounds && sounds->volume >= 0.0F) {
        play(event_name(sounds->event(Sounds::Fall)), kPlayer, feet, sounds->volume * 0.5F,
             sounds->pitch * 0.75F, random_.next_long());
    }
    play("minecraft:entity.player.hurt", kPlayer, feet, 1.0F, pitch_between(0.8F, 1.2F),
         random_.next_long());
}

void SoundDirector::toggled(registry::BlockStateId before, registry::BlockStateId after,
                            BlockPos pos) {
    if (before == after) {
        return;
    }
    const registry::BlockId block = blocks_->block_of(after);
    if (block != blocks_->block_of(before)) {
        return;
    }
    const auto sounds = blocks_->sounds(block);
    if (!sounds || (sounds->measured & Sounds::kToggleHeard) == 0) {
        return;
    }
    // The same three properties the server reads, in the same order.
    bool opened = false;
    bool found  = false;
    for (const std::string_view property : {"open", "powered", "power"}) {
        if (const auto view = blocks_->find_property(block, property)) {
            const auto value = [&](registry::BlockStateId state) {
                const std::string_view text = blocks_->property_value(state, *view);
                return property == "power" ? text != "0" : text == "true";
            };
            if (value(before) == value(after)) {
                return;
            }
            opened = value(after);
            found  = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    const auto audience = opened ? sounds->open_audience : sounds->close_audience;
    if (audience != Sounds::Audience::Others) {
        return;  // the server sent it to this player too
    }
    play(event_name(sounds->event(opened ? Sounds::Open : Sounds::Close)), kBlock, centre(pos),
         opened ? sounds->open_volume : sounds->close_volume,
         opened ? pitch_between(sounds->open_pitch_lo, sounds->open_pitch_hi)
                : pitch_between(sounds->close_pitch_lo, sounds->close_pitch_hi),
         random_.next_long());
}

void SoundDirector::tick(bool creative) {
    music_.tick(*engine_, audio::default_music(creative));
}

}  // namespace ov::client
