#define OV_LOG_CATEGORY "server"

#include "natural_spawning.hpp"

#include "ov/base/log.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace ov::server {
namespace {

/// The seven spawnable categories, under the names a biome file uses for them.
///
/// `misc` is deliberately absent: it never spawns naturally, and a file that
/// carried one would be describing something this table has no business
/// obeying.
constexpr std::array<std::pair<std::string_view, gameplay::MobCategory>, 7> kCategoryNames{{
    {"monster", gameplay::MobCategory::Monster},
    {"creature", gameplay::MobCategory::Creature},
    {"ambient", gameplay::MobCategory::Ambient},
    {"axolotls", gameplay::MobCategory::Axolotls},
    {"underground_water_creature", gameplay::MobCategory::UndergroundWaterCreature},
    {"water_creature", gameplay::MobCategory::WaterCreature},
    {"water_ambient", gameplay::MobCategory::WaterAmbient},
}};

}  // namespace

// ── Light ───────────────────────────────────────────────────────────────────

ChunkLight::ChunkLight(LightHooks hooks) : hooks_{std::move(hooks)} {}

u8 ChunkLight::block_light(BlockPos pos) const {
    return hooks_.block_light ? hooks_.block_light(pos) : u8{0};
}

u8 ChunkLight::sky_light(BlockPos pos) const {
    return hooks_.sky_light ? hooks_.sky_light(pos) : u8{0};
}

u8 ChunkLight::sky_darken() const { return hooks_.sky_darken ? hooks_.sky_darken() : u8{0}; }

u8 sky_darken_for(i64 day_time) noexcept {
    // Vanilla's shape: the day fraction offset by a quarter turn, run through a
    // cosine, clamped to [0, 1] and scaled by eleven. Written out in the order
    // the game applies it rather than simplified, because the offset and the
    // clamp are what make the number sit at 0 through the whole day and reach
    // 11 only around midnight.
    const auto  fraction = static_cast<f64>(((day_time % 24000) + 24000) % 24000) / 24000.0;
    const f64   angle    = (fraction - 0.25) * 2.0 * std::numbers::pi;
    const f64   raw      = 0.5 - std::cos(angle) * 0.5;
    const f64   clamped  = std::clamp(1.0 - (raw * 1.1 - 0.05), 0.0, 1.0);
    return static_cast<u8>(clamped * 11.0);
}

// ── The biome's lists ───────────────────────────────────────────────────────

bool load_biome_spawners(const std::filesystem::path& generated_root, std::string_view biome_name,
                         gameplay::NaturalSpawner& into, std::vector<std::string>& name_storage) {
    // Namespace stripped: the file is named by its path, not by its id.
    std::string_view bare = biome_name;
    if (const auto colon = bare.find(':'); colon != std::string_view::npos) {
        bare = bare.substr(colon + 1);
    }

    const auto path = generated_root / "data" / "minecraft" / "worldgen" / "biome" /
                      (std::string{bare} + ".json");
    if (!std::filesystem::exists(path)) {
        OV_LOG_WARN("no biome file at {} — natural spawning stays off", path.string());
        return false;
    }

    simdjson::dom::parser parser;
    const auto            document = parser.load(path.string());
    if (document.error() != simdjson::SUCCESS) {
        OV_LOG_WARN("could not parse {} — natural spawning stays off", path.string());
        return false;
    }

    simdjson::dom::element spawners;
    if (document.value_unsafe()["spawners"].get(spawners) != simdjson::SUCCESS) {
        OV_LOG_WARN("{} carries no `spawners` block — natural spawning stays off", path.string());
        return false;
    }

    // The names have to outlive this call: `SpawnerEntry::type_name` is a view,
    // and the parser's buffer is gone as soon as it goes out of scope. The
    // caller owns the storage for exactly that reason.
    name_storage.clear();
    name_storage.reserve(64);

    // Two passes, because reserving is not enough: `name_storage` handing out
    // views while it may still grow is how a spawner ends up naming a mob that
    // was never in the file. Every name is collected first and the vector is
    // never touched again afterwards.
    struct Raw {
        gameplay::MobCategory category{};
        usize                 name_index{0};
        i32                   weight{1};
        i32                   min_group{1};
        i32                   max_group{1};
    };
    std::vector<Raw> raw;

    usize loaded = 0;
    for (const auto& [key, category] : kCategoryNames) {
        simdjson::dom::array list;
        if (spawners[key].get(list) != simdjson::SUCCESS) {
            continue;
        }
        for (const simdjson::dom::element entry : list) {
            std::string_view type;
            if (entry["type"].get(type) != simdjson::SUCCESS) {
                continue;
            }
            i64 weight = 1;
            i64 min_c  = 1;
            i64 max_c  = 1;
            (void)entry["weight"].get(weight);
            (void)entry["minCount"].get(min_c);
            (void)entry["maxCount"].get(max_c);

            raw.push_back(Raw{category, name_storage.size(), static_cast<i32>(weight),
                              static_cast<i32>(min_c), static_cast<i32>(max_c)});
            name_storage.emplace_back(type);
            ++loaded;
        }
    }

    if (loaded == 0) {
        OV_LOG_WARN("{} lists no spawnable mob — natural spawning stays off", path.string());
        return false;
    }

    for (const auto& [key, category] : kCategoryNames) {
        std::vector<gameplay::SpawnerEntry> entries;
        for (const Raw& one : raw) {
            if (one.category != category) {
                continue;
            }
            entries.push_back(gameplay::SpawnerEntry{name_storage[one.name_index], one.weight,
                                                     one.min_group, one.max_group});
        }
        if (!entries.empty()) {
            into.set_entries(category, entries);
        }
    }

    OV_LOG_INFO("natural spawning: {} entries from {} (one biome's lists, applied everywhere)",
                loaded, path.string());
    return true;
}

}  // namespace ov::server
