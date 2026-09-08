#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/biome_source.hpp"

#include "ov/base/log.hpp"

#include <simdjson.h>

#include <limits>
#include <set>

namespace ov::worldgen {

namespace {

/// A parameter is either a single number or a two-element range. Both appear
/// in the same file, often in the same entry.
[[nodiscard]] bool read_range(simdjson::simdjson_result<simdjson::dom::element> field, i64& low,
                              i64& high) {
    if (field.error() != simdjson::SUCCESS) {
        return false;
    }
    f64 single = 0.0;
    if (field.get(single) == simdjson::SUCCESS) {
        low  = BiomeSource::quantise(static_cast<f32>(single));
        high = low;
        return true;
    }
    simdjson::dom::array pair;
    if (field.get(pair) != simdjson::SUCCESS) {
        return false;
    }
    std::array<f64, 2> ends{};
    usize              index = 0;
    for (auto value : pair) {
        if (index >= ends.size() || value.get(ends[index]) != simdjson::SUCCESS) {
            return false;
        }
        ++index;
    }
    if (index != 2) {
        return false;
    }
    low  = BiomeSource::quantise(static_cast<f32>(ends[0]));
    high = BiomeSource::quantise(static_cast<f32>(ends[1]));
    return true;
}

}  // namespace

std::expected<BiomeSource, DensityError> BiomeSource::load(const std::filesystem::path& data_root,
                                                           std::string_view dimension) {
    const auto path = data_root / "reports" / "biome_parameters" / "minecraft" /
                      (std::string(dimension) + ".json");
    if (!std::filesystem::is_regular_file(path)) {
        OV_LOG_ERROR(
            "worldgen: {} is missing. The biome table is not in the datapack — it is exported "
            "by the official data generator into reports/biome_parameters.",
            path.string());
        return std::unexpected(DensityError::Missing);
    }

    auto text = simdjson::padded_string::load(path.string());
    if (text.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    simdjson::dom::parser parser;
    auto                  document = parser.parse(text.value());
    if (document.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }

    simdjson::dom::array raw;
    if (document.at_key("biomes").get(raw) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }

    // The order of the seven axes is fixed and shared with the sampler. Getting
    // two of them the wrong way round produces a world made of plausible
    // biomes in impossible places.
    constexpr std::array<const char*, 7> kAxes{"temperature", "humidity",  "continentalness",
                                               "erosion",     "depth",     "weirdness",
                                               "offset"};

    BiomeSource source;
    for (auto element : raw) {
        Entry            entry;
        std::string_view name;
        if (element.at_key("biome").get(name) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        entry.biome = std::string(name);

        auto parameters = element.at_key("parameters");
        if (parameters.error() != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        for (usize axis = 0; axis < kAxes.size(); ++axis) {
            if (!read_range(parameters.at_key(kAxes[axis]), entry.box[axis].low,
                            entry.box[axis].high)) {
                return std::unexpected(DensityError::Malformed);
            }
        }
        source.entries_.push_back(std::move(entry));
    }

    OV_LOG_INFO("worldgen: {} biome boxes, {} distinct biomes", source.entries_.size(),
                source.biome_count());
    return source;
}

usize BiomeSource::biome_count() const noexcept {
    std::set<std::string_view> names;
    for (const Entry& entry : entries_) {
        names.insert(entry.biome);
    }
    return names.size();
}

ClimatePoint BiomeSource::sample(const NoiseRouter& router, i32 quart_x, i32 quart_y,
                                 i32 quart_z) const {
    // Biomes live on a 4x4x4 grid, and the climate is read at the *block* that
    // grid cell starts on. Sampling at the quart coordinate itself would
    // compress the whole world into a sixty-fourth of itself.
    const FunctionContext at{quart_x * 4, quart_y * 4, quart_z * 4};

    const auto read = [&](const char* name) {
        const DensityFunction* function = router.entry(name);
        return function == nullptr ? 0.0F : static_cast<f32>(function->compute(at));
    };

    ClimatePoint point;
    point.coordinates[0] = quantise(read("temperature"));
    // The router calls it vegetation; the biome table calls the same axis
    // humidity.
    point.coordinates[1] = quantise(read("vegetation"));
    point.coordinates[2] = quantise(read("continents"));
    point.coordinates[3] = quantise(read("erosion"));
    point.coordinates[4] = quantise(read("depth"));
    // And weirdness is the router's ridges.
    point.coordinates[5] = quantise(read("ridges"));
    // The seventh axis is not sampled: it is measured against zero, so a box
    // with a non-zero offset is penalised by exactly that offset.
    point.coordinates[6] = 0;
    return point;
}

std::array<i64, 7> BiomeSource::gap_to(const ClimatePoint& climate,
                                       std::string_view    biome) const {
    i64                best = std::numeric_limits<i64>::max();
    std::array<i64, 7> gaps{};
    for (const Entry& entry : entries_) {
        if (entry.biome != biome) {
            continue;
        }
        std::array<i64, 7> here{};
        i64                total = 0;
        for (usize axis = 0; axis < entry.box.size(); ++axis) {
            here[axis] = entry.box[axis].distance(climate.coordinates[axis]);
            total += here[axis] * here[axis];
        }
        if (total < best) {
            best = total;
            gaps = here;
        }
    }
    return gaps;
}

i64 BiomeSource::distance_to(const ClimatePoint& climate, std::string_view biome) const {
    i64 best = std::numeric_limits<i64>::max();
    for (const Entry& entry : entries_) {
        if (entry.biome != biome) {
            continue;
        }
        i64 total = 0;
        for (usize axis = 0; axis < entry.box.size(); ++axis) {
            const i64 gap = entry.box[axis].distance(climate.coordinates[axis]);
            total += gap * gap;
        }
        best = std::min(best, total);
    }
    return best;
}

std::string_view BiomeSource::biome_at(const ClimatePoint& climate) const {
    i64          best     = std::numeric_limits<i64>::max();
    const Entry* nearest  = nullptr;

    for (const Entry& entry : entries_) {
        i64 total = 0;
        for (usize axis = 0; axis < entry.box.size(); ++axis) {
            const i64 gap = entry.box[axis].distance(climate.coordinates[axis]);
            total += gap * gap;
        }
        // Strictly less, so the first of several equally distant boxes wins —
        // which is the order the table itself is in.
        if (total < best) {
            best    = total;
            nearest = &entry;
        }
    }
    return nearest == nullptr ? std::string_view{} : std::string_view{nearest->biome};
}

}  // namespace ov::worldgen
