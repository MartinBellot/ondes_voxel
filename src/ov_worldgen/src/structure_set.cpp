#include "ov/worldgen/structure_set.hpp"

#include "ov/base/log.hpp"
#include "ov/math/random.hpp"

#include <simdjson.h>

#include <algorithm>
#include <memory>

namespace ov::worldgen {
namespace {

/// Java's `long` multiply, wrapping. Signed overflow is undefined in C++ and
/// defined in Java, so every product here goes through the unsigned type and
/// comes back. Getting this wrong does not crash; it changes the world.
[[nodiscard]] i64 wrap_mul(i64 a, i64 b) noexcept {
    return static_cast<i64>(static_cast<u64>(a) * static_cast<u64>(b));
}

[[nodiscard]] i64 wrap_add(i64 a, i64 b) noexcept {
    return static_cast<i64>(static_cast<u64>(a) + static_cast<u64>(b));
}

}  // namespace

std::string_view to_string(SpreadType type) noexcept {
    switch (type) {
        case SpreadType::Linear:
            return "linear";
        case SpreadType::Triangular:
            return "triangular";
    }
    return "?";
}

std::string_view to_string(FrequencyReduction reduction) noexcept {
    switch (reduction) {
        case FrequencyReduction::Default:
            return "default";
        case FrequencyReduction::LegacyType1:
            return "legacy_type_1";
        case FrequencyReduction::LegacyType2:
            return "legacy_type_2";
        case FrequencyReduction::LegacyType3:
            return "legacy_type_3";
    }
    return "?";
}

std::string_view to_string(StructureSetError error) noexcept {
    switch (error) {
        case StructureSetError::Missing:
            return "worldgen/structure_set/ is missing";
        case StructureSetError::Malformed:
            return "a structure set file is not JSON of the expected shape";
        case StructureSetError::UnknownPlacement:
            return "a structure set uses a placement type we do not implement";
        case StructureSetError::UnknownSpread:
            return "a structure set uses a spread type or reducer we do not implement";
    }
    return "?";
}

i64 large_feature_seed(i64 level_seed, i32 chunk_x, i32 chunk_z) noexcept {
    math::LegacyRandomSource random{level_seed};
    // Two draws, in this order. `a` and `b` are named rather than inlined into
    // the expression below on purpose: C++ does not sequence the operands of
    // `^`, so writing `x * random.next_long() ^ z * random.next_long()` may
    // draw them in the other order and produce a different world with no
    // symptom. This is trap 2 of the project's list and it has been paid for.
    const i64 a = random.next_long();
    const i64 b = random.next_long();
    const i64 folded =
        wrap_mul(static_cast<i64>(chunk_x), a) ^ wrap_mul(static_cast<i64>(chunk_z), b) ^ level_seed;
    return folded;
}

i64 large_feature_with_salt(i64 level_seed, i32 chunk_x, i32 chunk_z, i32 salt) noexcept {
    i64 value = wrap_mul(static_cast<i64>(chunk_x), 341873128712LL);
    value     = wrap_add(value, wrap_mul(static_cast<i64>(chunk_z), 132897987541LL));
    value     = wrap_add(value, level_seed);
    value     = wrap_add(value, static_cast<i64>(salt));
    return value;
}

// ── RandomSpreadPlacement ───────────────────────────────────────────────────

ChunkPos RandomSpreadPlacement::candidate(i64 level_seed, i32 grid_x,
                                          i32 grid_z) const noexcept {
    math::LegacyRandomSource random{0};
    random.set_seed(large_feature_with_salt(level_seed, grid_x, grid_z, salt));

    const i32 range = spacing - separation;
    i32       off_x = 0;
    i32       off_z = 0;
    if (spread == SpreadType::Linear) {
        off_x = random.next_int(range);
        off_z = random.next_int(range);
    } else {
        // Four draws, in this exact order: x's two, then z's two. Named for the
        // same reason as above.
        const i32 x0 = random.next_int(range);
        const i32 x1 = random.next_int(range);
        const i32 z0 = random.next_int(range);
        const i32 z1 = random.next_int(range);
        off_x        = (x0 + x1) / 2;
        off_z        = (z0 + z1) / 2;
    }
    return ChunkPos{grid_x * spacing + off_x, grid_z * spacing + off_z};
}

bool RandomSpreadPlacement::is_candidate_chunk(i64 level_seed, i32 chunk_x,
                                               i32 chunk_z) const noexcept {
    const i32      grid_x = floor_div(chunk_x, spacing);
    const i32      grid_z = floor_div(chunk_z, spacing);
    const ChunkPos found  = candidate(level_seed, grid_x, grid_z);
    return found.x == chunk_x && found.z == chunk_z;
}

bool RandomSpreadPlacement::passes_frequency(i64 level_seed, i32 chunk_x,
                                             i32 chunk_z) const noexcept {
    if (frequency >= 1.0F) {
        return true;
    }
    math::LegacyRandomSource random{0};
    switch (reduction) {
        case FrequencyReduction::Default:
            random.set_seed(large_feature_with_salt(level_seed, chunk_x, chunk_z, salt));
            return random.next_float() < frequency;

        case FrequencyReduction::LegacyType1: {
            // The pillager outpost's, kept from 1.14: the chunk is folded by
            // hand rather than through either seeding helper, and one draw is
            // thrown away before the one that decides.
            const i32 i = chunk_x >> 4;
            const i32 j = chunk_z >> 4;
            random.set_seed(static_cast<i64>(i ^ (j << 4)) ^ level_seed);
            (void)random.next_int();
            return random.next_int(static_cast<i32>(1.0F / frequency)) == 0;
        }

        case FrequencyReduction::LegacyType2:
            // The buried treasure's fixed salt. Not the set's salt, which is 0
            // — using the set's would place treasure in a different ocean.
            random.set_seed(large_feature_with_salt(level_seed, chunk_x, chunk_z, 10387320));
            return random.next_float() < frequency;

        case FrequencyReduction::LegacyType3:
            // The mineshaft's. `next_double`, not `next_float`, and the other
            // seeding helper. Measured: 23 starts out of 23, with no
            // disagreement over 2 268 force-loaded chunks.
            random.set_seed(large_feature_seed(level_seed, chunk_x, chunk_z));
            return random.next_double() < static_cast<f64>(frequency);
    }
    return false;
}

// ── StructureSet ────────────────────────────────────────────────────────────

i32 StructureSet::total_weight() const noexcept {
    i32 total = 0;
    for (const StructureSetEntry& entry : entries) {
        total += entry.weight;
    }
    return total;
}

// ── Loading ─────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] std::optional<SpreadType> parse_spread(std::string_view text) {
    if (text == "linear") {
        return SpreadType::Linear;
    }
    if (text == "triangular") {
        return SpreadType::Triangular;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<FrequencyReduction> parse_reduction(std::string_view text) {
    if (text == "default") {
        return FrequencyReduction::Default;
    }
    if (text == "legacy_type_1") {
        return FrequencyReduction::LegacyType1;
    }
    if (text == "legacy_type_2") {
        return FrequencyReduction::LegacyType2;
    }
    if (text == "legacy_type_3") {
        return FrequencyReduction::LegacyType3;
    }
    return std::nullopt;
}

[[nodiscard]] std::string strip_namespace_prefix(std::string_view name) {
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? std::string{name} : std::string{name.substr(colon + 1)};
}

}  // namespace

std::expected<StructureSetRegistry, StructureSetError> StructureSetRegistry::load(
    const std::filesystem::path& data_root) {
    const auto directory = data_root / "worldgen" / "structure_set";
    if (!std::filesystem::is_directory(directory)) {
        OV_LOG_ERROR("worldgen: {} is missing", directory.string());
        return std::unexpected(StructureSetError::Missing);
    }

    // Sorted, so that two runs read the same sets in the same order. Nothing
    // downstream depends on the order today, but a set list that reshuffles
    // itself between runs is exactly the kind of thing that makes a parity
    // number move for no reason.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    StructureSetRegistry     registry;
    simdjson::dom::parser    parser;
    for (const auto& path : files) {
        auto text = simdjson::padded_string::load(path.string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        auto document = parser.parse(text.value());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }

        StructureSet set;
        set.name = "minecraft:" + path.stem().string();

        simdjson::dom::object placement;
        if (document.at_key("placement").get(placement) != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        std::string_view type;
        if (placement["type"].get(type) != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }

        if (type == "minecraft:random_spread") {
            RandomSpreadPlacement spread;
            i64                   value = 0;
            if (placement["spacing"].get(value) == simdjson::SUCCESS) {
                spread.spacing = static_cast<i32>(value);
            }
            if (placement["separation"].get(value) == simdjson::SUCCESS) {
                spread.separation = static_cast<i32>(value);
            }
            if (placement["salt"].get(value) == simdjson::SUCCESS) {
                spread.salt = static_cast<i32>(value);
            }
            std::string_view spread_name;
            if (placement["spread_type"].get(spread_name) == simdjson::SUCCESS) {
                auto parsed = parse_spread(spread_name);
                if (!parsed) {
                    OV_LOG_ERROR("worldgen: {} has spread type {}, which we do not implement",
                                 set.name, spread_name);
                    return std::unexpected(StructureSetError::UnknownSpread);
                }
                spread.spread = *parsed;
            }
            f64 frequency = 1.0;
            if (placement["frequency"].get(frequency) == simdjson::SUCCESS) {
                spread.frequency = static_cast<f32>(frequency);
            }
            std::string_view reduction_name;
            if (placement["frequency_reduction_method"].get(reduction_name) == simdjson::SUCCESS) {
                auto parsed = parse_reduction(reduction_name);
                if (!parsed) {
                    OV_LOG_ERROR("worldgen: {} has reducer {}, which we do not implement", set.name,
                                 reduction_name);
                    return std::unexpected(StructureSetError::UnknownSpread);
                }
                spread.reduction = *parsed;
            }
            simdjson::dom::object zone;
            if (placement["exclusion_zone"].get(zone) == simdjson::SUCCESS) {
                ExclusionZone     exclusion;
                std::string_view  other;
                if (zone["other_set"].get(other) == simdjson::SUCCESS) {
                    exclusion.other_set = std::string{other};
                }
                if (zone["chunk_count"].get(value) == simdjson::SUCCESS) {
                    exclusion.chunk_count = static_cast<i32>(value);
                }
                spread.exclusion = std::move(exclusion);
            }
            simdjson::dom::array offset;
            if (placement["locate_offset"].get(offset) == simdjson::SUCCESS) {
                usize index = 0;
                for (auto element : offset) {
                    if (index >= spread.locate_offset.size()) {
                        break;
                    }
                    i64 component = 0;
                    if (element.get(component) == simdjson::SUCCESS) {
                        spread.locate_offset[index] = static_cast<i32>(component);
                    }
                    ++index;
                }
            }
            if (spread.spacing <= spread.separation || spread.spacing <= 0) {
                OV_LOG_ERROR("worldgen: {} has spacing {} and separation {}", set.name,
                             spread.spacing, spread.separation);
                return std::unexpected(StructureSetError::Malformed);
            }
            set.spread = std::move(spread);
        } else if (type == "minecraft:concentric_rings") {
            ConcentricRingsPlacement rings;
            i64                      value = 0;
            if (placement["count"].get(value) == simdjson::SUCCESS) {
                rings.count = static_cast<i32>(value);
            }
            if (placement["distance"].get(value) == simdjson::SUCCESS) {
                rings.distance = static_cast<i32>(value);
            }
            if (placement["spread"].get(value) == simdjson::SUCCESS) {
                rings.spread = static_cast<i32>(value);
            }
            if (placement["salt"].get(value) == simdjson::SUCCESS) {
                rings.salt = static_cast<i32>(value);
            }
            std::string_view biomes;
            if (placement["preferred_biomes"].get(biomes) == simdjson::SUCCESS) {
                rings.preferred_biomes = std::string{biomes};
            }
            set.concentric = std::move(rings);
        } else {
            OV_LOG_ERROR("worldgen: {} has placement type {}, which we do not implement", set.name,
                         type);
            return std::unexpected(StructureSetError::UnknownPlacement);
        }

        simdjson::dom::array structures;
        if (document.at_key("structures").get(structures) != simdjson::SUCCESS) {
            return std::unexpected(StructureSetError::Malformed);
        }
        for (auto element : structures) {
            StructureSetEntry entry;
            std::string_view  name;
            if (element["structure"].get(name) != simdjson::SUCCESS) {
                return std::unexpected(StructureSetError::Malformed);
            }
            entry.structure = std::string{name};
            i64 weight      = 1;
            if (element["weight"].get(weight) == simdjson::SUCCESS) {
                entry.weight = static_cast<i32>(weight);
            }
            set.entries.push_back(std::move(entry));
        }

        registry.sets_.push_back(std::move(set));
    }

    return registry;
}

const StructureSet* StructureSetRegistry::find(std::string_view name) const noexcept {
    const std::string bare = strip_namespace_prefix(name);
    for (const StructureSet& set : sets_) {
        if (set.name == name || strip_namespace_prefix(set.name) == bare) {
            return &set;
        }
    }
    return nullptr;
}

const StructureSet* StructureSetRegistry::set_of(std::string_view structure) const noexcept {
    const std::string bare = strip_namespace_prefix(structure);
    for (const StructureSet& set : sets_) {
        for (const StructureSetEntry& entry : set.entries) {
            if (entry.structure == structure || strip_namespace_prefix(entry.structure) == bare) {
                return &set;
            }
        }
    }
    return nullptr;
}

}  // namespace ov::worldgen
