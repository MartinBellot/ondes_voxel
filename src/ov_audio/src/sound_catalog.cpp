#include "ov/audio/sound_catalog.hpp"

#include "ov/math/random.hpp"

#include <simdjson.h>

#include <fmt/format.h>

#include <array>
#include <cstring>

namespace ov::audio {

namespace {

// A reference chain longer than this is a cycle in a broken pack; vanilla's own
// file never nests more than one level.
constexpr int kMaxDepth = 8;

/// `minecraft:block.stone.place` for a key or reference, whatever form it came in.
std::string qualify(std::string_view name, std::string_view ns) {
    if (name.find(':') != std::string_view::npos) {
        return std::string{name};
    }
    return fmt::format("{}:{}", ns, name);
}

/// `block/stone/place1` -> `assets/minecraft/sounds/block/stone/place1.ogg`,
/// and `other:x/y` -> `assets/other/sounds/x/y.ogg`.
std::string file_path(std::string_view name, std::string_view ns) {
    std::string_view space = ns;
    if (const auto colon = name.find(':'); colon != std::string_view::npos) {
        space = name.substr(0, colon);
        name  = name.substr(colon + 1);
    }
    return fmt::format("assets/{}/sounds/{}.ogg", space, name);
}

}  // namespace

std::expected<SoundCatalog, std::string> SoundCatalog::parse(std::string_view json,
                                                             std::string_view ns) {
    simdjson::dom::parser  parser;
    simdjson::padded_string padded{json};
    simdjson::dom::element root;
    if (const auto error = parser.parse(padded).get(root); error) {
        return std::unexpected(fmt::format("sounds.json: {}", simdjson::error_message(error)));
    }
    simdjson::dom::object top;
    if (root.get(top)) {
        return std::unexpected(std::string{"sounds.json: the top level is not an object"});
    }

    SoundCatalog catalog;
    catalog.ns_ = std::string{ns};
    for (const auto field : top) {
        SoundEvent event;
        event.id = qualify(field.key, ns);

        simdjson::dom::object body;
        if (field.value.get(body)) {
            return std::unexpected(fmt::format("sounds.json: {} is not an object", event.id));
        }
        std::string_view subtitle;
        if (!body["subtitle"].get(subtitle)) {
            event.subtitle = std::string{subtitle};
        }

        simdjson::dom::array sounds;
        if (!body["sounds"].get(sounds)) {
            for (const auto element : sounds) {
                SoundEntry entry;
                std::string_view name;
                if (!element.get(name)) {
                    entry.name = file_path(name, ns);
                    event.entries.push_back(std::move(entry));
                    continue;
                }
                simdjson::dom::object object;
                if (element.get(object) || object["name"].get(name)) {
                    return std::unexpected(
                        fmt::format("sounds.json: {} has an entry with no name", event.id));
                }
                std::string_view type = "file";
                (void)object["type"].get(type);
                if (type == "event") {
                    entry.kind = SoundEntry::Kind::Event;
                    entry.name = qualify(name, ns);
                } else if (type == "file") {
                    entry.name = file_path(name, ns);
                } else {
                    // A type this reader does not know is refused by name: read as
                    // a file it would be a path that exists nowhere, and the event
                    // would go silent without saying why.
                    return std::unexpected(fmt::format(
                        "sounds.json: {} has an entry of unknown type '{}'", event.id, type));
                }
                double number = 0.0;
                if (!object["volume"].get(number)) {
                    entry.volume = static_cast<f32>(number);
                }
                if (!object["pitch"].get(number)) {
                    entry.pitch = static_cast<f32>(number);
                }
                i64 integer = 0;
                if (!object["weight"].get(integer)) {
                    entry.weight = static_cast<i32>(integer);
                }
                if (!object["attenuation_distance"].get(integer)) {
                    entry.attenuation_distance = static_cast<i32>(integer);
                }
                bool flag = false;
                if (!object["stream"].get(flag)) {
                    entry.stream = flag;
                }
                if (!object["preload"].get(flag)) {
                    entry.preload = flag;
                }
                event.entries.push_back(std::move(entry));
            }
        }
        std::string key = event.id;
        catalog.events_.emplace(std::move(key), std::move(event));
    }
    return catalog;
}

const SoundEvent* SoundCatalog::find(std::string_view id) const noexcept {
    if (id.find(':') != std::string_view::npos) {
        const auto it = events_.find(id);
        return it == events_.end() ? nullptr : &it->second;
    }
    // A bare path is qualified in a stack buffer rather than a std::string.
    // Longer than any vanilla event by a factor of four; past it, not found.
    std::array<char, 256> buffer{};
    if (ns_.size() + 1 + id.size() > buffer.size()) {
        return nullptr;
    }
    std::memcpy(buffer.data(), ns_.data(), ns_.size());
    buffer[ns_.size()] = ':';
    std::memcpy(buffer.data() + ns_.size() + 1, id.data(), id.size());
    const auto it = events_.find(std::string_view{buffer.data(), ns_.size() + 1 + id.size()});
    return it == events_.end() ? nullptr : &it->second;
}

usize SoundCatalog::entry_count() const noexcept {
    usize count = 0;
    for (const auto& [id, event] : events_) {
        count += event.entries.size();
    }
    return count;
}

std::optional<ChosenSound> SoundCatalog::choose(std::string_view id, i64 seed) const {
    const SoundEvent* event = find(id);
    if (event == nullptr) {
        return std::nullopt;
    }
    return choose_in(*event, seed, 0);
}

std::optional<ChosenSound> SoundCatalog::choose(const SoundEvent& event, i64 seed) const {
    return choose_in(event, seed, 0);
}

std::optional<ChosenSound> SoundCatalog::choose_in(const SoundEvent& event, i64 seed,
                                                   int depth) const {
    if (depth > kMaxDepth || event.entries.empty()) {
        return std::nullopt;
    }
    i32 total = 0;
    for (const auto& entry : event.entries) {
        total += entry.weight > 0 ? entry.weight : 0;
    }
    if (total <= 0) {
        return std::nullopt;
    }
    math::LegacyRandomSource random{seed};
    i32                      pick = random.next_int(total);
    for (const auto& entry : event.entries) {
        const i32 weight = entry.weight > 0 ? entry.weight : 0;
        if (pick >= weight) {
            pick -= weight;
            continue;
        }
        if (entry.kind == SoundEntry::Kind::File) {
            return ChosenSound{&entry, entry.volume, entry.pitch};
        }
        const SoundEvent* target = find(entry.name);
        if (target == nullptr) {
            return std::nullopt;
        }
        // The referenced event draws from a generator of its own, seeded from
        // the first so that one seed still names one sound.
        auto inner = choose_in(*target, seed ^ static_cast<i64>(0x5DEECE66DLL), depth + 1);
        if (!inner) {
            return std::nullopt;
        }
        inner->volume *= entry.volume;
        inner->pitch *= entry.pitch;
        return inner;
    }
    return std::nullopt;
}

void SoundCatalog::files_of(std::string_view id, std::vector<const SoundEntry*>& out) const {
    if (const SoundEvent* event = find(id); event != nullptr) {
        files_in(*event, out, 0);
    }
}

void SoundCatalog::files_in(const SoundEvent& event, std::vector<const SoundEntry*>& out,
                            int depth) const {
    if (depth > kMaxDepth) {
        return;
    }
    for (const auto& entry : event.entries) {
        if (entry.kind == SoundEntry::Kind::File) {
            out.push_back(&entry);
        } else if (const SoundEvent* target = find(entry.name); target != nullptr) {
            files_in(*target, out, depth + 1);
        }
    }
}

std::vector<const SoundEntry*> SoundCatalog::preloaded() const {
    std::vector<const SoundEntry*> out;
    for (const auto& [id, event] : events_) {
        for (const auto& entry : event.entries) {
            if (entry.kind == SoundEntry::Kind::File && entry.preload) {
                out.push_back(&entry);
            }
        }
    }
    return out;
}

}  // namespace ov::audio
