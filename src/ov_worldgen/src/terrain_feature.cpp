#define OV_LOG_CATEGORY "worldgen"

// The rest of the surface. The implemented ones — vines, boulders, lava lakes,
// ice spikes — are in surface_feature.cpp; this file is where the ones that are
// refused on purpose are named, each with its reason, so that "not
// implemented" is never the whole story.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

ClaimedFeature parse_terrain_feature(std::string_view kind, Json config,
                                     const registry::BlockRegistry& blocks, const BlockTags& tags) {
    if (auto claimed = parse_surface_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    if (auto claimed = parse_bamboo_feature(kind, config, blocks, tags)) {
        return claimed;
    }
    if (kind == "monster_room") {
        // The dungeon's cobblestone is easy; its spawner and its two chests are
        // *block entities* — a mob type and a loot table — and a FeatureLevel
        // writes block states only. A dungeon without them would be a room
        // that looks right and is not one.
        OV_LOG_ERROR("worldgen: monster_room needs block entities (spawner, loot chests), which "
                     "FeatureLevel cannot write");
        return std::unexpected(FeatureError::Unsupported);
    }
    if (kind == "desert_well") {
        OV_LOG_ERROR("worldgen: desert_well belongs to the templated structures work, not here");
        return std::unexpected(FeatureError::Unsupported);
    }
    if (kind == "bonus_chest") {
        OV_LOG_ERROR("worldgen: bonus_chest is placed by the spawn logic when the world option "
                     "asks for it, and holds a loot-table block entity; out of scope here");
        return std::unexpected(FeatureError::Unsupported);
    }
    if (kind == "freeze_top_layer") {
        // Snow and ice need `Biome.shouldFreeze` / `shouldSnow`, which read the
        // biome's temperature *with* its height adjustment and the frozen-ocean
        // modifier — two PerlinSimplexNoise fields this generator does not
        // build. A guess here would put snow on the wrong side of every
        // mountain line.
        OV_LOG_ERROR("worldgen: freeze_top_layer needs the biome temperature noise, which is not "
                     "built");
        return std::unexpected(FeatureError::Unsupported);
    }
    return std::nullopt;
}

}  // namespace ov::worldgen
