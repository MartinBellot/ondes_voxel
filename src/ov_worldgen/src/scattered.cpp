#define OV_LOG_CATEGORY "worldgen"

#include "scattered.hpp"

#include <array>
#include <utility>

namespace ov::worldgen {

namespace {

void put(nbt::Tag& compound, std::string name, nbt::Tag value) {
    compound.compound()->push_back(nbt::CompoundEntry{std::move(name), std::move(value)});
}

/// The horizontal facings in the order the draw indexes them — north, east,
/// south, west — each as its 2D data value.
constexpr std::array<i32, 4> kDrawnFacing{2, 3, 0, 1};

/// West and east turn the piece: its depth runs along x.
[[nodiscard]] constexpr bool turned(i32 facing) noexcept {
    return facing == 1 || facing == 3;
}

}  // namespace

std::string_view scattered_piece_id(ScatteredKind kind) noexcept {
    switch (kind) {
        case ScatteredKind::SwampHut: return "minecraft:tesh";
        case ScatteredKind::DesertPyramid: return "minecraft:tedp";
        case ScatteredKind::JungleTemple: return "minecraft:tejp";
    }
    return "minecraft:unknown";
}

std::optional<ScatteredKind> scattered_kind_of(std::string_view id) noexcept {
    if (id == "minecraft:tesh") {
        return ScatteredKind::SwampHut;
    }
    if (id == "minecraft:tedp") {
        return ScatteredKind::DesertPyramid;
    }
    if (id == "minecraft:tejp") {
        return ScatteredKind::JungleTemple;
    }
    return std::nullopt;
}

ScatteredSize scattered_size(ScatteredKind kind) noexcept {
    // As the game stores them: `Width`, `Height`, `Depth`.
    switch (kind) {
        case ScatteredKind::SwampHut: return {7, 7, 9};
        case ScatteredKind::DesertPyramid: return {21, 15, 21};
        case ScatteredKind::JungleTemple: return {12, 10, 15};
    }
    return {};
}

std::optional<ScatteredKind> scattered_kind_for(StructureKind kind) noexcept {
    switch (kind) {
        case StructureKind::SwampHut: return ScatteredKind::SwampHut;
        case StructureKind::DesertPyramid: return ScatteredKind::DesertPyramid;
        case StructureKind::JungleTemple: return ScatteredKind::JungleTemple;
        default: return std::nullopt;
    }
}

StructurePiece make_scattered_piece(ScatteredKind kind, i32 chunk_x, i32 chunk_z,
                                    math::LegacyRandomSource& random) {
    const i32           facing = kDrawnFacing[static_cast<usize>(random.next_int(4))];
    const ScatteredSize size   = scattered_size(kind);
    const i32           size_x = turned(facing) ? size.depth : size.width;
    const i32           size_z = turned(facing) ? size.width : size.depth;

    StructurePiece piece;
    piece.kind                  = PieceKind::Scattered;
    piece.scattered.kind        = kind;
    piece.scattered.orientation = facing;
    piece.scattered.width       = size.width;
    piece.scattered.height      = size.height;
    piece.scattered.depth       = size.depth;
    piece.scattered.hpos        = -1;
    piece.origin                = {chunk_x * 16, kScatteredPlaceholderY, chunk_z * 16};
    piece.generated_origin      = piece.origin;
    piece.box = {piece.origin.x, piece.origin.y, piece.origin.z, piece.origin.x + size_x - 1,
                 piece.origin.y + size.height - 1, piece.origin.z + size_z - 1};
    return piece;
}

void scattered_to_nbt(const StructurePiece& piece, nbt::Tag& out) {
    const ScatteredPiece& s = piece.scattered;
    put(out, "Width", nbt::Tag{s.width});
    put(out, "Height", nbt::Tag{s.height});
    put(out, "Depth", nbt::Tag{s.depth});
    put(out, "HPos", nbt::Tag{s.hpos});
    switch (s.kind) {
        case ScatteredKind::SwampHut:
            put(out, "Witch", nbt::Tag::make_bool(s.witch));
            put(out, "Cat", nbt::Tag::make_bool(s.cat));
            break;
        case ScatteredKind::DesertPyramid:
            for (usize index = 0; index < s.placed_chests.size(); ++index) {
                put(out, "hasPlacedChest" + std::to_string(index),
                    nbt::Tag::make_bool(s.placed_chests[index]));
            }
            break;
        case ScatteredKind::JungleTemple:
            put(out, "placedMainChest", nbt::Tag::make_bool(s.placed_main_chest));
            put(out, "placedHiddenChest", nbt::Tag::make_bool(s.placed_hidden_chest));
            put(out, "placedTrap1", nbt::Tag::make_bool(s.placed_trap1));
            put(out, "placedTrap2", nbt::Tag::make_bool(s.placed_trap2));
            break;
    }
}

std::expected<void, std::string> scattered_from_nbt(const nbt::Tag& child, StructurePiece& piece) {
    ScatteredPiece& s      = piece.scattered;
    const auto      int_of = [&](std::string_view key) -> std::optional<i32> {
        const nbt::Tag* tag = child.find(key);
        return tag != nullptr ? std::optional<i32>{static_cast<i32>(tag->as_i64())} : std::nullopt;
    };
    const auto flag = [&](std::string_view key) {
        const nbt::Tag* tag = child.find(key);
        return tag != nullptr && tag->as_bool();
    };
    const auto o      = int_of("O");
    const auto width  = int_of("Width");
    const auto height = int_of("Height");
    const auto depth  = int_of("Depth");
    if (!o || !width || !height || !depth) {
        return std::unexpected(std::string{scattered_piece_id(s.kind)} +
                               ": a scattered piece without O, Width, Height or Depth");
    }
    s.orientation = *o;
    s.width       = *width;
    s.height      = *height;
    s.depth       = *depth;
    s.hpos        = int_of("HPos").value_or(-1);
    s.witch       = flag("Witch");
    s.cat         = flag("Cat");
    for (usize index = 0; index < s.placed_chests.size(); ++index) {
        s.placed_chests[index] = flag("hasPlacedChest" + std::to_string(index));
    }
    s.placed_main_chest   = flag("placedMainChest");
    s.placed_hidden_chest = flag("placedHiddenChest");
    s.placed_trap1        = flag("placedTrap1");
    s.placed_trap2        = flag("placedTrap2");
    piece.origin           = {piece.box.min_x, piece.box.min_y, piece.box.min_z};
    piece.generated_origin = piece.origin;
    piece.height_settled   = s.hpos != -1;
    return {};
}

}  // namespace ov::worldgen
