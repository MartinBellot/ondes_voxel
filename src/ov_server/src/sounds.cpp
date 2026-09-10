#include "sounds.hpp"

#include "ov/protocol/chat.hpp"
#include "ov/protocol/sound.hpp"

#include <algorithm>

namespace ov::server {

namespace {

/// A Sound Effect's reach at volume 1. Above 1 it is multiplied by the volume.
constexpr f64 kHearing = 16.0;

constexpr i32 kBlockCategory  = net::sound_category::kBlock;
constexpr i32 kPlayerCategory = net::sound_category::kPlayer;

/// The pitch a hurt player is heard at is random; two samples (1.058, 1.087)
/// cannot bound it, so the creatures' measured range is used — every hurt and
/// death sound sampled across the living types fell inside 0.8 .. 1.2.
constexpr f32 kVoiceLo = 0.8F;
constexpr f32 kVoiceHi = 1.2F;

[[nodiscard]] Vec3d centre(BlockPos pos) noexcept {
    return Vec3d{static_cast<f64>(pos.x) + 0.5, static_cast<f64>(pos.y) + 0.5,
                 static_cast<f64>(pos.z) + 0.5};
}

}  // namespace

Sounds::Sounds(const registry::BlockRegistry& blocks, const registry::Registries& registries,
               i64 seed)
    : blocks_(&blocks), registries_(&registries), random_(seed) {
    // Sized once: queue_changed runs inside the tick, which must not allocate.
    pending_.reserve(256);
    tnt_primed_  = sound_named("minecraft:entity.tnt.primed");
    levelup_     = sound_named("minecraft:entity.player.levelup");
    small_fall_  = sound_named("minecraft:entity.player.small_fall");
    big_fall_    = sound_named("minecraft:entity.player.big_fall");
    player_hurt_ = sound_named("minecraft:entity.player.hurt");
    chest_open_  = sound_named("minecraft:block.chest.open");
    chest_close_ = sound_named("minecraft:block.chest.close");
    attack_crit_      = sound_named("minecraft:entity.player.attack.crit");
    attack_sweep_     = sound_named("minecraft:entity.player.attack.sweep");
    attack_knockback_ = sound_named("minecraft:entity.player.attack.knockback");
    attack_strong_    = sound_named("minecraft:entity.player.attack.strong");
    attack_weak_      = sound_named("minecraft:entity.player.attack.weak");
}

void Sounds::eating(const SoundHost& host, const void* eater, Vec3d feet) {
    const f32 volume = random_.next_int(2) == 0 ? 0.5F : 1.0F;
    play(host, eater, sound_named("minecraft:entity.generic.eat"), kPlayerCategory, feet, volume,
         pitch_between(kVoiceLo, kVoiceHi));
}

void Sounds::ate(const SoundHost& host, Vec3d feet) {
    play(host, nullptr, sound_named("minecraft:entity.player.burp"), kPlayerCategory, feet, 0.5F,
         pitch_between(0.9F, 1.0F));
    const f32 volume = random_.next_int(2) == 0 ? 0.5F : 1.0F;
    play(host, nullptr, sound_named("minecraft:entity.generic.eat"), net::sound_category::kNeutral,
         feet, volume, pitch_between(kVoiceLo, kVoiceHi));
}

void Sounds::player_attack(const SoundHost& host, Vec3d feet, bool critical, bool swept,
                           bool knockback, f32 strength_scale) {
    const bool strong = strength_scale > 0.9F;
    const i32  sound  = critical               ? attack_crit_
                        : swept                ? attack_sweep_
                        : knockback && strong  ? attack_knockback_
                        : strong               ? attack_strong_
                                               : attack_weak_;
    play(host, nullptr, sound, kPlayerCategory, feet, 1.0F, 1.0F);
}

i32 Sounds::sound_named(std::string_view name) const noexcept {
    const auto registry = registries_->find("minecraft:sound_event");
    if (!registry) {
        return -1;
    }
    const auto id = registries_->protocol_id(*registry, name);
    return id ? static_cast<i32>(*id) : -1;
}

std::optional<registry::BlockRegistry::BlockSounds> Sounds::sounds_of(
    registry::BlockStateId state) const noexcept {
    return blocks_->sounds(blocks_->block_of(state));
}

f32 Sounds::pitch_between(f32 lo, f32 hi) {
    if (!(hi > lo)) {
        return lo;
    }
    return lo + random_.next_float() * (hi - lo);
}

void Sounds::play(const SoundHost& host, const void* except, i32 sound, i32 category, Vec3d at,
                  f32 volume, f32 pitch) {
    if (sound < 0 || category < 0 || volume <= 0.0F || !host.send_near) {
        return;
    }
    net::SoundEffect effect;
    effect.sound.sound_id = sound;
    effect.category       = category;
    effect.x              = net::sound_coordinate(at.x);
    effect.y              = net::sound_coordinate(at.y);
    effect.z              = net::sound_coordinate(at.z);
    effect.volume         = volume;
    effect.pitch          = pitch;
    // The seed picks the variant on every client, so all of them hear the
    // same stone. Drawn from the server's own generator: principle 5.
    effect.seed           = random_.next_long();
    const f64 radius      = kHearing * std::max(1.0, static_cast<f64>(volume));
    host.send_near(except, at, radius, net::clientbound::kSoundEffect,
                   net::encode_sound_effect(effect));
    ++sent_;
}

void Sounds::block_placed(const SoundHost& host, const void* placer, BlockPos pos,
                          registry::BlockStateId state) {
    const auto sounds = sounds_of(state);
    if (!sounds || sounds->volume < 0.0F) {
        return;
    }
    play(host, placer, sounds->event(registry::BlockRegistry::BlockSounds::Place), kBlockCategory,
         centre(pos), (sounds->volume + 1.0F) / 2.0F, sounds->pitch * 0.8F);
}

void Sounds::block_broken(const SoundHost& host, const void* breaker, BlockPos pos,
                          registry::BlockStateId before) {
    if (before == registry::kAirState || !host.send_near) {
        return;
    }
    host.send_near(breaker, centre(pos), kWorldEventRadius, net::clientbound::kWorldEvent,
                   net::encode_world_event(net::kWorldEventBlockBreak,
                                           net::WirePosition{pos.x, pos.y, pos.z},
                                           static_cast<i32>(before.value()), false));
}

void Sounds::block_changed(const SoundHost& host, const void* actor, BlockPos pos,
                           registry::BlockStateId before, registry::BlockStateId after) {
    using Sound = registry::BlockRegistry::BlockSounds;
    if (before == after) {
        return;
    }
    const registry::BlockId block = blocks_->block_of(after);
    if (block != blocks_->block_of(before)) {
        return;
    }
    const auto sounds = blocks_->sounds(block);
    if (!sounds || (sounds->measured & Sound::kToggleHeard) == 0) {
        return;
    }

    // Which way it went. Doors, trapdoors and gates say `open`; buttons,
    // levers and most plates `powered`; weighted plates a `power` level. A
    // change to any other property — a door's `powered` without its `open`,
    // say — plays nothing.
    bool opened = false;
    if (const auto open = blocks_->find_property(block, "open")) {
        const bool was = blocks_->property_value(before, *open) == "true";
        const bool is  = blocks_->property_value(after, *open) == "true";
        if (was == is) {
            return;
        }
        // A door is two blocks and a tick writes both halves; only the lower
        // one speaks. A click reaches here once, for the half clicked.
        if (actor == nullptr) {
            if (const auto half = blocks_->find_property(block, "half");
                half && blocks_->property_value(after, *half) == "upper") {
                return;
            }
        }
        opened = is;
    } else if (const auto powered = blocks_->find_property(block, "powered")) {
        const bool was = blocks_->property_value(before, *powered) == "true";
        const bool is  = blocks_->property_value(after, *powered) == "true";
        if (was == is) {
            return;
        }
        opened = is;
    } else if (const auto power = blocks_->find_property(block, "power")) {
        const bool was = blocks_->property_value(before, *power) != "0";
        const bool is  = blocks_->property_value(after, *power) != "0";
        if (was == is) {
            return;
        }
        opened = is;
    } else {
        return;
    }

    const i32  event    = sounds->event(opened ? Sound::Open : Sound::Close);
    const f32  volume   = opened ? sounds->open_volume : sounds->close_volume;
    const f32  lo       = opened ? sounds->open_pitch_lo : sounds->close_pitch_lo;
    const f32  hi       = opened ? sounds->open_pitch_hi : sounds->close_pitch_hi;
    const auto audience = opened ? sounds->open_audience : sounds->close_audience;
    // A tick has no player to leave out; a click leaves out the clicker only
    // when vanilla does.
    const void* except = audience == Sound::Audience::Others ? actor : nullptr;
    play(host, except, event, kBlockCategory, centre(pos), volume, pitch_between(lo, hi));
}

void Sounds::queue_changed(BlockPos pos, registry::BlockStateId before,
                           registry::BlockStateId after) {
    // Filtered here, before the buffer: a tick writes water, dust and leaves by
    // the thousand, and a bounded queue full of those would drop the one door.
    if (before == after || blocks_->block_of(before) != blocks_->block_of(after)) {
        return;
    }
    const auto sounds = blocks_->sounds(blocks_->block_of(after));
    if (!sounds ||
        (sounds->measured & registry::BlockRegistry::BlockSounds::kToggleHeard) == 0) {
        return;
    }
    if (pending_.size() < pending_.capacity()) {
        pending_.push_back(Pending{pos, before, after});
    }
}

void Sounds::flush(const SoundHost& host) {
    for (const Pending& change : pending_) {
        block_changed(host, nullptr, change.pos, change.before, change.after);
    }
    pending_.clear();
}

void Sounds::container(const SoundHost& host, BlockPos pos, registry::BlockStateId state,
                       bool opened) {
    // The chest alone: its sound, volume and audience were captured (0.5,
    // everyone). A barrel, an ender chest and a shulker box have sounds of
    // their own that no capture has heard yet, and are left silent by name.
    const std::string_view name = blocks_->block_name(blocks_->block_of(state));
    if (name != "minecraft:chest" && name != "minecraft:trapped_chest") {
        return;
    }
    play(host, nullptr, opened ? chest_open_ : chest_close_, kBlockCategory, centre(pos), 0.5F,
         pitch_between(0.9F, 1.0F));
}

void Sounds::step(const SoundHost& host, const void* walker, Vec3d feet,
                  registry::BlockStateId under) {
    const auto sounds = sounds_of(under);
    if (!sounds || sounds->volume < 0.0F) {
        return;
    }
    play(host, walker, sounds->event(registry::BlockRegistry::BlockSounds::Step),
         kPlayerCategory, feet, sounds->volume * 0.15F, sounds->pitch);
}

void Sounds::fall(const SoundHost& host, const void* faller, Vec3d feet,
                  registry::BlockStateId under, f32 damage) {
    // The order the capture shows: the fall, the block, the hurt.
    play(host, faller, damage > 4.0F ? big_fall_ : small_fall_, kPlayerCategory, feet, 1.0F, 1.0F);
    if (const auto sounds = sounds_of(under); sounds && sounds->volume >= 0.0F) {
        play(host, faller, sounds->event(registry::BlockRegistry::BlockSounds::Fall),
             kPlayerCategory, feet, sounds->volume * 0.5F, sounds->pitch * 0.75F);
    }
    player_hurt(host, faller, feet);
}

void Sounds::player_hurt(const SoundHost& host, const void* victim, Vec3d feet) {
    play(host, victim, player_hurt_, kPlayerCategory, feet, 1.0F, pitch_between(kVoiceLo, kVoiceHi));
}

void Sounds::mob_hurt(const SoundHost& host, i32 type, Vec3d position) {
    const auto sounds = registries_->entity_sounds(type);
    if (!sounds || sounds->hurt < 0) {
        return;
    }
    play(host, nullptr, sounds->hurt, sounds->category, position, sounds->volume,
         pitch_between(sounds->pitch_lo, sounds->pitch_hi));
}

void Sounds::mob_death(const SoundHost& host, i32 type, Vec3d position) {
    const auto sounds = registries_->entity_sounds(type);
    if (!sounds || sounds->death < 0) {
        return;
    }
    play(host, nullptr, sounds->death, sounds->category, position, sounds->volume,
         pitch_between(sounds->pitch_lo, sounds->pitch_hi));
}

void Sounds::tnt_primed(const SoundHost& host, Vec3d position) {
    play(host, nullptr, tnt_primed_, kBlockCategory, position, 1.0F, 1.0F);
}

void Sounds::level_up(const SoundHost& host, Vec3d feet, i32 level) {
    // Every fifth level, at a fifth of the way to 30 times 0.75: level 5 was
    // captured at 0.125. Levels 1 and 2 sent nothing.
    if (level <= 0 || level % 5 != 0) {
        return;
    }
    const f32 loudness = level > 30 ? 1.0F : static_cast<f32>(level) / 30.0F;
    play(host, nullptr, levelup_, kPlayerCategory, feet, loudness * 0.75F, 1.0F);
}

bool Sounds::advance(Stride& stride, f64 horizontal) noexcept {
    stride.walked += horizontal * 0.6;
    if (stride.walked > stride.next) {
        stride.next = stride.walked + 1.0;
        return true;
    }
    return false;
}

}  // namespace ov::server
