#include "ov/registry/registries.hpp"

#include "pack_format.hpp"

#include "ov/io/file.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ov::registry {
namespace {

/// Read a NUL-terminated name out of the string blob.
///
/// The bound is the blob, not the file: a name offset pointing into the record
/// sections would otherwise produce a plausible string made of struct fields.
[[nodiscard]] std::string_view string_at(const std::vector<u8>& data, u32 blob_offset,
                                         u32 blob_bytes, u32 offset) noexcept {
    if (offset >= blob_bytes) {
        return {};
    }
    const auto* begin  = reinterpret_cast<const char*>(data.data()) + blob_offset + offset;
    const usize limit  = blob_bytes - offset;
    const usize length = ::strnlen(begin, limit);
    if (length == limit) {
        return {};  // unterminated: refuse rather than run on
    }
    return std::string_view{begin, length};
}

}  // namespace

std::expected<Registries, RegistryError> Registries::from_bytes(std::vector<u8> data) {
    if (data.size() < kHeaderSize) {
        return std::unexpected{RegistryError::Corrupt};
    }

    PackHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));

    if (std::memcmp(header.magic, "OVPK", 4) != 0) {
        return std::unexpected{RegistryError::Corrupt};
    }
    if (header.format_version != kFormatVersion) {
        return std::unexpected{RegistryError::VersionMismatch};
    }

    Registries result;
    result.data_                     = std::move(data);
    const std::vector<u8>& data_view = result.data_;

    const auto* records =
        pack_at<RegistryRecord>(data_view, header.registries_offset, header.registry_count);
    const auto* entry_offsets = pack_at<u32>(data_view, header.entries_offset, header.entry_count);
    if (records == nullptr || entry_offsets == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }
    if (header.strings_offset + header.string_bytes > data_view.size()) {
        return std::unexpected{RegistryError::Corrupt};
    }

    // Resolve every name once, at load. Doing it per query would undo the point
    // of a format that needs no parsing.
    result.entry_names_.reserve(header.entry_count);
    for (u32 i = 0; i < header.entry_count; ++i) {
        const std::string_view name =
            string_at(result.data_, header.strings_offset, header.string_bytes, entry_offsets[i]);
        if (name.empty()) {
            return std::unexpected{RegistryError::Corrupt};
        }
        result.entry_names_.push_back(name);
    }

    result.registries_.reserve(header.registry_count);
    for (u32 i = 0; i < header.registry_count; ++i) {
        const RegistryRecord& record = records[i];

        // A record whose span leaves the entry table would index past the end
        // of entry_names_ on every lookup.
        if (static_cast<usize>(record.entry_first) + record.entry_count >
            result.entry_names_.size()) {
            return std::unexpected{RegistryError::Corrupt};
        }

        const std::string_view name =
            string_at(result.data_, header.strings_offset, header.string_bytes, record.name_offset);
        if (name.empty()) {
            return std::unexpected{RegistryError::Corrupt};
        }

        result.registries_.push_back(Entry{name, record.entry_first, record.entry_count,
                                           static_cast<ProtocolId>(record.first_id)});
    }

    // ── Tags ────────────────────────────────────────────────────────────────
    const auto* tag_records = pack_at<TagRecord>(data_view, header.tags_offset, header.tag_count);
    const auto* members =
        pack_at<ProtocolId>(data_view, header.members_offset, header.member_count);
    if (tag_records == nullptr || members == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }
    result.members_ = std::span{members, header.member_count};

    result.tags_.reserve(header.tag_count);
    for (u32 i = 0; i < header.tag_count; ++i) {
        const TagRecord& record = tag_records[i];

        if (record.registry_index >= result.registries_.size()) {
            return std::unexpected{RegistryError::Corrupt};
        }
        if (static_cast<usize>(record.member_first) + record.member_count > header.member_count) {
            return std::unexpected{RegistryError::Corrupt};
        }

        const std::string_view name =
            string_at(result.data_, header.strings_offset, header.string_bytes, record.name_offset);
        if (name.empty()) {
            return std::unexpected{RegistryError::Corrupt};
        }

        result.tags_.push_back(Tag{name, static_cast<u16>(record.registry_index),
                                   record.member_first, record.member_count});
    }

    return result;
}

std::optional<TagId> Registries::find_tag(RegistryId       registry,
                                          std::string_view name) const noexcept {
    // Emitted sorted by (registry, name), which is exactly this comparison.
    const auto key = std::pair{static_cast<u16>(registry.value()), name};
    const auto it  = std::ranges::lower_bound(
        tags_, key, {}, [](const Tag& tag) { return std::pair{tag.registry_index, tag.name}; });
    if (it == tags_.end() || it->registry_index != registry.value() || it->name != name) {
        return std::nullopt;
    }
    return TagId{static_cast<u16>(std::distance(tags_.begin(), it))};
}

std::string_view Registries::tag_name(TagId tag) const noexcept {
    if (tag.value() >= tags_.size()) {
        return {};
    }
    return tags_[tag.value()].name;
}

std::span<const ProtocolId> Registries::tag_members(TagId tag) const noexcept {
    if (tag.value() >= tags_.size()) {
        return {};
    }
    const Tag& entry = tags_[tag.value()];
    return members_.subspan(entry.member_first, entry.member_count);
}

bool Registries::tag_contains(TagId tag, ProtocolId id) const noexcept {
    // The hot one: "is this block a log?" runs on every break, every fire tick,
    // every pathfinding step. Members are stored sorted so this is a binary
    // search rather than a scan of 375 ids.
    return std::ranges::binary_search(tag_members(tag), id);
}

std::expected<Registries, RegistryError> Registries::load(const std::filesystem::path& path) {
    auto bytes = io::read_file(path);
    if (!bytes) {
        return std::unexpected{RegistryError::FileNotFound};
    }
    return from_bytes(std::move(*bytes));
}

std::optional<RegistryId> Registries::find(std::string_view name) const noexcept {
    // The emitter writes registries in sorted order, so this is a binary search
    // over 66 entries rather than a hash table nobody would notice building.
    const auto it = std::ranges::lower_bound(registries_, name, {}, &Entry::name);
    if (it == registries_.end() || it->name != name) {
        return std::nullopt;
    }
    return RegistryId{static_cast<u16>(std::distance(registries_.begin(), it))};
}

std::string_view Registries::name(RegistryId registry) const noexcept {
    if (registry.value() >= registries_.size()) {
        return {};
    }
    return registries_[registry.value()].name;
}

usize Registries::size(RegistryId registry) const noexcept {
    if (registry.value() >= registries_.size()) {
        return 0;
    }
    return registries_[registry.value()].entry_count;
}

ProtocolId Registries::first_id(RegistryId registry) const noexcept {
    if (registry.value() >= registries_.size()) {
        return 0;
    }
    return registries_[registry.value()].first_id;
}

std::span<const std::string_view> Registries::entries(RegistryId registry) const noexcept {
    if (registry.value() >= registries_.size()) {
        return {};
    }
    const Entry& entry = registries_[registry.value()];
    return std::span{entry_names_}.subspan(entry.entry_first, entry.entry_count);
}

std::optional<ProtocolId> Registries::protocol_id(RegistryId       registry,
                                                  std::string_view entry) const noexcept {
    const auto names = entries(registry);
    // Linear: registry order is id order and cannot be sorted for searching,
    // and this is a load-time path — resolving a datapack, not a hot loop.
    const auto it = std::ranges::find(names, entry);
    if (it == names.end()) {
        return std::nullopt;
    }
    return first_id(registry) + static_cast<ProtocolId>(std::distance(names.begin(), it));
}

std::string_view Registries::entry_of(RegistryId registry, ProtocolId id) const noexcept {
    const auto       names = entries(registry);
    const ProtocolId index = id - first_id(registry);
    if (index < 0 || static_cast<usize>(index) >= names.size()) {
        return {};
    }
    return names[static_cast<usize>(index)];
}

}  // namespace ov::registry
