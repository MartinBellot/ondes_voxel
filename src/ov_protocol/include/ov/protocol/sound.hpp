// Sound on the wire: Sound Effect, Entity Sound Effect, Stop Sound, and the
// reading side of World Event.
//
// Identified by content in a capture of the real 1.20.1 server
// (scripts/capture_sound_packets.py), not by the archived page alone. The
// `layout.*` gestures of that capture chose every field from the console —
// `/playsound` with a known position, volume, pitch and category, an
// unregistered name, and the four forms of `/stopsound` — so each field below
// was recognised by its value:
//
//   * **Sound Effect is 0x62.** The sound is the registry id **plus one**, or 0
//     followed by an identifier and an optional fixed range. Gameplay sends
//     registered sounds by id; `/playsound` sends even registered ones by name.
//   * Positions are eighths of a block, and the conversion **truncates toward
//     zero**: a walker at x = -0.7736 is sent as -6, and one at 0.9528 as 7 —
//     floor would give -7, rounding 8.
//   * **Stop Sound is 0x63**: a flags byte (1 source, 2 sound), then the
//     source as a category index, then the name.
//   * **Entity Sound Effect is 0x61 in the archive and was not observed** in
//     the 42 gestures captured. Its layout is written from the archive and is
//     refused nowhere — but it is not a measurement, and docs/provenance/son.md
//     says so.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::net {

namespace clientbound {
/// From the archive; never seen in a capture (see above).
inline constexpr i32 kEntitySoundEffect = 0x61;
/// Measured.
inline constexpr i32 kSoundEffect = 0x62;
/// Measured.
inline constexpr i32 kStopSound = 0x63;
}  // namespace clientbound

/// Mojang's sound categories, in wire order (verified: block 4, hostile 5,
/// neutral 6, player 7, weather 3 in the capture).
namespace sound_category {
inline constexpr i32 kMaster  = 0;
inline constexpr i32 kMusic   = 1;
inline constexpr i32 kRecord  = 2;
inline constexpr i32 kWeather = 3;
inline constexpr i32 kBlock   = 4;
inline constexpr i32 kHostile = 5;
inline constexpr i32 kNeutral = 6;
inline constexpr i32 kPlayer  = 7;
inline constexpr i32 kAmbient = 8;
inline constexpr i32 kVoice   = 9;
}  // namespace sound_category

/// A sound as the packet names it: a registry id, or an identifier.
struct SoundRef {
    /// Index into minecraft:sound_event. -1 when the sound travels by name.
    i32                sound_id{-1};
    std::string        name;
    std::optional<f32> fixed_range;
};

struct SoundEffect {
    SoundRef sound;
    i32      category{sound_category::kMaster};
    /// Eighths of a block. See sound_coordinate().
    i32 x{0};
    i32 y{0};
    i32 z{0};
    f32 volume{1.0F};
    f32 pitch{1.0F};
    i64 seed{0};

    [[nodiscard]] Vec3d position() const noexcept {
        return Vec3d{static_cast<f64>(x) / 8.0, static_cast<f64>(y) / 8.0,
                     static_cast<f64>(z) / 8.0};
    }
};

struct EntitySoundEffect {
    SoundRef sound;
    i32      category{sound_category::kMaster};
    i32      entity_id{0};
    f32      volume{1.0F};
    f32      pitch{1.0F};
    i64      seed{0};
};

struct StopSound {
    /// A category index, or empty for every category.
    std::optional<i32> category;
    /// An identifier, or empty for every sound.
    std::string sound;
};

/// World Event (0x25), as a client reads it. The encoder lives in chat.hpp.
struct WorldEvent {
    i32  event{0};
    i32  x{0};
    i32  y{0};
    i32  z{0};
    i32  data{0};
    bool global{false};
};

/// One coordinate in the packet's units: eighths of a block, truncated toward
/// zero as the capture shows. A float-to-int cast in C++ truncates the same way.
[[nodiscard]] i32 sound_coordinate(f64 blocks) noexcept;

[[nodiscard]] std::vector<u8> encode_sound_effect(const SoundEffect& sound);
[[nodiscard]] std::optional<SoundEffect> parse_sound_effect(std::span<const u8> payload);

[[nodiscard]] std::vector<u8> encode_entity_sound_effect(const EntitySoundEffect& sound);
[[nodiscard]] std::optional<EntitySoundEffect> parse_entity_sound_effect(
    std::span<const u8> payload);

[[nodiscard]] std::vector<u8> encode_stop_sound(const StopSound& stop);
[[nodiscard]] std::optional<StopSound> parse_stop_sound(std::span<const u8> payload);

[[nodiscard]] std::optional<WorldEvent> parse_world_event(std::span<const u8> payload);

}  // namespace ov::net
