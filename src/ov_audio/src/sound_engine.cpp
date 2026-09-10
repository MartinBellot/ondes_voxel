#include "ov/audio/sound_engine.hpp"

#include "ov/audio/sound_catalog.hpp"
#include "ov/base/log.hpp"
#include "ov/base/thread.hpp"

#include "miniaudio_config.hpp"

#include <miniaudio.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <numbers>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ov::audio {

namespace {

/// Commands in flight between one frame and the next mixer callback. A frame
/// that starts more than this many sounds before the device drains them has
/// the rest refused as QueueFull — named, and counted.
constexpr u32 kRingSize = 256;

/// Frames decoded per refill of a streamed voice.
constexpr u32 kStreamChunk = 2048;

/// Vanilla's pitch range: /playsound documents values below 0.5 as 0.5, and
/// the argument stops at 2.
constexpr f32 kMinPitch = 0.5F;
constexpr f32 kMaxPitch = 2.0F;

constexpr usize kLogLimit = 4096;

enum : u8 { kPending = 0, kReady = 1, kFailed = 2 };

/// One file, decoded (a buffer) or held compressed (a stream).
struct DecodedFile {
    std::string      path;
    bool             stream{false};
    std::vector<i16> samples;
    std::vector<u8>  compressed;
    u32              channels{1};
    u32              rate{44100};
    u64              frames{0};
    PlayRefusal      failure{PlayRefusal::FileMissing};
    /// Written last by whoever decodes; everything above is visible to anyone
    /// who reads kReady here with acquire.
    std::atomic<u8> state{kPending};
};

struct Command {
    enum class Kind : u8 { Start, Stop, SetListener, SetVolumes };

    Kind                            kind{Kind::Start};
    const DecodedFile*              file{nullptr};
    i32                             stream_slot{-1};
    const SoundEvent*               event{nullptr};
    Vec3d                           position{};
    f32                             volume{1.0F};
    f32                             pitch{1.0F};
    i32                             attenuation_distance{16};
    SoundCategory                   category{SoundCategory::Master};
    bool                            relative{false};
    /// Stop: whether `category` filters.
    bool                            any_category{true};
    Listener                        listener{};
    std::array<f32, kCategoryCount> volumes{};
};

/// A decoder for one streamed voice.
///
/// The frame thread opens it (stb_vorbis allocates) before handing the slot
/// over, and closes it after the mixer says `finished`: the audio thread only
/// ever reads from an open decoder and never allocates or frees.
struct StreamSlot {
    stb_vorbis*        decoder{nullptr};
    const DecodedFile* file{nullptr};
    bool               busy{false};
    std::atomic<bool>  finished{false};

    // Audio thread only. Element 0 carries the last frame of the previous
    // chunk, so interpolation spans the seam.
    std::array<f32, (kStreamChunk + 1) * 2> buffer{};
    u32                                     buffered{0};
    bool                                    eof{false};
};

struct Voice {
    bool               active{false};
    const DecodedFile* file{nullptr};
    StreamSlot*        stream{nullptr};
    const SoundEvent*  event{nullptr};
    Vec3d              position{};
    f32                volume{1.0F};
    f32                pitch{1.0F};
    i32                attenuation_distance{16};
    SoundCategory      category{SoundCategory::Master};
    bool               relative{false};
    /// In source frames.
    f64 cursor{0.0};
    /// The gains last applied, so the next callback ramps from them instead of
    /// jumping — a jump is a click.
    f32  gain_l{0.0F};
    f32  gain_r{0.0F};
    bool fresh{true};
    u32  waited{0};
};

struct Gains {
    f32 left{0.0F};
    f32 right{0.0F};
};

[[nodiscard]] usize index_of(SoundCategory category) noexcept {
    return static_cast<usize>(category);
}

/// Everything the audio thread touches, and the ring the frame thread feeds it.
struct Mixer {
    u32 sample_rate{48000};
    u32 max_voices{0};
    u32 max_streams{0};

    std::array<Command, kRingSize> ring{};
    std::atomic<u32>               head{0};  // written by the frame thread
    std::atomic<u32>               tail{0};  // written by the audio thread

    std::vector<Voice>            voices;  // [0, max_voices) buffers, then one per stream slot
    std::unique_ptr<StreamSlot[]> streams;
    /// Which event each voice is playing, for is_playing() on the frame thread.
    std::unique_ptr<std::atomic<const SoundEvent*>[]> playing;

    Listener                        listener{};
    std::array<f32, kCategoryCount> volumes{};

    std::atomic<u32> active_voices{0};
    std::atomic<u32> active_streams{0};
    std::atomic<u64> dropped{0};
    std::atomic<u64> frames_mixed{0};

    [[nodiscard]] bool push(const Command& command) noexcept {
        const u32 h = head.load(std::memory_order_relaxed);
        const u32 t = tail.load(std::memory_order_acquire);
        if (h - t >= kRingSize) {
            return false;
        }
        ring[h % kRingSize] = command;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    void drain() noexcept {
        u32       t = tail.load(std::memory_order_relaxed);
        const u32 h = head.load(std::memory_order_acquire);
        while (t != h) {
            apply(ring[t % kRingSize]);
            ++t;
        }
        tail.store(t, std::memory_order_release);
    }

    void release(usize index) noexcept {
        Voice& voice = voices[index];
        if (!voice.active) {
            return;
        }
        voice.active = false;
        playing[index].store(nullptr, std::memory_order_release);
        if (voice.stream != nullptr) {
            voice.stream->finished.store(true, std::memory_order_release);
            active_streams.fetch_sub(1, std::memory_order_relaxed);
        } else {
            active_voices.fetch_sub(1, std::memory_order_relaxed);
        }
        voice.stream = nullptr;
        voice.file   = nullptr;
    }

    void apply(const Command& command) noexcept {
        switch (command.kind) {
            case Command::Kind::Start: {
                usize index = voices.size();
                if (command.stream_slot >= 0) {
                    index = max_voices + static_cast<usize>(command.stream_slot);
                } else {
                    for (usize i = 0; i < max_voices; ++i) {
                        if (!voices[i].active) {
                            index = i;
                            break;
                        }
                    }
                }
                if (index >= voices.size() || voices[index].active) {
                    // Every voice is taken. The new sound is dropped and counted;
                    // stealing one would cut a sound off mid-word to make room.
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    if (command.stream_slot >= 0) {
                        streams[static_cast<usize>(command.stream_slot)].finished.store(
                            true, std::memory_order_release);
                    }
                    return;
                }
                Voice& voice               = voices[index];
                voice                      = Voice{};
                voice.active               = true;
                voice.file                 = command.file;
                voice.event                = command.event;
                voice.position             = command.position;
                voice.volume               = command.volume;
                voice.pitch                = command.pitch;
                voice.attenuation_distance = command.attenuation_distance;
                voice.category             = command.category;
                voice.relative             = command.relative;
                if (command.stream_slot >= 0) {
                    voice.stream           = &streams[static_cast<usize>(command.stream_slot)];
                    voice.stream->buffered = 0;
                    voice.stream->eof      = false;
                    active_streams.fetch_add(1, std::memory_order_relaxed);
                } else {
                    active_voices.fetch_add(1, std::memory_order_relaxed);
                }
                playing[index].store(command.event, std::memory_order_release);
                break;
            }
            case Command::Kind::Stop:
                for (usize i = 0; i < voices.size(); ++i) {
                    const Voice& voice = voices[i];
                    if (voice.active &&
                        (command.any_category || voice.category == command.category) &&
                        (command.event == nullptr || voice.event == command.event)) {
                        release(i);
                    }
                }
                break;
            case Command::Kind::SetListener:
                listener = command.listener;
                break;
            case Command::Kind::SetVolumes:
                volumes = command.volumes;
                break;
        }
    }

    [[nodiscard]] Gains target_gains(const Voice& voice, bool stereo_source) const noexcept {
        const f32 category = volumes[index_of(SoundCategory::Master)] *
                             (voice.category == SoundCategory::Master
                                  ? 1.0F
                                  : volumes[index_of(voice.category)]);
        f32 base = 0.0F;
        f32 pan  = 0.0F;
        if (voice.relative) {
            base = std::clamp(voice.volume, 0.0F, 1.0F);
        } else {
            const f64 dx       = voice.position.x - listener.position.x;
            const f64 dy       = voice.position.y - listener.position.y;
            const f64 dz       = voice.position.z - listener.position.z;
            const f64 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            base = SoundEngine::attenuation(distance, voice.volume, voice.attenuation_distance);
            if (distance > 1e-6) {
                // Minecraft's yaw: 0 faces +Z, 90 faces -X. The listener's right
                // hand points along (-cos yaw, 0, -sin yaw). Dividing by the full
                // distance and not the horizontal one centres a sound overhead.
                const f64 yaw     = static_cast<f64>(listener.yaw_degrees) * std::numbers::pi / 180.0;
                const f64 right_x = -std::cos(yaw);
                const f64 right_z = -std::sin(yaw);
                pan = static_cast<f32>(std::clamp((dx * right_x + dz * right_z) / distance, -1.0, 1.0));
            }
        }
        const f32 gain = base * category;
        if (stereo_source) {
            // A stereo file is not positioned — vanilla spatialises mono only —
            // so its two channels go out as they are.
            return Gains{gain, gain};
        }
        const f32 angle = (pan + 1.0F) * std::numbers::pi_v<f32> / 4.0F;
        return Gains{gain * std::cos(angle), gain * std::sin(angle)};
    }

    [[nodiscard]] static bool sample_buffer(const Voice& voice, f32& left, f32& right) noexcept {
        const DecodedFile& file = *voice.file;
        const u64          i0   = static_cast<u64>(voice.cursor);
        if (i0 >= file.frames) {
            return false;
        }
        const u64     i1    = std::min(i0 + 1, file.frames - 1);
        const f32     t     = static_cast<f32>(voice.cursor - static_cast<f64>(i0));
        const u64     ch    = file.channels;
        constexpr f32 kScale = 1.0F / 32768.0F;
        const f32     a     = static_cast<f32>(file.samples[i0 * ch]);
        const f32     b     = static_cast<f32>(file.samples[i1 * ch]);
        left                = (a + (b - a) * t) * kScale;
        if (ch >= 2) {
            const f32 c = static_cast<f32>(file.samples[i0 * ch + 1]);
            const f32 d = static_cast<f32>(file.samples[i1 * ch + 1]);
            right       = (c + (d - c) * t) * kScale;
        } else {
            right = left;
        }
        return true;
    }

    [[nodiscard]] static bool refill(StreamSlot& slot, u32 channels, f64& cursor) noexcept {
        if (slot.eof) {
            return false;
        }
        u32 carry = 0;
        if (slot.buffered > 0) {
            for (u32 c = 0; c < channels; ++c) {
                slot.buffer[c] = slot.buffer[(slot.buffered - 1) * channels + c];
            }
            carry = 1;
            cursor -= static_cast<f64>(slot.buffered - 1);
        }
        const int got = stb_vorbis_get_samples_float_interleaved(
            slot.decoder, static_cast<int>(channels), slot.buffer.data() + carry * channels,
            static_cast<int>(kStreamChunk * channels));
        if (got <= 0) {
            slot.eof      = true;
            slot.buffered = carry;
            return false;
        }
        slot.buffered = carry + static_cast<u32>(got);
        return true;
    }

    [[nodiscard]] static bool sample_stream(Voice& voice, f32& left, f32& right) noexcept {
        StreamSlot& slot     = *voice.stream;
        const u32   channels = voice.file->channels;
        while (static_cast<u64>(voice.cursor) + 1 >= slot.buffered) {
            if (!refill(slot, channels, voice.cursor)) {
                break;
            }
        }
        const u64 i0 = static_cast<u64>(voice.cursor);
        if (i0 >= slot.buffered) {
            return false;
        }
        const u64 i1 = std::min<u64>(i0 + 1, slot.buffered - 1);
        const f32 t  = static_cast<f32>(voice.cursor - static_cast<f64>(i0));
        const f32 a  = slot.buffer[i0 * channels];
        const f32 b  = slot.buffer[i1 * channels];
        left         = a + (b - a) * t;
        if (channels >= 2) {
            const f32 c = slot.buffer[i0 * channels + 1];
            const f32 d = slot.buffer[i1 * channels + 1];
            right       = c + (d - c) * t;
        } else {
            right = left;
        }
        return true;
    }

    void mix(f32* out, u32 frames) noexcept {
        drain();
        std::fill_n(out, static_cast<usize>(frames) * 2, 0.0F);
        const f32 inverse_frames = frames > 0 ? 1.0F / static_cast<f32>(frames) : 0.0F;
        for (usize i = 0; i < voices.size(); ++i) {
            Voice& voice = voices[i];
            if (!voice.active) {
                continue;
            }
            const DecodedFile* file = voice.file;
            if (voice.stream == nullptr) {
                const u8 state = file->state.load(std::memory_order_acquire);
                if (state == kPending) {
                    // Still being decoded. A second without it and it is given up:
                    // a sound that arrives that late is no longer about anything.
                    voice.waited += frames;
                    if (voice.waited > sample_rate) {
                        release(i);
                    }
                    continue;
                }
                if (state == kFailed) {
                    release(i);
                    continue;
                }
            }
            const Gains target = target_gains(voice, file->channels >= 2);
            if (voice.fresh) {
                voice.gain_l = target.left;
                voice.gain_r = target.right;
                voice.fresh  = false;
            }
            const f32 step_l = (target.left - voice.gain_l) * inverse_frames;
            const f32 step_r = (target.right - voice.gain_r) * inverse_frames;
            f32       gain_l = voice.gain_l;
            f32       gain_r = voice.gain_r;
            const f64 step   = static_cast<f64>(file->rate) / static_cast<f64>(sample_rate) *
                             static_cast<f64>(voice.pitch);
            bool finished = false;
            for (u32 k = 0; k < frames; ++k) {
                gain_l += step_l;
                gain_r += step_r;
                f32        left  = 0.0F;
                f32        right = 0.0F;
                const bool more  = voice.stream != nullptr ? sample_stream(voice, left, right)
                                                           : sample_buffer(voice, left, right);
                if (!more) {
                    finished = true;
                    break;
                }
                out[2 * static_cast<usize>(k)] += left * gain_l;
                out[2 * static_cast<usize>(k) + 1] += right * gain_r;
                voice.cursor += step;
            }
            voice.gain_l = target.left;
            voice.gain_r = target.right;
            if (finished) {
                release(i);
            }
        }
        frames_mixed.fetch_add(frames, std::memory_order_relaxed);
    }
};

void device_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
    static_cast<Mixer*>(device->pUserData)->mix(static_cast<f32*>(output), frames);
}

struct NameHash {
    using is_transparent = void;
    [[nodiscard]] usize operator()(std::string_view name) const noexcept {
        return std::hash<std::string_view>{}(name);
    }
};

}  // namespace

// ── names ───────────────────────────────────────────────────────────────────

namespace {
constexpr std::array<std::string_view, kCategoryCount> kCategoryNames{
    "master", "music", "record", "weather", "block",
    "hostile", "neutral", "player", "ambient", "voice"};
}  // namespace

std::string_view to_string(SoundCategory category) noexcept {
    const usize index = index_of(category);
    return index < kCategoryNames.size() ? kCategoryNames[index] : std::string_view{"?"};
}

std::optional<SoundCategory> category_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kCategoryNames.size(); ++i) {
        if (kCategoryNames[i] == name) {
            return static_cast<SoundCategory>(i);
        }
    }
    return std::nullopt;
}

std::string_view to_string(PlayRefusal refusal) noexcept {
    switch (refusal) {
        case PlayRefusal::UnknownEvent: return "unknown sound event";
        case PlayRefusal::NoVariant: return "the event resolves to no sound";
        case PlayRefusal::FileMissing: return "the sound file is not in the asset stack";
        case PlayRefusal::Undecodable: return "the sound file is not a readable Ogg Vorbis stream";
        case PlayRefusal::QueueFull: return "the mixer's command queue is full";
    }
    return "?";
}

// ── the engine ──────────────────────────────────────────────────────────────

struct SoundEngine::Impl {
    EngineDesc desc;
    bool       synchronous{false};
    Mixer      mixer;

    // Frame thread only.
    std::unordered_map<const SoundEntry*, std::unique_ptr<DecodedFile>>                  cache;
    std::unordered_map<std::string, std::unique_ptr<DecodedFile>, NameHash, std::equal_to<>> supplied;
    std::array<f32, kCategoryCount> volumes{};
    bool                            volumes_dirty{true};
    u64                             started{0};
    u64                             refused{0};
    std::deque<PlayedSound>         played;
    std::vector<Command>            deferred;

    struct PendingStream {
        DecodedFile* file{nullptr};
        Command      command{};
    };
    std::vector<PendingStream> pending_streams;

    // Frame thread -> loader thread.
    std::mutex               load_mutex;
    std::condition_variable  load_ready;
    std::deque<DecodedFile*> load_queue;
    bool                     load_stop{false};
    std::thread              loader;

    bool      has_device{false};
    ma_device device{};

    void load(DecodedFile& file) const {
        const auto bytes = desc.read(file.path);
        if (!bytes || bytes->empty()) {
            file.failure = PlayRefusal::FileMissing;
            file.state.store(kFailed, std::memory_order_release);
            return;
        }
        if (bytes->size() > static_cast<usize>(INT_MAX)) {
            file.failure = PlayRefusal::Undecodable;
            file.state.store(kFailed, std::memory_order_release);
            return;
        }
        const int length = static_cast<int>(bytes->size());
        if (file.stream) {
            int         error = 0;
            stb_vorbis* probe = stb_vorbis_open_memory(bytes->data(), length, &error, nullptr);
            if (probe == nullptr) {
                file.failure = PlayRefusal::Undecodable;
                file.state.store(kFailed, std::memory_order_release);
                return;
            }
            const stb_vorbis_info info = stb_vorbis_get_info(probe);
            stb_vorbis_close(probe);
            if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0) {
                file.failure = PlayRefusal::Undecodable;
                file.state.store(kFailed, std::memory_order_release);
                return;
            }
            file.channels   = static_cast<u32>(info.channels);
            file.rate       = info.sample_rate;
            file.compressed = std::move(*bytes);
        } else {
            int    channels = 0;
            int    rate     = 0;
            short* output   = nullptr;
            const int frames =
                stb_vorbis_decode_memory(bytes->data(), length, &channels, &rate, &output);
            if (frames < 0 || output == nullptr || channels < 1 || channels > 2 || rate <= 0) {
                std::free(output);  // NOLINT: stb_vorbis allocates with malloc
                file.failure = PlayRefusal::Undecodable;
                file.state.store(kFailed, std::memory_order_release);
                return;
            }
            const usize count = static_cast<usize>(frames) * static_cast<usize>(channels);
            file.samples.assign(output, output + count);
            std::free(output);  // NOLINT: stb_vorbis allocates with malloc
            file.channels = static_cast<u32>(channels);
            file.rate     = static_cast<u32>(rate);
            file.frames   = static_cast<u64>(frames);
        }
        file.state.store(kReady, std::memory_order_release);
    }

    DecodedFile* request(const SoundEntry& entry) {
        if (const auto it = cache.find(&entry); it != cache.end()) {
            return it->second.get();
        }
        if (const auto it = supplied.find(std::string_view{entry.name}); it != supplied.end()) {
            return it->second.get();
        }
        auto file    = std::make_unique<DecodedFile>();
        file->path   = entry.name;
        file->stream = entry.stream;
        DecodedFile* raw = file.get();
        cache.emplace(&entry, std::move(file));
        if (synchronous) {
            load(*raw);
        } else {
            {
                const std::scoped_lock lock{load_mutex};
                load_queue.push_back(raw);
            }
            load_ready.notify_one();
        }
        return raw;
    }

    void reclaim_streams() {
        for (u32 s = 0; s < mixer.max_streams; ++s) {
            StreamSlot& slot = mixer.streams[s];
            if (slot.busy && slot.finished.load(std::memory_order_acquire)) {
                stb_vorbis_close(slot.decoder);
                slot.decoder = nullptr;
                slot.file    = nullptr;
                slot.busy    = false;
                slot.finished.store(false, std::memory_order_relaxed);
            }
        }
    }

    void start_streams() {
        for (usize i = 0; i < pending_streams.size();) {
            PendingStream& pending = pending_streams[i];
            const u8       state   = pending.file->state.load(std::memory_order_acquire);
            if (state == kPending) {
                ++i;
                continue;
            }
            if (state == kFailed) {
                ++refused;
                pending_streams.erase(pending_streams.begin() + static_cast<isize>(i));
                continue;
            }
            if (!pending.file->stream) {
                // Supplied as PCM: a buffer, not a decoder.
                (void)mixer.push(pending.command);
                pending_streams.erase(pending_streams.begin() + static_cast<isize>(i));
                continue;
            }
            i32 free_slot = -1;
            for (u32 s = 0; s < mixer.max_streams; ++s) {
                if (!mixer.streams[s].busy) {
                    free_slot = static_cast<i32>(s);
                    break;
                }
            }
            if (free_slot < 0) {
                ++i;  // wait for a stream to finish
                continue;
            }
            StreamSlot& slot  = mixer.streams[static_cast<usize>(free_slot)];
            int         error = 0;
            slot.decoder      = stb_vorbis_open_memory(
                pending.file->compressed.data(), static_cast<int>(pending.file->compressed.size()),
                &error, nullptr);
            if (slot.decoder == nullptr) {
                ++refused;
                pending_streams.erase(pending_streams.begin() + static_cast<isize>(i));
                continue;
            }
            slot.file = pending.file;
            slot.busy = true;
            slot.finished.store(false, std::memory_order_relaxed);
            pending.command.stream_slot = free_slot;
            if (!mixer.push(pending.command)) {
                stb_vorbis_close(slot.decoder);
                slot.decoder = nullptr;
                slot.file    = nullptr;
                slot.busy    = false;
                ++i;  // the ring is full; next frame
                continue;
            }
            pending_streams.erase(pending_streams.begin() + static_cast<isize>(i));
        }
    }
};

SoundEngine::SoundEngine() : impl_(std::make_unique<Impl>()) {}

SoundEngine::~SoundEngine() {
    if (!impl_) {
        return;
    }
    Impl& impl = *impl_;
    // The device first: after uninit the callback can no longer run, so
    // nothing below races the mixer.
    if (impl.has_device) {
        ma_device_uninit(&impl.device);
        impl.has_device = false;
    }
    if (impl.loader.joinable()) {
        {
            const std::scoped_lock lock{impl.load_mutex};
            impl.load_stop = true;
        }
        impl.load_ready.notify_all();
        impl.loader.join();
    }
    for (u32 s = 0; s < impl.mixer.max_streams; ++s) {
        if (impl.mixer.streams[s].decoder != nullptr) {
            stb_vorbis_close(impl.mixer.streams[s].decoder);
            impl.mixer.streams[s].decoder = nullptr;
        }
    }
}

std::expected<std::unique_ptr<SoundEngine>, std::string> SoundEngine::create(
    const EngineDesc& desc) {
    if (desc.catalog == nullptr) {
        return std::unexpected(std::string{"no sound catalog"});
    }
    if (!desc.read) {
        return std::unexpected(std::string{"no asset reader"});
    }
    if (desc.max_voices == 0 || desc.sample_rate == 0) {
        return std::unexpected(std::string{"no voices, or no sample rate"});
    }

    std::unique_ptr<SoundEngine> self(new SoundEngine);
    Impl&                        impl = *self->impl_;
    impl.desc                         = desc;
    impl.synchronous                  = desc.backend == Backend::Null;
    impl.volumes.fill(1.0F);
    impl.deferred.reserve(32);
    // Bounded, and allocated once: a play past it is refused as QueueFull.
    impl.pending_streams.reserve(static_cast<usize>(desc.max_streams) * 4 + 4);

    Mixer& mixer      = impl.mixer;
    mixer.sample_rate = desc.sample_rate;
    mixer.max_voices  = desc.max_voices;
    mixer.max_streams = desc.max_streams;
    const usize total = static_cast<usize>(desc.max_voices) + desc.max_streams;
    mixer.voices.resize(total);
    mixer.streams = std::make_unique<StreamSlot[]>(std::max<usize>(desc.max_streams, 1));
    mixer.playing = std::make_unique<std::atomic<const SoundEvent*>[]>(total);
    mixer.volumes.fill(1.0F);

    if (desc.backend == Backend::Device) {
        // The loader captures the Impl, never `this`: see the unique_ptr trap
        // in the briefing — ~unique_ptr nulls impl_ before ~Impl runs.
        Impl* raw   = &impl;
        impl.loader = std::thread([raw]() {
            set_thread_role("ov-audio-load", ThreadRole::Io);
            for (;;) {
                DecodedFile* next = nullptr;
                {
                    std::unique_lock lock{raw->load_mutex};
                    raw->load_ready.wait(
                        lock, [raw]() { return raw->load_stop || !raw->load_queue.empty(); });
                    if (raw->load_stop) {
                        return;
                    }
                    next = raw->load_queue.front();
                    raw->load_queue.pop_front();
                }
                raw->load(*next);
            }
        });

        ma_device_config config  = ma_device_config_init(ma_device_type_playback);
        config.playback.format   = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate        = desc.sample_rate;
        config.dataCallback      = &device_callback;
        config.pUserData         = &impl.mixer;
        if (ma_device_init(nullptr, &config, &impl.device) != MA_SUCCESS) {
            return std::unexpected(std::string{"no audio output device could be opened"});
        }
        impl.has_device = true;
        if (ma_device_start(&impl.device) != MA_SUCCESS) {
            return std::unexpected(std::string{"the audio output device would not start"});
        }
        OV_LOG_INFO("audio: {} Hz stereo on '{}', {} voices + {} streams", desc.sample_rate,
                    impl.device.playback.name, desc.max_voices, desc.max_streams);
    }
    return self;
}

void SoundEngine::set_listener(const Listener& listener) noexcept {
    Command command;
    command.kind     = Command::Kind::SetListener;
    command.listener = listener;
    // A full ring loses one listener update; the next frame sends another.
    (void)impl_->mixer.push(command);
}

void SoundEngine::set_volume(SoundCategory category, f32 volume) noexcept {
    impl_->volumes[index_of(category)] = std::clamp(volume, 0.0F, 1.0F);
    impl_->volumes_dirty               = true;
}

f32 SoundEngine::volume(SoundCategory category) const noexcept {
    return impl_->volumes[index_of(category)];
}

void SoundEngine::add_pcm(std::string_view path, std::span<const i16> samples, u32 channels,
                          u32 rate) {
    auto file      = std::make_unique<DecodedFile>();
    file->path     = std::string{path};
    file->stream   = false;
    file->channels = std::clamp<u32>(channels, 1, 2);
    file->rate     = rate > 0 ? rate : 44100;
    file->samples.assign(samples.begin(), samples.end());
    file->frames = file->samples.size() / file->channels;
    file->state.store(kReady, std::memory_order_release);
    impl_->supplied.insert_or_assign(std::string{path}, std::move(file));
}

std::expected<void, PlayRefusal> SoundEngine::play(const PlayRequest& request) {
    Impl&      impl   = *impl_;
    const auto refuse = [&impl](PlayRefusal why) -> std::expected<void, PlayRefusal> {
        ++impl.refused;
        return std::unexpected(why);
    };

    const SoundEvent* event = impl.desc.catalog->find(request.event);
    if (event == nullptr) {
        return refuse(PlayRefusal::UnknownEvent);
    }
    const auto chosen = impl.desc.catalog->choose(*event, request.seed);
    if (!chosen || chosen->file == nullptr) {
        return refuse(PlayRefusal::NoVariant);
    }
    DecodedFile* file = impl.request(*chosen->file);
    if (file->state.load(std::memory_order_acquire) == kFailed) {
        return refuse(file->failure);
    }

    Command command;
    command.kind                 = Command::Kind::Start;
    command.file                 = file;
    command.event                = event;
    command.position             = request.position;
    command.volume               = request.volume * chosen->volume;
    command.pitch                = std::clamp(request.pitch * chosen->pitch, kMinPitch, kMaxPitch);
    command.attenuation_distance = chosen->file->attenuation_distance;
    command.category             = request.category;
    command.relative             = request.relative;

    if (file->stream) {
        if (impl.pending_streams.size() >= impl.pending_streams.capacity()) {
            return refuse(PlayRefusal::QueueFull);
        }
        impl.pending_streams.push_back(Impl::PendingStream{file, command});
        impl.start_streams();
    } else if (!impl.mixer.push(command)) {
        return refuse(PlayRefusal::QueueFull);
    }

    ++impl.started;
    if (impl.desc.keep_log) {
        if (impl.played.size() >= kLogLimit) {
            impl.played.pop_front();
        }
        impl.played.push_back(PlayedSound{event->id, file->path, request.category,
                                          request.position, command.volume, command.pitch,
                                          request.relative, file->stream});
    }
    return {};
}

void SoundEngine::stop(std::optional<SoundCategory> category, std::string_view event) {
    Impl&             impl   = *impl_;
    const SoundEvent* target = nullptr;
    if (!event.empty()) {
        target = impl.desc.catalog->find(event);
        if (target == nullptr) {
            return;  // nothing can be playing an event that does not exist
        }
    }
    std::erase_if(impl.pending_streams, [&](const Impl::PendingStream& pending) {
        return (!category || pending.command.category == *category) &&
               (target == nullptr || pending.command.event == target);
    });
    Command command;
    command.kind         = Command::Kind::Stop;
    command.any_category = !category.has_value();
    command.category     = category.value_or(SoundCategory::Master);
    command.event        = target;
    if (!impl.mixer.push(command) && impl.deferred.size() < impl.deferred.capacity()) {
        impl.deferred.push_back(command);  // a lost stop is a sound that never ends
    }
}

void SoundEngine::update() {
    Impl& impl = *impl_;
    usize sent = 0;
    while (sent < impl.deferred.size() && impl.mixer.push(impl.deferred[sent])) {
        ++sent;
    }
    impl.deferred.erase(impl.deferred.begin(), impl.deferred.begin() + static_cast<isize>(sent));
    if (impl.volumes_dirty) {
        Command command;
        command.kind    = Command::Kind::SetVolumes;
        command.volumes = impl.volumes;
        if (impl.mixer.push(command)) {
            impl.volumes_dirty = false;
        }
    }
    impl.reclaim_streams();
    impl.start_streams();
}

void SoundEngine::render(std::span<f32> out) {
    if (impl_->has_device) {
        return;
    }
    impl_->mixer.mix(out.data(), static_cast<u32>(out.size() / 2));
}

bool SoundEngine::is_playing(std::string_view event) const {
    const Impl&       impl   = *impl_;
    const SoundEvent* target = impl.desc.catalog->find(event);
    if (target == nullptr) {
        return false;
    }
    const Mixer& mixer = impl.mixer;
    for (usize i = 0; i < mixer.voices.size(); ++i) {
        if (mixer.playing[i].load(std::memory_order_acquire) == target) {
            return true;
        }
    }
    for (const auto& pending : impl.pending_streams) {
        if (pending.command.event == target) {
            return true;
        }
    }
    // Started this frame and not yet reached by the mixer.
    const u32 tail = mixer.tail.load(std::memory_order_acquire);
    const u32 head = mixer.head.load(std::memory_order_relaxed);
    for (u32 k = tail; k != head; ++k) {
        const Command& command = mixer.ring[k % kRingSize];
        if (command.kind == Command::Kind::Start && command.event == target) {
            return true;
        }
    }
    return false;
}

EngineStats SoundEngine::stats() const {
    const Impl& impl = *impl_;
    EngineStats out;
    out.active_voices    = impl.mixer.active_voices.load(std::memory_order_relaxed);
    out.active_streams   = impl.mixer.active_streams.load(std::memory_order_relaxed);
    out.started          = impl.started;
    out.refused          = impl.refused;
    out.dropped_no_voice = impl.mixer.dropped.load(std::memory_order_relaxed);
    out.frames_mixed     = impl.mixer.frames_mixed.load(std::memory_order_relaxed);
    out.cached_files     = impl.cache.size() + impl.supplied.size();
    for (const auto& [entry, file] : impl.cache) {
        if (file->state.load(std::memory_order_acquire) == kReady) {
            out.cached_bytes += file->samples.size() * sizeof(i16) + file->compressed.size();
        }
    }
    return out;
}

std::vector<PlayedSound> SoundEngine::take_log() {
    std::vector<PlayedSound> out(std::make_move_iterator(impl_->played.begin()),
                                 std::make_move_iterator(impl_->played.end()));
    impl_->played.clear();
    return out;
}

f32 SoundEngine::attenuation(f64 distance, f32 volume, i32 attenuation_distance) noexcept {
    if (volume <= 0.0F || attenuation_distance <= 0) {
        return 0.0F;
    }
    // /playsound: "for values greater than 1, the sound does not actually grow
    // louder, but its audible range (a 16-block radius at 1) is multiplied by
    // volume." The 16 is sounds.json's attenuation_distance, whose default it is.
    const f64 range   = static_cast<f64>(attenuation_distance) * std::max(1.0, static_cast<f64>(volume));
    const f64 falloff = std::clamp(1.0 - distance / range, 0.0, 1.0);
    return static_cast<f32>(std::min(1.0, static_cast<f64>(volume)) * falloff);
}

}  // namespace ov::audio
