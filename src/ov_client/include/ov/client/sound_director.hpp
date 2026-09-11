// What the client hears: the server's sounds, and the ones it plays itself.
//
// Vanilla's client is not a loudspeaker for the server. Some of what a player
// hears the server never sends — to that player — because the client already
// knows: its own footsteps, the block it is mining, the one it broke or placed,
// its own landing. Others the server sends to nobody, because every client can
// work them out from a packet it already gets: an explosion from Explosion,
// the pickup plop from Take Item Entity, another player's broken block from
// World Event 2001. The capture of the real server (docs/provenance/son.md) is
// what separates the three; this class covers all of them.
//
// Volumes and pitches of the local gestures are the same set the server's
// are derived from — BlockRegistry::sounds() — through the ratios the capture
// measured (place (v+1)/2 and p*0.8, step v*0.15, fall v*0.5 and p*0.75) and,
// for the two no packet carries, the wiki's block tables (break (v+1)/2 and
// p*0.8, hit (v+1)/8 and p*0.5: stone's row reads 1.0/0.8 and 0.25/0.5).
//
// Layer 15, and nothing here draws: it takes events and positions, and calls
// the engine. Testable with the null backend.
#pragma once

#include "ov/audio/music.hpp"
#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace ov::netclient {
struct ClientEvents;
}

namespace ov::audio {
class SoundEngine;
enum class SoundCategory : u8;
}  // namespace ov::audio

namespace ov::client {

/// An entity the director needs to place a sound on.
struct HeardEntity {
    Vec3d position{};
    /// Mojang's entity type id, or ClientEvents::kSpawnedAsExperienceOrb.
    i32 type{0};
};

/// Where an entity is, by network id. Empty for one this client does not know.
using EntityLookup = std::function<std::optional<HeardEntity>(i32 id)>;

class SoundDirector {
public:
    SoundDirector(audio::SoundEngine& engine, const registry::BlockRegistry& blocks,
                  const registry::Registries& registries, i64 seed);

    /// Everything the server said this poll that makes a sound. Call it
    /// **before** the entity world applies the same events: a pickup removes the
    /// item it plops for, and a removed entity has no position left to plop at.
    void on_events(const netclient::ClientEvents& events, const EntityLookup& lookup);

    /// The ears: the camera, every frame.
    void listen(Vec3d eyes, f32 yaw_degrees);

    // ── The player's own gestures, which the server sends to everyone else ──

    /// A physics step of the local player. Counts strides itself; plays the
    /// footstep of `under` when one is due and the player is on the ground.
    void walked(f64 horizontal, bool on_ground, registry::BlockStateId under, Vec3d feet);
    /// One tick of mining the block at `pos`. The hit plays every fourth.
    void mining(registry::BlockStateId state, BlockPos pos);
    /// ── breaking ── One hit sound, now: the dig controller already counts
    /// the fourth ticks (gameplay::DigController::tick's `hit_sound`).
    void hit(registry::BlockStateId state, BlockPos pos);
    void broke(registry::BlockStateId state, BlockPos pos);
    void placed(registry::BlockStateId state, BlockPos pos);
    /// A landing that did `damage` points, onto `under`.
    void landed(f32 damage, registry::BlockStateId under, Vec3d feet);
    /// A block this player clicked came back from the server changed. Plays the
    /// open or close sound **only** when vanilla leaves the clicker out of it
    /// (a door, a trapdoor, a gate, a button pressed): then the server sent it to
    /// everyone else, and this client is the one that must play it. A lever is
    /// sent to the clicker too, and playing it here would double it.
    void toggled(registry::BlockStateId before, registry::BlockStateId after, BlockPos pos);

    /// One client tick, 20 Hz: the music.
    void tick(bool creative);

    [[nodiscard]] u64 played() const noexcept { return played_; }
    [[nodiscard]] u64 refused() const noexcept { return refused_; }
    [[nodiscard]] const audio::MusicManager& music() const noexcept { return music_; }

private:
    void play(std::string_view event, i32 category, Vec3d at, f32 volume, f32 pitch, i64 seed);
    [[nodiscard]] f32 pitch_between(f32 lo, f32 hi);
    [[nodiscard]] std::optional<registry::BlockRegistry::BlockSounds> sounds_of(
        registry::BlockStateId state) const noexcept;
    [[nodiscard]] std::string_view event_name(i32 id) const noexcept;

    audio::SoundEngine*            engine_;
    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    math::LegacyRandomSource       random_;
    audio::MusicManager            music_;
    std::optional<registry::RegistryId> sound_registry_;

    f64      walked_{0.0};
    f64      next_step_{1.0};
    BlockPos mining_at_{};
    i32      mining_ticks_{0};
    u64      played_{0};
    u64      refused_{0};
};

}  // namespace ov::client
