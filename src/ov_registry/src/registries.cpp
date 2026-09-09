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

    // ── name → id, once ─────────────────────────────────────────────────────
    //
    // See the note on `entry_index_`: `protocol_id` used to be a linear scan
    // and its own comment said that was fine because nothing hot called it.
    // The natural spawner calls it once per spawn attempt, a couple of thousand
    // times a tick, and a profile put 19 of the 50 samples in the spawn pass
    // inside `memcmp` under it.
    //
    // The first entry wins on a duplicate name, which is what the linear scan
    // did — `std::ranges::find` returns the first match — so the two agree on a
    // malformed pack as well as on a good one.
    result.entry_index_.resize(result.registries_.size());
    for (usize i = 0; i < result.registries_.size(); ++i) {
        const Entry& entry = result.registries_[i];
        auto&        index = result.entry_index_[i];
        index.reserve(entry.entry_count);
        for (u32 offset = 0; offset < entry.entry_count; ++offset) {
            index.try_emplace(result.entry_names_[entry.entry_first + offset],
                              entry.first_id + static_cast<ProtocolId>(offset));
        }
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

    const auto* stacks = pack_at<u8>(data_view, header.stacks_offset, header.item_count);
    if (stacks == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }
    result.stack_sizes_ = std::span{stacks, header.item_count};

    // ── Entity types ────────────────────────────────────────────────────────
    const auto* entities =
        pack_at<EntityTypeRecord>(data_view, header.entities_offset, header.entity_count);
    if (entities == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }
    u32 attribute_total = 0;
    for (u32 i = 0; i < header.entity_count; ++i) {
        attribute_total = std::max<u32>(
            attribute_total, u32{entities[i].attribute_first} + entities[i].attribute_count);
    }
    const auto* entity_attrs =
        pack_at<EntityAttributeRecord>(data_view, header.entity_attrs_offset, attribute_total);
    if (entity_attrs == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }

    result.entity_attributes_.reserve(attribute_total);
    for (u32 i = 0; i < attribute_total; ++i) {
        result.entity_attributes_.push_back(
            EntityAttribute{static_cast<ProtocolId>(entity_attrs[i].attribute),
                            entity_attrs[i].base});
    }
    result.entity_types_.reserve(header.entity_count);
    for (u32 i = 0; i < header.entity_count; ++i) {
        const EntityTypeRecord& record = entities[i];
        result.entity_types_.push_back(EntityRecord{record.width, record.height,
                                                    record.eye_height, record.attribute_first,
                                                    record.attribute_count, record.measured});
    }

    // ── Recipes ─────────────────────────────────────────────────────────────
    const auto* recipe_records =
        pack_at<RecipeRecord>(data_view, header.recipes_offset, header.recipe_count);
    const auto* recipe_ingredients = pack_at<RecipeIngredientRecord>(
        data_view, header.recipe_ingredients_offset, header.recipe_ingredient_count);
    const auto* recipe_choices =
        pack_at<i32>(data_view, header.recipe_choices_offset, header.recipe_choice_count);
    const auto* fuel =
        pack_at<u16>(data_view, header.fuel_offset, kFuelKinds * header.item_count);
    const auto* remainder = pack_at<i32>(data_view, header.remainder_offset, header.item_count);
    if (recipe_records == nullptr || recipe_ingredients == nullptr ||
        recipe_choices == nullptr || fuel == nullptr || remainder == nullptr) {
        return std::unexpected{RegistryError::Corrupt};
    }

    // Every span a record points into is checked once, here. Checking at each
    // lookup would put the same three comparisons inside the matching loop,
    // which runs once per grid cell per candidate recipe.
    for (u32 i = 0; i < header.recipe_count; ++i) {
        const RecipeRecord& record = recipe_records[i];
        if (static_cast<usize>(record.ingredient_first) + record.ingredient_count >
            header.recipe_ingredient_count) {
            return std::unexpected{RegistryError::Corrupt};
        }
        if (record.kind > static_cast<u8>(RecipeKind::Special)) {
            return std::unexpected{RegistryError::Corrupt};
        }
        // A shaped recipe whose cells do not fill its rectangle would read the
        // neighbouring recipe's ingredients as its own.
        if (record.kind == static_cast<u8>(RecipeKind::CraftingShaped) &&
            static_cast<u32>(record.width) * record.height != record.ingredient_count) {
            return std::unexpected{RegistryError::Corrupt};
        }
    }
    for (u32 i = 0; i < header.recipe_ingredient_count; ++i) {
        if (static_cast<usize>(recipe_ingredients[i].choice_first) +
                recipe_ingredients[i].choice_count >
            header.recipe_choice_count) {
            return std::unexpected{RegistryError::Corrupt};
        }
    }

    result.recipes_ = RecipeData{
        std::span{recipe_records, header.recipe_count},
        std::span{recipe_ingredients, header.recipe_ingredient_count},
        std::span{recipe_choices, header.recipe_choice_count},
        std::span{fuel, kFuelKinds * header.item_count},
        std::span{remainder, header.item_count},
    };
    result.strings_offset_ = header.strings_offset;
    result.strings_bytes_  = header.string_bytes;

    return result;
}

std::optional<Registries::EntityTypeInfo> Registries::entity_type(
    ProtocolId type) const noexcept {
    if (type < 0 || static_cast<usize>(type) >= entity_types_.size()) {
        return std::nullopt;
    }
    const EntityRecord& record = entity_types_[static_cast<usize>(type)];
    // Bit 0 is the only thing that says a box is real. Testing width != 0
    // instead would work today and break the day a type is measured at zero
    // width, which is exactly what a marker is.
    if ((record.measured & 0b01) == 0) {
        return std::nullopt;
    }
    return EntityTypeInfo{record.width, record.height, record.eye_height,
                          (record.measured & 0b10) != 0};
}

std::vector<Registries::EntityAttribute> Registries::entity_attributes(ProtocolId type) const {
    if (type < 0 || static_cast<usize>(type) >= entity_types_.size()) {
        return {};
    }
    const EntityRecord& record = entity_types_[static_cast<usize>(type)];
    const auto          first  = static_cast<usize>(record.attribute_first);
    const auto          count  = static_cast<usize>(record.attribute_count);
    if (first + count > entity_attributes_.size()) {
        return {};
    }
    return std::vector<EntityAttribute>{entity_attributes_.begin() + static_cast<isize>(first),
                                        entity_attributes_.begin() +
                                            static_cast<isize>(first + count)};
}

std::optional<f64> Registries::attribute_base(ProtocolId type,
                                              ProtocolId attribute) const noexcept {
    if (type < 0 || static_cast<usize>(type) >= entity_types_.size()) {
        return std::nullopt;
    }
    const EntityRecord& record = entity_types_[static_cast<usize>(type)];
    for (usize i = 0; i < record.attribute_count; ++i) {
        const usize index = static_cast<usize>(record.attribute_first) + i;
        if (index >= entity_attributes_.size()) {
            return std::nullopt;
        }
        if (entity_attributes_[index].attribute == attribute) {
            return entity_attributes_[index].base;
        }
    }
    return std::nullopt;
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

i8 Registries::max_stack_size(ProtocolId item) const noexcept {
    // Out of range, or an item the pack never measured: 64. That is the
    // majority answer and the safe direction — over-stacking one tool is
    // visible and fixable, under-stacking every block would make each chest
    // behave oddly.
    if (item < 0 || static_cast<usize>(item) >= stack_sizes_.size()) {
        return 64;
    }
    const u8 measured = stack_sizes_[static_cast<usize>(item)];
    return measured == 0 ? i8{64} : static_cast<i8>(measured);
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
    // Was a linear scan, on the stated grounds that this is "a load-time path —
    // resolving a datapack, not a hot loop". It stopped being one when the
    // natural spawner started resolving a mob type by name once per spawn
    // attempt; see the note on `entry_index_`.
    if (registry.value() >= entry_index_.size()) {
        return std::nullopt;
    }
    const auto& index = entry_index_[registry.value()];
    const auto  found = index.find(entry);
    if (found == index.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string_view Registries::entry_of(RegistryId registry, ProtocolId id) const noexcept {
    const auto       names = entries(registry);
    const ProtocolId index = id - first_id(registry);
    if (index < 0 || static_cast<usize>(index) >= names.size()) {
        return {};
    }
    return names[static_cast<usize>(index)];
}

// ── Recipes ─────────────────────────────────────────────────────────────────

std::string_view Registries::recipe_name(usize index) const noexcept {
    if (index >= recipes_.recipes.size()) {
        return {};
    }
    return string_at(data_, strings_offset_, strings_bytes_,
                     recipes_.recipes[index].name_offset);
}

std::string_view Registries::recipe_group(usize index) const noexcept {
    if (index >= recipes_.recipes.size()) {
        return {};
    }
    return string_at(data_, strings_offset_, strings_bytes_,
                     recipes_.recipes[index].group_offset);
}

std::string_view Registries::recipe_type(usize index) const noexcept {
    if (index >= recipes_.recipes.size()) {
        return {};
    }
    return string_at(data_, strings_offset_, strings_bytes_,
                     recipes_.recipes[index].type_offset);
}

u16 Registries::burn_ticks(ProtocolId item, FuelKind kind) const noexcept {
    const usize items = recipes_.remainder.size();
    if (item < 0 || static_cast<usize>(item) >= items) {
        return 0;
    }
    const usize offset = static_cast<usize>(kind) * items + static_cast<usize>(item);
    if (offset >= recipes_.fuel.size()) {
        return 0;
    }
    return recipes_.fuel[offset];
}

std::optional<ProtocolId> Registries::crafting_remainder(ProtocolId item) const noexcept {
    if (item < 0 || static_cast<usize>(item) >= recipes_.remainder.size()) {
        return std::nullopt;
    }
    const i32 left = recipes_.remainder[static_cast<usize>(item)];
    if (left < 0) {
        return std::nullopt;
    }
    return left;
}

}  // namespace ov::registry
