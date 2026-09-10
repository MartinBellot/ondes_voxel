// What the server makes heard, and to whom.
//
// Every rule here was read off a capture of the real 1.20.1 server by two
// probes — one that acts, one that listens four blocks away — and not taken
// from memory (scripts/capture_sound_packets.py, docs/provenance/son.md). The
// question that matters most is not *which* sound but *who hears it*, because
// vanilla leaves out exactly the player whose own client already played it:
//
//   placing a block          Sound Effect `…place`, everyone near but the placer
//   breaking a block         World Event 2001 with the old state, everyone but
//                            the breaker (the client plays the sound itself)
//   door, trapdoor, gate,    the block's own open/close sound, everyone but the
//   button pressed by hand   player who clicked
//   lever, button released,  everyone, the player who did it included — the
//   anything moved by power  server did it, not a client
//   chest opened or closed   everyone
//   footsteps, a fall        everyone but the walker; category player
//   a creature hurt or dead  everyone; its own category and volume
//   TNT primed               everyone, at the entity
//   level 5, 10, 15…         everyone, volume level/30 * 0.75, capped at 1
//   an explosion, a pickup   nothing: the client plays both from the packet it
//                            already gets (Explosion, Take Item Entity)
//
// A Sound Effect reaches a player within 16 blocks, times the volume when the
// volume is above 1: the capture's /playsound at volume 0.7 from 20 blocks
// never arrived, the same at volume 2 did.
//
// No reference to the server: everything goes through SoundHost, a callback
// the server builds once, the way workbench.hpp and world_ticks.hpp do it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ov::server {

struct SoundHost {
    /// Send one packet to every player within `radius` blocks of `at`, except
    /// the one whose key is `except` (nullptr: nobody is left out). The key is
    /// whatever the server identifies a player by — its connection.
    std::function<void(const void* except, Vec3d at, f64 radius, i32 packet_id,
                       std::span<const u8> payload)>
        send_near;
};

/// How far a World Event travels. Not measured — the probes were four blocks
/// apart — and named as ours rather than presented as vanilla's.
inline constexpr f64 kWorldEventRadius = 64.0;

class Sounds {
public:
    Sounds(const registry::BlockRegistry& blocks, const registry::Registries& registries,
           i64 seed);

    /// A player put a block down.
    void block_placed(const SoundHost& host, const void* placer, BlockPos pos,
                      registry::BlockStateId state);

    /// A player broke a block that was `before`.
    void block_broken(const SoundHost& host, const void* breaker, BlockPos pos,
                      registry::BlockStateId before);

    /// A block changed state in place. `actor` is the player whose click did it,
    /// or nullptr when a tick did (a button releasing, a door moved by power).
    /// Plays nothing unless the change is one of the measured toggles.
    void block_changed(const SoundHost& host, const void* actor, BlockPos pos,
                       registry::BlockStateId before, registry::BlockStateId after);

    /// The same, for the tick thread: queued without allocating (the buffer is
    /// reused) and sent by `flush`, once the player lock can be taken.
    void queue_changed(BlockPos pos, registry::BlockStateId before, registry::BlockStateId after);
    void flush(const SoundHost& host);

    /// A container at `pos` was opened by its first viewer, or closed by its last.
    void container(const SoundHost& host, BlockPos pos, registry::BlockStateId state, bool opened);

    /// A walking player's footstep, on `under` — the block below the feet.
    void step(const SoundHost& host, const void* walker, Vec3d feet, registry::BlockStateId under);

    /// A player took `damage` points from a fall onto `under`.
    void fall(const SoundHost& host, const void* faller, Vec3d feet, registry::BlockStateId under,
              f32 damage);

    /// A player was hurt by something other than a fall.
    void player_hurt(const SoundHost& host, const void* victim, Vec3d feet);

    /// A creature of `type` (minecraft:entity_type id) was hurt, or killed.
    void mob_hurt(const SoundHost& host, i32 type, Vec3d position);
    void mob_death(const SoundHost& host, i32 type, Vec3d position);

    /// A TNT entity was primed at `position` (the entity's, not the block's).
    void tnt_primed(const SoundHost& host, Vec3d position);

    /// A player's experience level rose to `level`.
    void level_up(const SoundHost& host, Vec3d feet, i32 level);

    /// A player's footstep accounting: horizontal distance in, a step out when
    /// one is due. Vanilla adds six tenths of the distance walked and steps
    /// every whole unit — one footstep per 1.67 blocks, which the capture's
    /// spacings average to (1.65 over five).
    struct Stride {
        f64 walked{0.0};
        f64 next{1.0};
    };
    [[nodiscard]] static bool advance(Stride& stride, f64 horizontal) noexcept;

    /// Sounds sent since construction, for the log and the tests.
    [[nodiscard]] u64 sent() const noexcept { return sent_; }

private:
    struct Pending {
        BlockPos               pos;
        registry::BlockStateId before;
        registry::BlockStateId after;
    };

    void play(const SoundHost& host, const void* except, i32 sound, i32 category, Vec3d at,
              f32 volume, f32 pitch);
    [[nodiscard]] f32 pitch_between(f32 lo, f32 hi);
    [[nodiscard]] i32 sound_named(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<registry::BlockRegistry::BlockSounds> sounds_of(
        registry::BlockStateId state) const noexcept;

    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    math::LegacyRandomSource       random_;
    std::vector<Pending>           pending_;
    u64                            sent_{0};

    // Resolved once: the sounds this module names rather than reads from a table.
    i32 tnt_primed_{-1};
    i32 levelup_{-1};
    i32 small_fall_{-1};
    i32 big_fall_{-1};
    i32 player_hurt_{-1};
    i32 chest_open_{-1};
    i32 chest_close_{-1};
};

}  // namespace ov::server
