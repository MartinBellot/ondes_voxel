#include "ov/worldgen/structure_nbt.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::worldgen {

namespace {

void put(nbt::Tag& compound, std::string name, nbt::Tag value) {
    compound.compound()->push_back(nbt::CompoundEntry{std::move(name), std::move(value)});
}

[[nodiscard]] std::string_view piece_id(PieceKind kind) noexcept {
    switch (kind) {
        case PieceKind::Igloo: return "minecraft:iglu";
        case PieceKind::Shipwreck: return "minecraft:shipwreck";
        case PieceKind::OceanRuin: return "minecraft:orp";
        case PieceKind::RuinedPortal: return "minecraft:rupo";
        case PieceKind::BuriedTreasure: return "minecraft:btp";
        case PieceKind::NetherFossil: return "minecraft:nefos";
        case PieceKind::Jigsaw: return "minecraft:jigsaw";  // ── jigsaw ──
    }
    return "minecraft:unknown";
}

}  // namespace

nbt::Tag piece_to_nbt(const StructurePiece& piece) {
    if (piece.kind == PieceKind::Jigsaw) {  // ── jigsaw ── jigsaw_nbt.cpp
        return jigsaw_piece_to_nbt(piece);
    }
    nbt::Tag out = nbt::Tag::make_compound();
    put(out, "id", nbt::Tag{std::string{piece_id(piece.kind)}});
    put(out, "BB", nbt::Tag{nbt::Tag::IntArray{piece.box.min_x, piece.box.min_y, piece.box.min_z,
                                               piece.box.max_x, piece.box.max_y, piece.box.max_z}});
    put(out, "GD", nbt::Tag{i32{0}});
    // The orientation a template piece reports is south (2); the buried
    // treasure has none (-1). Read, never derived.
    put(out, "O", nbt::Tag{piece.kind == PieceKind::BuriedTreasure ? i32{-1} : i32{2}});
    if (piece.kind == PieceKind::BuriedTreasure) {
        return out;
    }

    // The igloo keeps the template position the start gave it; every other
    // kind stores where its height settled.
    const i32 stored_y =
        piece.kind == PieceKind::Igloo ? piece.generated_origin.y : piece.origin.y;
    put(out, "TPX", nbt::Tag{piece.origin.x});
    put(out, "TPY", nbt::Tag{stored_y});
    put(out, "TPZ", nbt::Tag{piece.origin.z});
    put(out, "Template", nbt::Tag{piece.template_name});

    switch (piece.kind) {
        case PieceKind::Igloo:
        case PieceKind::NetherFossil:
            put(out, "Rot", nbt::Tag{std::string{to_string(piece.rotation)}});
            break;
        case PieceKind::Shipwreck:
            put(out, "Rot", nbt::Tag{std::string{to_string(piece.rotation)}});
            put(out, "isBeached", nbt::Tag::make_bool(piece.beached));
            break;
        case PieceKind::OceanRuin:
            put(out, "Rot", nbt::Tag{std::string{to_string(piece.rotation)}});
            put(out, "Integrity", nbt::Tag{piece.integrity});
            put(out, "IsLarge", nbt::Tag::make_bool(piece.large));
            put(out, "BiomeType", nbt::Tag{std::string{piece.warm ? "WARM" : "COLD"}});
            break;
        case PieceKind::RuinedPortal: {
            put(out, "Rotation", nbt::Tag{std::string{to_string(piece.rotation)}});
            put(out, "Mirror", nbt::Tag{std::string{to_string(piece.mirror)}});
            put(out, "VerticalPlacement", nbt::Tag{piece.portal.placement});
            nbt::Tag properties = nbt::Tag::make_compound();
            put(properties, "cold", nbt::Tag::make_bool(piece.portal.cold));
            put(properties, "mossiness", nbt::Tag{piece.portal.mossiness});
            put(properties, "air_pocket", nbt::Tag::make_bool(piece.portal.air_pocket));
            put(properties, "overgrown", nbt::Tag::make_bool(piece.portal.overgrown));
            put(properties, "vines", nbt::Tag::make_bool(piece.portal.vines));
            put(properties, "replace_with_blackstone",
                nbt::Tag::make_bool(piece.portal.replace_with_blackstone));
            put(out, "Properties", std::move(properties));
            break;
        }
        case PieceKind::BuriedTreasure:
        case PieceKind::Jigsaw: break;  // ── jigsaw ── returned above
    }
    return out;
}

nbt::Tag start_to_nbt(const StructureStart& start) {
    nbt::Tag out = nbt::Tag::make_compound();
    put(out, "id", nbt::Tag{start.structure});
    put(out, "ChunkX", nbt::Tag{start.chunk_x});
    put(out, "ChunkZ", nbt::Tag{start.chunk_z});
    put(out, "references", nbt::Tag{i32{0}});
    nbt::Tag children = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const StructurePiece& piece : start.pieces) {
        children.list()->push_back(piece_to_nbt(piece));
    }
    put(out, "Children", std::move(children));
    return out;
}

nbt::Tag chunk_structures_to_nbt(std::span<const StructureStart>     starts,
                                 std::span<const StructureReference> references) {
    nbt::Tag starts_tag = nbt::Tag::make_compound();
    for (const StructureStart& start : starts) {
        if (!start.pieces.empty()) {
            put(starts_tag, start.structure, start_to_nbt(start));
        }
    }

    // One array per structure, in first-seen order, each position once.
    std::vector<std::pair<std::string_view, nbt::Tag::LongArray>> grouped;
    for (const StructureReference& reference : references) {
        auto found = std::ranges::find_if(grouped, [&](const auto& entry) {
            return entry.first == reference.structure;
        });
        if (found == grouped.end()) {
            grouped.emplace_back(reference.structure, nbt::Tag::LongArray{});
            found = std::prev(grouped.end());
        }
        const i64 packed = packed_chunk_pos(reference.chunk_x, reference.chunk_z);
        if (std::ranges::find(found->second, packed) == found->second.end()) {
            found->second.push_back(packed);
        }
    }
    nbt::Tag references_tag = nbt::Tag::make_compound();
    for (auto& [name, positions] : grouped) {
        put(references_tag, std::string{name}, nbt::Tag{std::move(positions)});
    }

    nbt::Tag out = nbt::Tag::make_compound();
    put(out, "References", std::move(references_tag));
    put(out, "starts", std::move(starts_tag));
    return out;
}

}  // namespace ov::worldgen
