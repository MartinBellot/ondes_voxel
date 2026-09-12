// A pool element piece in a chunk's `structures.starts`, as 1.20.1 writes it —
// field names and tag types read on the reference worlds' stored starts.
#include "ov/worldgen/jigsaw.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <string>
#include <utility>

namespace ov::worldgen {

nbt::Tag jigsaw_piece_to_nbt(const StructurePiece& piece) {
    nbt::Tag out = nbt::Tag::make_compound();
    out.put("id", nbt::Tag{std::string{"minecraft:jigsaw"}});
    out.put("BB", nbt::Tag{nbt::Tag::IntArray{piece.box.min_x, piece.box.min_y, piece.box.min_z,
                                              piece.box.max_x, piece.box.max_y,
                                              piece.box.max_z}});
    out.put("GD", nbt::Tag{i32{0}});
    // A pool element piece has no orientation.
    out.put("O", nbt::Tag{i32{-1}});
    out.put("PosX", nbt::Tag{piece.origin.x});
    out.put("PosY", nbt::Tag{piece.origin.y});
    out.put("PosZ", nbt::Tag{piece.origin.z});
    out.put("ground_level_delta", nbt::Tag{piece.ground_level_delta});
    out.put("pool_element", piece.element != nullptr ? JigsawLibrary::element_to_nbt(*piece.element)
                                                     : nbt::Tag::make_compound());
    out.put("rotation", nbt::Tag{std::string{to_string(piece.rotation)}});
    nbt::Tag junctions = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const JigsawJunction& junction : piece.junctions) {
        nbt::Tag one = nbt::Tag::make_compound();
        one.put("source_x", nbt::Tag{junction.source_x});
        one.put("source_ground_y", nbt::Tag{junction.source_ground_y});
        one.put("source_z", nbt::Tag{junction.source_z});
        one.put("delta_y", nbt::Tag{junction.delta_y});
        one.put("dest_proj", nbt::Tag{std::string{to_string(junction.dest_projection)}});
        junctions.push(std::move(one));
    }
    out.put("junctions", std::move(junctions));
    return out;
}

std::expected<StructurePiece, std::string> jigsaw_piece_from_nbt(const JigsawLibrary& library,
                                                                 const nbt::Tag&      child) {
    StructurePiece  piece;
    piece.kind      = PieceKind::Jigsaw;
    const nbt::Tag* bb  = child.find("BB");
    const auto*     box = bb != nullptr ? bb->get_if<nbt::Tag::IntArray>() : nullptr;
    if (box == nullptr || box->size() != 6) {
        return std::unexpected(std::string{"jigsaw piece without BB"});
    }
    piece.box = {(*box)[0], (*box)[1], (*box)[2], (*box)[3], (*box)[4], (*box)[5]};
    const auto int_of = [&](std::string_view key, i32 fallback) {
        const nbt::Tag* tag = child.find(key);
        return tag != nullptr ? static_cast<i32>(tag->as_i64(fallback)) : fallback;
    };
    piece.origin             = {int_of("PosX", 0), int_of("PosY", 0), int_of("PosZ", 0)};
    piece.generated_origin   = piece.origin;
    piece.ground_level_delta = int_of("ground_level_delta", 1);
    piece.height_settled     = true;
    if (const nbt::Tag* rotation = child.find("rotation")) {
        piece.rotation = parse_rotation(rotation->as_string()).value_or(Rotation::None);
    }
    const nbt::Tag* element = child.find("pool_element");
    if (element == nullptr) {
        return std::unexpected(std::string{"jigsaw piece without pool_element"});
    }
    piece.element = library.element_from_nbt(*element);
    if (piece.element == nullptr) {
        return std::unexpected(std::string{"jigsaw piece whose element no pool has"});
    }
    piece.template_name = piece.element->location;
    if (const nbt::Tag* list = child.find("junctions"); list != nullptr && list->list() != nullptr) {
        for (const nbt::Tag& one : *list->list()) {
            JigsawJunction junction;
            const auto     field = [&](std::string_view key) {
                const nbt::Tag* tag = one.find(key);
                return tag != nullptr ? static_cast<i32>(tag->as_i64()) : 0;
            };
            junction.source_x        = field("source_x");
            junction.source_ground_y = field("source_ground_y");
            junction.source_z        = field("source_z");
            junction.delta_y         = field("delta_y");
            const nbt::Tag* projection = one.find("dest_proj");
            junction.dest_projection =
                projection != nullptr && projection->as_string() == "terrain_matching"
                    ? Projection::TerrainMatching
                    : Projection::Rigid;
            piece.junctions.push_back(junction);
        }
    }
    return piece;
}

}  // namespace ov::worldgen
