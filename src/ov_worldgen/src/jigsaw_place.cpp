// A pool element piece, written into a level chunk by chunk.
#include "jigsaw_impl.hpp"

#include "ov/worldgen/jigsaw.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <string>

namespace ov::worldgen {

namespace {

/// The processors every element gets, borrowed from the library.
struct Common {
    ProcessorRef ignore_structure_block;
    ProcessorRef ignore_structure_and_air;
    ProcessorRef jigsaw_replacement;
    ProcessorRef gravity;
};

void place_element(const Common& impl, const PoolElement& element,
                   StructureLevel& level, BlockPos position, Rotation rotation,
                   const BoundingBox& clip, FeatureRandom& random,
                   const registry::BlockRegistry& blocks, PiecePlaceResult& result) {
    switch (element.type) {
        case PoolElementType::Single:
        case PoolElementType::LegacySingle: {
            if (element.tpl == nullptr) {
                return;  // the empty template the game places for a missing one
            }
            if (!element.refusal.empty()) {
                result.unknown_markers.push_back(element.location + ": " + element.refusal);
            }
            PlaceSettings settings;
            settings.rotation = rotation;
            settings.mirror   = Mirror::None;
            settings.pivot    = {0, 0, 0};
            settings.clip     = clip;
            settings.random   = &random;
            const bool legacy = element.type == PoolElementType::LegacySingle;
            if (!legacy) {
                settings.processors.push_back(impl.ignore_structure_block);
            }
            settings.processors.push_back(impl.jigsaw_replacement);
            if (element.processor_list) {
                settings.processors.insert(settings.processors.end(),
                                           element.processor_list->processors.begin(),
                                           element.processor_list->processors.end());
            }
            if (element.projection == Projection::TerrainMatching) {
                settings.processors.push_back(impl.gravity);
            }
            // A legacy element trades the structure-block filter for one that
            // drops the air as well, and adds it last.
            if (legacy) {
                settings.processors.push_back(impl.ignore_structure_and_air);
            }
            const PlaceResult placed = place_template(level, *element.tpl, position, settings, blocks);
            result.written += placed.written;
            result.dropped += placed.dropped_by_processors;
            result.shaped.insert(result.shaped.end(), placed.shaped.begin(), placed.shaped.end());
            return;
        }
        case PoolElementType::List:
            for (const PoolElement& child : element.elements) {
                place_element(impl, child, level, position, rotation, clip, random, blocks, result);
            }
            return;
        case PoolElementType::Feature:
            if (clip.contains(position.x, position.y, position.z)) {
                result.unknown_markers.push_back("feature element " + element.feature +
                                                 " is not placed");
            }
            return;
        case PoolElementType::Empty: return;
    }
}

}  // namespace

PiecePlaceResult JigsawLibrary::place(StructureLevel& level, const StructurePiece& piece,
                                      const BoundingBox& clip, FeatureRandom& random,
                                      const registry::BlockRegistry& blocks) const {
    PiecePlaceResult result;
    if (piece.element == nullptr) {
        result.unknown_markers.push_back("jigsaw piece without an element");
        return result;
    }
    const Common common{impl_->ignore_structure_block, impl_->ignore_structure_and_air,
                        impl_->jigsaw_replacement, impl_->gravity};
    place_element(common, *piece.element, level, piece.origin, piece.rotation, clip, random,
                  blocks, result);
    return result;
}

}  // namespace ov::worldgen
