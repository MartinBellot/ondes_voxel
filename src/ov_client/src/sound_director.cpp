#include "ov/client/sound_director.hpp"

#include "ov/audio/sound_catalog.hpp"
#include "ov/audio/sound_engine.hpp"
#include "ov/base/log.hpp"
#include "ov/client/subtitles.hpp"
#include "ov/netclient/client.hpp"
#include "ov/protocol/chat.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ov::client {

namespace {

using Sounds = registry::BlockRegistry::BlockSounds;

constexpr i32 kMaster = 0;
constexpr i32 kRecord = 2;
constexpr i32 kBlock  = 4;
constexpr i32 kPlayer = 7;

/// World Event numbers, both measured on the real server
/// (scripts/measure_jukebox_events.py): a disc into a jukebox sends 1010 with
/// the disc's item id; ejecting it, or breaking the jukebox, sends 1011.
constexpr i32 kWorldEventPlayRecord = 1010;
constexpr i32 kWorldEventStopRecord = 1011;

/// Boss Bar's "play boss music" flag.
constexpr u8 kBossMusicFlag = 0x02;

/// `#minecraft:plays_underwater_music`, expanded: `#is_ocean` and `#is_river`
/// in the data generator's tags. Tags travel in Update Tags, which this client
/// does not decode yet — the one piece of the music read from a table here
/// rather than from the wire.
constexpr std::string_view kUnderwaterMusicBiomes[] = {
    "minecraft:deep_frozen_ocean", "minecraft:deep_cold_ocean", "minecraft:deep_ocean",
    "minecraft:deep_lukewarm_ocean", "minecraft:frozen_ocean", "minecraft:ocean",
    "minecraft:cold_ocean", "minecraft:lukewarm_ocean", "minecraft:warm_ocean",
    "minecraft:river", "minecraft:frozen_river"};

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
      sound_registry_(registries.find("minecraft:sound_event")),
      item_registry_(registries.find("minecraft:item")) {}

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
                         i64 seed, bool relative) {
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
    request.relative = relative;
    if (const auto started = engine_->play(request); started) {
        ++played_;
        if (subtitles_ != nullptr) {
            // A line for a sound that reaches the ears. The range is the
            // default 16 blocks times the volume: close enough for a line of
            // text, and the variant's own attenuation_distance is the engine's.
            const audio::SoundEvent* found = engine_->catalog().find(event);
            const f64 dx = at.x - ears_.x, dy = at.y - ears_.y, dz = at.z - ears_.z;
            const bool in_range =
                relative ||
                audio::SoundEngine::attenuation(std::sqrt(dx * dx + dy * dy + dz * dz), volume, 16) >
                    0.0F;
            if (found != nullptr && !found->subtitle.empty() && in_range) {
                subtitles_->heard(found->subtitle, at, relative);
            }
        }
    } else {
        // The first few by name: a refusal nobody can see is a sound that
        // silently never plays. After that, only counted.
        if (refused_ < 8) {
            OV_LOG_INFO("sound {} refused: {}", event, audio::to_string(started.error()));
        }
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
        // 2001, a block someone else broke, and the jukebox's 1010 and 1011.
        // The other world events — dispensers, portals, anvils — have sounds
        // no capture here has heard.
        const BlockPos pos{event.x, event.y, event.z};
        if (event.event == net::kWorldEventBlockBreak) {
            broke(registry::BlockStateId{static_cast<u16>(event.data)}, pos);
        } else if (event.event == kWorldEventPlayRecord) {
            record_started(event.data, pos);
        } else if (event.event == kWorldEventStopRecord) {
            record_stopped(pos);
        }
    }
    // ── music ── what the server says about where the player is.
    if (events.dimension) {
        dimension_ = *events.dimension;
    }
    if (events.biome_music) {
        biome_music_.clear();
        for (const net::BiomeMusic& music : *events.biome_music) {
            biome_music_.push_back(BiomeTrack{music.biome, music.sound, music.min_delay,
                                              music.max_delay, music.replace_current});
        }
    }
    for (const netclient::ClientEvents::BossBarChange& change : events.boss_bars) {
        std::erase_if(boss_bars_, [&](const BossBar& bar) {
            return bar.most == change.most && bar.least == change.least;
        });
        if (change.action != 1 && (change.flags & kBossMusicFlag) != 0) {
            boss_bars_.push_back(BossBar{change.most, change.least});
        }
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

void SoundDirector::listen(Vec3d eyes, f32 yaw_degrees, f32 pitch_degrees) {
    ears_     = eyes;
    ears_yaw_ = yaw_degrees;
    engine_->set_listener(audio::Listener{eyes, yaw_degrees, pitch_degrees});
}

void SoundDirector::clicked() {
    play("minecraft:ui.button.click", kMaster, ears_, 0.25F, 1.0F, random_.next_long(), true);
}

void SoundDirector::record_started(i32 item_id, BlockPos pos) {
    if (!item_registry_) {
        ++refused_;
        return;
    }
    // `minecraft:music_disc_cat` is played as `minecraft:music_disc.cat`, and
    // described by `item.minecraft.music_disc_cat.desc`.
    const std::string_view item = registries_->entry_of(*item_registry_, item_id);
    constexpr std::string_view kPrefix = "minecraft:music_disc_";
    if (!item.starts_with(kPrefix)) {
        OV_LOG_INFO("World Event 1010 with item {} ({}): not a disc", item_id, item);
        ++refused_;
        return;
    }
    const std::string_view disc = item.substr(kPrefix.size());
    record_stopped(pos);  // one record per jukebox
    std::string event = "minecraft:music_disc." + std::string{disc};
    const Vec3d centre{static_cast<f64>(pos.x) + 0.5, static_cast<f64>(pos.y) + 0.5,
                       static_cast<f64>(pos.z) + 0.5};
    play(event, kRecord, centre, kRecordVolume, 1.0F, random_.next_long());
    records_.push_back(Record{pos, std::move(event)});
    now_playing_ = R"({"translate":"record.nowPlaying","with":[{"translate":"item.minecraft.music_disc_)" +
                   std::string{disc} + R"(.desc"}]})";
}

void SoundDirector::record_stopped(BlockPos pos) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->pos == pos) {
            // Stop Sound's granularity: every voice of that disc. Two jukeboxes
            // playing the same disc stop together — named in son.md.
            engine_->stop(audio::SoundCategory::Record, it->event);
            records_.erase(it);
            return;
        }
    }
}

std::optional<std::string> SoundDirector::take_now_playing() {
    auto out = std::move(now_playing_);
    now_playing_.reset();
    return out;
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
    hit(state, pos);
}

void SoundDirector::hit(registry::BlockStateId state, BlockPos pos) {  // ── breaking ──
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
    MusicContext context;
    context.creative = creative;
    tick(context);
}

void SoundDirector::tick(const MusicContext& context) {
    // Records that ended on their own leave the list.
    std::erase_if(records_, [this](const Record& record) {
        return !engine_->is_playing(record.event);
    });
    // "Background music fades out when a music disc song can be heard, and
    // fades in again when no music disc is playing": a record within the 64
    // blocks a jukebox carries.
    bool record_heard = false;
    for (const Record& record : records_) {
        const f64 dx = static_cast<f64>(record.pos.x) + 0.5 - ears_.x;
        const f64 dy = static_cast<f64>(record.pos.y) + 0.5 - ears_.y;
        const f64 dz = static_cast<f64>(record.pos.z) + 0.5 - ears_.z;
        if (audio::SoundEngine::attenuation(std::sqrt(dx * dx + dy * dy + dz * dz), kRecordVolume,
                                            16) > 0.0F) {
            record_heard = true;
            break;
        }
    }
    const f32 step = 1.0F / static_cast<f32>(kFadeTicks);
    music_fade_    = std::clamp(music_fade_ + (record_heard ? -step : step), 0.0F, 1.0F);
    engine_->set_fade(audio::SoundCategory::Music, music_fade_);

    audio::MusicSituation situation;
    situation.menu       = context.menu;
    situation.dimension  = dimension_;
    situation.boss_music = !boss_bars_.empty();
    situation.creative   = context.creative;
    situation.underwater = context.underwater;
    situation.underwater_biome =
        std::ranges::find(kUnderwaterMusicBiomes, context.biome) != std::end(kUnderwaterMusicBiomes);
    for (const BiomeTrack& track : biome_music_) {
        if (track.biome == context.biome) {
            situation.biome = audio::MusicChoice{track.sound, track.min_delay, track.max_delay,
                                                 track.replace};
            break;
        }
    }
    music_.tick(*engine_, audio::situational_music(situation));
}

}  // namespace ov::client
