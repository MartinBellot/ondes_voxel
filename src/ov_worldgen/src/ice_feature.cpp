#define OV_LOG_CATEGORY "worldgen"

// `iceberg`, `blue_ice`, `ice_spike`. Not implemented yet; every type is left
// unclaimed and so refused by name in feature.cpp.

#include "overworld_feature.hpp"

namespace ov::worldgen {

ClaimedFeature parse_ice_feature(std::string_view, Json, const registry::BlockRegistry&,
                                 const BlockTags&) {
    return std::nullopt;
}

}  // namespace ov::worldgen
