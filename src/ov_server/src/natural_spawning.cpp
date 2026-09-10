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
    // Two stages, and the first one is the surprise.
    //
    // `time of day` is **not** the day fraction. The overworld runs its sun
    // through a shaping curve first, which is what makes dawn and dusk quick
    // and noon and midnight long:
    //
    //     d0 = frac(time / 24000 - 0.25)
    //     d1 = 0.5 - cos(d0 * pi) / 2
    //     timeOfDay = (d0 * 2 + d1) / 3
    //
    // Only then does the darkening come out of a second cosine, clamped and
    // scaled by eleven. The first version of this function used the raw
    // fraction and one cosine, which reads plausibly and is wrong at every hour
    // except the two it happens to cross: it gave **5 at time 0**, where the
    // game gives 0, so a freshly started server spawned monsters on lit grass
    // at dawn. Checked against the three hours the shape is pinned by:
    // 0 -> 0 (sunrise), 6000 -> 0 (noon), 18000 -> 11 (midnight).
    const auto  ticks    = static_cast<f64>(((day_time % 24000) + 24000) % 24000);
    const f64   fraction = ticks / 24000.0 - 0.25;
    const f64   d0       = fraction - std::floor(fraction);
    const f64   d1       = 0.5 - std::cos(d0 * std::numbers::pi) / 2.0;
    const f64   time_of_day = (d0 * 2.0 + d1) / 3.0;

    const f64 raw = 1.0 - (std::cos(time_of_day * 2.0 * std::numbers::pi) * 2.0 + 0.5);
    return static_cast<u8>(std::clamp(raw, 0.0, 1.0) * 11.0);
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

// ── mobs-2 ──────────────────────────────────────────────────────────────────

usize load_all_biome_spawners(const std::filesystem::path&      generated_root,
                              std::span<const std::string_view> biome_names,
                              gameplay::NaturalSpawner& into, std::vector<std::string>& name_storage) {
    struct Raw {
        u16                   biome{0};
        gameplay::MobCategory category{};
        usize                 name_index{0};
        i32                   weight{1};
        i32                   min_group{1};
        i32                   max_group{1};
    };
    std::vector<Raw> raw;
    name_storage.clear();
    const auto biome_dir = generated_root / "data" / "minecraft" / "worldgen" / "biome";

    simdjson::dom::parser parser;
    usize                 with_lists = 0;
    for (usize index = 0; index < biome_names.size(); ++index) {
        std::string_view bare = biome_names[index];
        if (const auto colon = bare.find(':'); colon != std::string_view::npos) {
            bare = bare.substr(colon + 1);
        }
        const auto path = biome_dir / (std::string{bare} + ".json");
        if (!std::filesystem::exists(path)) {
            OV_LOG_WARN("natural spawning: no biome file for {} — it spawns nothing",
                        biome_names[index]);
            continue;
        }
        const auto document = parser.load(path.string());
        simdjson::dom::element spawners;
        if (document.error() != simdjson::SUCCESS ||
            document.value_unsafe()["spawners"].get(spawners) != simdjson::SUCCESS) {
            OV_LOG_WARN("natural spawning: {} has no readable `spawners`", path.string());
            continue;
        }
        bool any = false;
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
                raw.push_back(Raw{static_cast<u16>(index), category, name_storage.size(),
                                  static_cast<i32>(weight), static_cast<i32>(min_c),
                                  static_cast<i32>(max_c)});
                name_storage.emplace_back(type);
                any = true;
            }
        }
        with_lists += any ? 1 : 0;
    }

    // Views are taken only now: `name_storage` never grows again.
    std::vector<gameplay::SpawnerEntry> entries;
    for (usize index = 0; index < biome_names.size(); ++index) {
        for (const auto& [key, category] : kCategoryNames) {
            entries.clear();
            for (const Raw& one : raw) {
                if (one.biome == index && one.category == category) {
                    entries.push_back(gameplay::SpawnerEntry{name_storage[one.name_index],
                                                             one.weight, one.min_group,
                                                             one.max_group});
                }
            }
            if (!entries.empty()) {
                into.set_biome_entries(static_cast<u16>(index), category, entries);
            }
        }
    }

    // `#minecraft:allows_surface_slime_spawns`: the swamps a slime may appear
    // on at the surface. Read from the generated tag, not listed here.
    const auto tag_path = generated_root / "data" / "minecraft" / "tags" / "worldgen" / "biome" /
                          "allows_surface_slime_spawns.json";
    usize slimy = 0;
    if (const auto tag = parser.load(tag_path.string()); tag.error() == simdjson::SUCCESS) {
        simdjson::dom::array values;
        if (tag.value_unsafe()["values"].get(values) == simdjson::SUCCESS) {
            for (const simdjson::dom::element value : values) {
                std::string_view name;
                if (value.get(name) != simdjson::SUCCESS) {
                    continue;
                }
                for (usize index = 0; index < biome_names.size(); ++index) {
                    if (biome_names[index] == name) {
                        into.set_surface_slimes(static_cast<u16>(index), true);
                        ++slimy;
                    }
                }
            }
        }
    } else {
        OV_LOG_WARN("natural spawning: no {} — no surface slimes", tag_path.string());
    }

    OV_LOG_INFO("natural spawning: {} entries over {} biomes, drawn from the biome of each "
                "position; {} surface-slime biomes",
                raw.size(), with_lists, slimy);
    return with_lists;
}
// ── end mobs-2 ──

}  // namespace ov::server
