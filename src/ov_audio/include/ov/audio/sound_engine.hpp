// The sound engine: positioned voices, a mixer, and a device — or no device.
//
// Layer 14, beside the renderer and ignorant of it. It plays what it is told,
// where it is told: an event name, a position, a volume, a pitch, a category.
// It does not know what a block is or what a packet looks like; ov_client
// translates both into calls here.
//
// Threads. The frame thread calls everything public. The device's own
// real-time thread runs the mixer. They share one fixed-size command ring and a
// handful of atomics — no lock is taken on the audio thread, and nothing on it
// allocates: every voice, every command slot and every decoded buffer exists
// before the mixer can reach it. Files are decoded on a loader thread
// (ThreadRole::Io) the first time they are asked for, so a sound nobody has
// played yet costs a decode once and never a frame.
//
// The null backend is the same engine without a device: `render()` pulls the
// mixer by hand, decodes happen on the caller's thread, and the log of what
// started is what a test asserts on. CI has no sound card; the mixer it tests
// is still the one that runs.
//
// miniaudio and stb_vorbis are private to the .cpp; this header names neither.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::audio {

class SoundCatalog;

/// Mojang's sound categories, in wire order: the category varint of Sound
/// Effect and Entity Sound Effect is an index into this, and so is the
/// source of Stop Sound. Verified by content in docs/provenance/son.md.
enum class SoundCategory : u8 {
    Master,
    Music,
    Record,
    Weather,
    Block,
    Hostile,
    Neutral,
    Player,
    Ambient,
    Voice,
};

inline constexpr usize kCategoryCount = 10;

[[nodiscard]] std::string_view to_string(SoundCategory category) noexcept;
[[nodiscard]] std::optional<SoundCategory> category_from_name(std::string_view name) noexcept;

enum class Backend : u8 {
    /// The system's default output, through miniaudio.
    Device,
    /// No output at all. `render()` runs the mixer; decodes are synchronous.
    Null,
};

/// Reads a pack-relative file, `assets/minecraft/sounds/x.ogg`. The client
/// hands in its asset stack, so a resource pack's sounds win over the vanilla
/// ones exactly as its textures do.
using AssetReader = std::function<std::optional<std::vector<u8>>(std::string_view path)>;

struct EngineDesc {
    const SoundCatalog* catalog{nullptr};
    AssetReader         read;
    Backend             backend{Backend::Device};
    u32                 sample_rate{48000};
    /// Voice limits. Ours, not vanilla's: the numbers vanilla's own client
    /// uses are not documented, and a guess dressed as parity is worse than a
    /// bound that says it is ours. A sound that finds no voice is dropped and
    /// counted, never made to steal one.
    u32 max_voices{64};
    u32 max_streams{4};
    /// Keep a record of every play (`take_log`). Off by default: a record is
    /// two strings, and the frame loop must not allocate per sound.
    bool keep_log{false};
};

/// Where the ears are, and which way the head faces. Minecraft's angles: yaw 0
/// faces +Z and 90 faces -X; pitch 90 looks straight down.
///
/// The pan is the source's component along the head's right-hand axis, the
/// cross product of the look and up vectors. With no roll that axis is
/// horizontal whatever the pitch, so looking up or down never swaps ears —
/// which the tests prove rather than assume; the pitch is carried so that the
/// basis is the whole head and not a horizontal shortcut.
struct Listener {
    Vec3d position{};
    f32   yaw_degrees{0.0F};
    f32   pitch_degrees{0.0F};
};

/// What one mono source sends to each ear.
struct StereoGain {
    f32 left{0.0F};
    f32 right{0.0F};
};

struct PlayRequest {
    std::string_view event;
    SoundCategory    category{SoundCategory::Master};
    Vec3d            position{};
    /// The packet's volume and pitch, before the variant's own factors.
    f32 volume{1.0F};
    f32 pitch{1.0F};
    /// Picks the variant (see SoundCatalog::choose).
    i64 seed{0};
    /// Heard at the listener whatever the position: the interface, music.
    bool relative{false};
};

/// Why a play was not started. Each is counted and each is named; none is
/// silence that looks like success.
enum class PlayRefusal : u8 {
    /// The catalog has no such event.
    UnknownEvent,
    /// The event exists and resolves to nothing — an empty list, or a
    /// reference to an event that does not exist.
    NoVariant,
    /// The variant's file is absent from the asset stack (not imported:
    /// ov-assetimport --sounds, and --music for the music).
    FileMissing,
    /// The file exists and is not a Vorbis stream this decoder can read.
    Undecodable,
    /// The mixer's command ring is full — the frame asked for more sounds
    /// than the audio thread drained since the last one.
    QueueFull,
};

[[nodiscard]] std::string_view to_string(PlayRefusal refusal) noexcept;

/// One play that was started, as the engine understood it. Kept for every
/// backend (bounded), and what the tests and the --sound-log option read.
struct PlayedSound {
    std::string   event;
    std::string   file;
    SoundCategory category{SoundCategory::Master};
    Vec3d         position{};
    /// After the variant's factors, before distance and category volume.
    f32  volume{1.0F};
    /// Clamped to vanilla's 0.5 .. 2.0.
    f32  pitch{1.0F};
    bool relative{false};
    bool stream{false};
};

struct EngineStats {
    u32   active_voices{0};
    u32   active_streams{0};
    u64   started{0};
    u64   refused{0};
    /// Plays that reached the mixer and found every voice taken.
    u64   dropped_no_voice{0};
    u64   frames_mixed{0};
    usize cached_files{0};
    usize cached_bytes{0};
};

class SoundEngine {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<SoundEngine>, std::string> create(
        const EngineDesc& desc);

    SoundEngine(const SoundEngine&)            = delete;
    SoundEngine& operator=(const SoundEngine&) = delete;
    ~SoundEngine();

    void set_listener(const Listener& listener) noexcept;

    /// 0 .. 1. Master scales every category, itself included.
    void               set_volume(SoundCategory category, f32 volume) noexcept;
    [[nodiscard]] f32  volume(SoundCategory category) const noexcept;

    /// Start a sound. The variant is picked and its file decoded (or fetched
    /// from the cache) here; the mixer is only handed a finished buffer.
    [[nodiscard]] std::expected<void, PlayRefusal> play(const PlayRequest& request);

    /// Stop Sound: every playing sound that matches both filters. An empty
    /// filter matches everything, so `stop({}, {})` silences the lot.
    void stop(std::optional<SoundCategory> category, std::string_view event = {});

    /// Frame thread, once per frame. Reads back what the mixer finished.
    void update();

    /// Supply a file's samples directly: 16-bit interleaved PCM, 1 or 2
    /// channels. A later play resolving to `path` uses them instead of reading
    /// and decoding the file, and plays them as a buffer even when sounds.json
    /// marks the entry `stream`.
    ///
    /// For tests, above all: CI has no Vorbis encoder and may not hold a single
    /// Mojang .ogg, so this is how the mixer is fed a waveform whose every
    /// sample is known. Must be called before the first play of that path.
    void add_pcm(std::string_view path, std::span<const i16> samples, u32 channels, u32 rate);

    /// Null backend: run the mixer for `out.size() / 2` stereo frames, exactly
    /// as the device callback would. Ignored by the device backend.
    void render(std::span<f32> out);

    /// Whether any voice is playing this event (or has been asked to and is
    /// waiting for its file).
    [[nodiscard]] bool is_playing(std::string_view event) const;

    [[nodiscard]] EngineStats stats() const;

    /// What started since the last call.
    [[nodiscard]] std::vector<PlayedSound> take_log();

    /// Vanilla's distance model: linear, silent at `attenuation_distance`
    /// blocks times the volume when the volume exceeds 1, and the volume
    /// itself capped at 1 as the gain. See docs/provenance/son.md.
    [[nodiscard]] static f32 attenuation(f64 distance, f32 volume,
                                         i32 attenuation_distance) noexcept;

private:
    struct Impl;

    SoundEngine();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::audio
