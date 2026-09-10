// What a sound event is made of: sounds.json, read once.
//
// A sound event — `minecraft:block.stone.place` — is not a file. It is a list
// of weighted variants in the resource pack's sounds.json, each naming a file
// under `sounds/` and carrying its own volume, pitch, streaming flag and
// attenuation distance. The server sends the event and a seed; which of the
// four stone placing sounds is heard is decided here, on the client.
//
// The format is the one the wiki documents (Sounds.json, see
// docs/provenance/son.md). Two things it leaves open are decided here and
// named rather than hidden:
//
//   * a `"type": "event"` entry keeps its **own** weight, and its volume and
//     pitch multiply those of the variant it resolves to;
//   * the variant is chosen by `LegacyRandomSource(seed).next_int(total)`.
//     The packet documents the seed as "used to pick the sound variant"; the
//     generator behind it is not documented, and this choice is not verified
//     against the vanilla client, which is the only oracle for it.
//
// Pure data: no device, no decoder, no thread. Testable with a string.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::audio {

/// One element of an event's `sounds` array.
struct SoundEntry {
    enum class Kind : u8 {
        /// `name` is a pack-relative file: `assets/minecraft/sounds/x/y.ogg`.
        File,
        /// `name` is another event, `minecraft:x.y`.
        Event,
    };

    Kind        kind{Kind::File};
    std::string name;
    f32         volume{1.0F};
    f32         pitch{1.0F};
    i32         weight{1};
    bool        stream{false};
    i32         attenuation_distance{16};
    bool        preload{false};
};

/// One sound event: its variants, and the subtitle key it would show.
struct SoundEvent {
    std::string             id;
    std::string             subtitle;
    std::vector<SoundEntry> entries;
};

/// A variant picked for one play: the file, and the factors the entries on
/// the way to it contributed.
struct ChosenSound {
    const SoundEntry* file{nullptr};
    f32               volume{1.0F};
    f32               pitch{1.0F};
};

class SoundCatalog {
public:
    /// Parse a sounds.json document. `ns` is the namespace its keys and bare
    /// file names belong to — "minecraft" for the vanilla file.
    [[nodiscard]] static std::expected<SoundCatalog, std::string> parse(
        std::string_view json, std::string_view ns = "minecraft");

    /// The event, by full id (`minecraft:block.stone.place`) or bare path.
    [[nodiscard]] const SoundEvent* find(std::string_view id) const noexcept;

    /// Pick a variant for one play. Empty when the event is unknown, has no
    /// sounds, or every path through it ends in an unknown event — each of
    /// which the caller reports by name rather than playing silence.
    [[nodiscard]] std::optional<ChosenSound> choose(std::string_view id, i64 seed) const;
    /// The same, for an event already found. Allocates nothing: this is the
    /// form the engine calls once per sound, on the frame thread.
    [[nodiscard]] std::optional<ChosenSound> choose(const SoundEvent& event, i64 seed) const;

    /// Every file an event can resolve to, following references. For
    /// preloading and for tests.
    void files_of(std::string_view id, std::vector<const SoundEntry*>& out) const;

    /// Files marked `preload`, which the engine decodes before they are asked for.
    [[nodiscard]] std::vector<const SoundEntry*> preloaded() const;

    [[nodiscard]] usize event_count() const noexcept { return events_.size(); }
    [[nodiscard]] usize entry_count() const noexcept;

private:
    [[nodiscard]] std::optional<ChosenSound> choose_in(const SoundEvent& event, i64 seed,
                                                       int depth) const;
    void files_in(const SoundEvent& event, std::vector<const SoundEntry*>& out, int depth) const;

    /// Transparent, so a lookup by string_view builds no std::string: `find`
    /// runs for every sound the frame starts.
    struct NameHash {
        using is_transparent = void;
        [[nodiscard]] usize operator()(std::string_view name) const noexcept {
            return std::hash<std::string_view>{}(name);
        }
    };

    std::string                                                          ns_;
    std::unordered_map<std::string, SoundEvent, NameHash, std::equal_to<>> events_;
};

}  // namespace ov::audio
