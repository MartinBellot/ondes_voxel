// The jigsaw assembler: a start piece, then its neighbours, breadth first.
//
// Every rule and every draw here was fixed against the pieces the game stored
// in the reference worlds' `structures.starts` — see the header and
// docs/provenance/jigsaw.md. Nothing is drawn that the game does not draw, and
// nothing is skipped that it draws: a shuffle of one element draws nothing, a
// shuffle of four draws three times, and those counts are the specification.
#define OV_LOG_CATEGORY "worldgen"

#include "ov/base/log.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/jigsaw.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <utility>

namespace ov::worldgen {

namespace {

constexpr std::string_view kEmptyPool = "minecraft:empty";

/// The jigsaw block a feature element carries: at its own position, facing
/// down, pointing into the empty pool.
[[nodiscard]] const JigsawConnector& feature_connector() {
    static const JigsawConnector connector{
        {0, 0, 0}, Direction::Down, Direction::South, true, "minecraft:bottom", "minecraft:empty",
        "minecraft:empty", "minecraft:air"};
    return connector;
}

/// A jigsaw block where a piece put it.
struct PlacedConnector {
    BlockPos               pos;
    Direction              front{Direction::North};
    Direction              top{Direction::Up};
    const JigsawConnector* data{nullptr};
};

/// `Util.shuffle`: from the end, each slot swapped with one drawn below it.
template<typename T>
void shuffle(std::vector<T>& items, math::LegacyRandomSource& random) {
    for (usize index = items.size(); index > 1; --index) {
        const auto drawn = static_cast<usize>(random.next_int(static_cast<i32>(index)));
        std::swap(items[index - 1], items[drawn]);
    }
}

/// The element's jigsaw blocks at a position and rotation, shuffled.
void connectors_of(const PoolElement& element, BlockPos pos, Rotation rotation,
                   math::LegacyRandomSource& random, std::vector<PlacedConnector>& out) {
    out.clear();
    switch (element.type) {
        case PoolElementType::Single:
        case PoolElementType::LegacySingle:
            for (const JigsawConnector& connector : element.connectors) {
                const BlockPos offset = transform(connector.pos, Mirror::None, rotation, {0, 0, 0});
                out.push_back({pos.offset(offset.x, offset.y, offset.z),
                               rotate(connector.front, rotation), rotate(connector.top, rotation),
                               &connector});
            }
            shuffle(out, random);
            return;
        case PoolElementType::List:
            connectors_of(element.elements.front(), pos, rotation, random, out);
            return;
        case PoolElementType::Feature: {
            const JigsawConnector& connector = feature_connector();
            out.push_back({pos, connector.front, connector.top, &connector});
            return;
        }
        case PoolElementType::Empty: return;
    }
}

/// Whether a child's jigsaw block `b` may join the parent's `a`: facing each
/// other, the tops agreeing unless the parent's joint rolls, and the parent's
/// target the child's name.
[[nodiscard]] bool can_attach(const PlacedConnector& a, const PlacedConnector& b) noexcept {
    return a.front == opposite(b.front) && (a.data->rollable || a.top == b.top) &&
           a.data->target == b.data->name;
}

/// The space a piece may still take: a box, less the boxes already placed in
/// it. Boxes are whole blocks, so "the candidate, shrunk by a quarter block,
/// lies in the space" is "every block of the candidate is in the box and in
/// none of the placed ones".
struct FreeSpace {
    BoundingBox              outer;
    std::vector<BoundingBox> taken;

    [[nodiscard]] bool fits(const BoundingBox& box) const noexcept {
        if (box.min_x < outer.min_x || box.max_x > outer.max_x || box.min_y < outer.min_y ||
            box.max_y > outer.max_y || box.min_z < outer.min_z || box.max_z > outer.max_z) {
            return false;
        }
        return std::ranges::none_of(taken,
                                    [&](const BoundingBox& other) { return other.intersects(box); });
    }
};

struct Grown {
    const PoolElement*          element{nullptr};
    BlockPos                    position;
    Rotation                    rotation{Rotation::None};
    BoundingBox                 box;
    i32                         ground_level_delta{1};
    std::vector<JigsawJunction> junctions;
};

/// `OV_JIGSAW_EXPANSION_HACK=0` switches the hack off: a measuring instrument,
/// read once, for the two stored starts whose boxes show no expansion at all
/// (docs/provenance/jigsaw.md). Not a supported setting.
[[nodiscard]] bool expansion_hack_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("OV_JIGSAW_EXPANSION_HACK");
        return value == nullptr || std::string_view{value} != "0";
    }();
    return enabled;
}

[[nodiscard]] i64 column_key(i32 x, i32 z) noexcept {
    return (static_cast<i64>(x) << 32) ^ static_cast<i64>(static_cast<u32>(z));
}

/// The start: height, rotation, template, and the named start jigsaw.
struct Begun {
    JigsawStartPoint point;
    Grown            piece;
    i32              anchor_y{0};
};

std::optional<Begun> begin(const JigsawLibrary& library, const JigsawConfig& config,
                           i32 chunk_x, i32 chunk_z, math::LegacyRandomSource& random,
                           const StructureWorldSampler* sampler, std::string* why) {
    const TemplatePool* start_pool = library.pool(config.start_pool);
    if (start_pool == nullptr || start_pool->weighted.empty()) {
        *why = config.name + ": start pool " + config.start_pool + " is missing or empty";
        return std::nullopt;
    }
    // The start height is a constant here and draws nothing (see the loader).
    const BlockPos start{chunk_x * 16, config.start_height, chunk_z * 16};
    const auto     rotation = static_cast<Rotation>(random.next_int(4));
    const PoolElement* element =
        start_pool->weighted[static_cast<usize>(
            random.next_int(static_cast<i32>(start_pool->weighted.size())))];
    if (element->type == PoolElementType::Empty) {
        *why = config.name + ": the start pool drew the empty element";
        return std::nullopt;
    }

    BlockPos offset{0, 0, 0};
    if (!config.start_jigsaw_name.empty()) {
        std::vector<PlacedConnector> connectors;
        connectors_of(*element, start, rotation, random, connectors);
        const auto named = std::ranges::find_if(connectors, [&](const PlacedConnector& c) {
            return c.data->name == config.start_jigsaw_name;
        });
        if (named == connectors.end()) {
            *why = config.name + ": the start piece has no jigsaw named " +
                   config.start_jigsaw_name;
            return std::nullopt;
        }
        offset = {named->pos.x - start.x, named->pos.y - start.y, named->pos.z - start.z};
    }
    const BlockPos position{start.x - offset.x, start.y - offset.y, start.z - offset.z};

    Begun begun;
    begun.piece.element  = element;
    begun.piece.position = position;
    begun.piece.rotation = rotation;
    begun.piece.box      = element->box(position, rotation);

    const BoundingBox& box      = begun.piece.box;
    const i32          centre_x = (box.max_x + box.min_x) / 2;
    const i32          centre_z = (box.max_z + box.min_z) / 2;
    i32                floor    = position.y;
    if (config.project_to) {
        if (sampler == nullptr) {
            *why = config.name + ": no sampler for the start height";
            return std::nullopt;
        }
        const bool sea_bed = *config.project_to == world::HeightmapType::OceanFloorWG;
        floor = start.y + (sea_bed ? sampler->ocean_floor_height(centre_x, centre_z)
                                   : sampler->surface_height(centre_x, centre_z));
    }
    // The piece's floor — its box's bottom plus its ground delta — goes to the
    // projected height. Unprojected, that lowers the piece by its ground delta:
    // every bastion of the reference world stands at 32 for a start height 33.
    const i32 dy = floor - (box.min_y + begun.piece.ground_level_delta);
    begun.piece.position.y += dy;
    begun.piece.box.move(0, dy, 0);
    begun.anchor_y = floor + offset.y;

    begun.point.element  = element;
    begun.point.position = begun.piece.position;
    begun.point.rotation = rotation;
    begun.point.box      = begun.piece.box;
    begun.point.anchor   = {centre_x, begun.anchor_y, centre_z};
    return begun;
}

}  // namespace

std::optional<JigsawStartPoint> JigsawLibrary::start_point(const JigsawConfig& config,
                                                           i64 level_seed, i32 chunk_x,
                                                           i32 chunk_z,
                                                           const StructureWorldSampler* sampler) const {
    math::LegacyRandomSource random{0};
    random.set_seed(large_feature_seed(level_seed, chunk_x, chunk_z));
    std::string why;
    auto        begun = begin(*this, config, chunk_x, chunk_z, random, sampler, &why);
    if (!begun) {
        return std::nullopt;
    }
    return begun->point;
}

std::expected<StructureStart, std::string> JigsawLibrary::assemble(
    const JigsawConfig& config, i64 level_seed, i32 chunk_x, i32 chunk_z,
    const StructureWorldSampler* sampler) const {
    math::LegacyRandomSource random{0};
    random.set_seed(large_feature_seed(level_seed, chunk_x, chunk_z));
    std::string why;
    auto        begun = begin(*this, config, chunk_x, chunk_z, random, sampler, &why);
    if (!begun) {
        return std::unexpected(why);
    }

    // Heights of the terrain under a joint: a pure function of the column, so
    // asked once per column.
    std::unordered_map<i64, i32> heights;
    bool                         no_sampler = false;
    const auto surface = [&](i32 x, i32 z) {
        if (sampler == nullptr) {
            no_sampler = true;
            return 0;
        }
        const auto [slot, inserted] = heights.try_emplace(column_key(x, z), 0);
        if (inserted) {
            slot->second = sampler->surface_height(x, z);
        }
        return slot->second;
    };

    std::vector<Grown> pieces;
    pieces.push_back(std::move(begun->piece));

    if (config.size > 0) {
        const BlockPos anchor = begun->point.anchor;
        const i32      reach  = config.max_distance;
        // Owned here, pointed at by the queue: a child placed in a space grows
        // in that same space.
        std::deque<FreeSpace> spaces;
        spaces.push_back({{anchor.x - reach, anchor.y - reach, anchor.z - reach, anchor.x + reach,
                           anchor.y + reach, anchor.z + reach},
                          {pieces.front().box}});

        struct Pending {
            usize      piece;
            FreeSpace* space;
            i32        depth;
        };
        std::deque<Pending> queue;
        queue.push_back({0, &spaces.front(), 0});

        std::vector<PlacedConnector> parent_connectors;
        std::vector<PlacedConnector> child_connectors;
        std::vector<const PoolElement*> candidates;
        std::array<Rotation, 4> rotations{};

        while (!queue.empty()) {
            const Pending pending = queue.front();
            queue.pop_front();
            // Copied: `pieces` grows below and would move the parent.
            const PoolElement& element  = *pieces[pending.piece].element;
            const BlockPos     position = pieces[pending.piece].position;
            const Rotation     rotation = pieces[pending.piece].rotation;
            const BoundingBox  box      = pieces[pending.piece].box;
            const bool         rigid    = element.projection == Projection::Rigid;
            const i32          min_y    = box.min_y;
            FreeSpace*         inside   = nullptr;

            connectors_of(element, position, rotation, random, parent_connectors);
            for (const PlacedConnector& parent : parent_connectors) {
                const Vec3i    step   = direction_offset(parent.front);
                const BlockPos facing = parent.pos.offset(step.x, step.y, step.z);
                const i32      rel_y  = parent.pos.y - min_y;
                i32            ground = -1;

                const TemplatePool* pool = this->pool(parent.data->pool);
                if (pool == nullptr || (pool->weighted.empty() && pool->name != kEmptyPool)) {
                    continue;
                }
                const TemplatePool* fallback = this->pool(pool->fallback);
                if (fallback == nullptr ||
                    (fallback->weighted.empty() && fallback->name != kEmptyPool)) {
                    continue;
                }

                FreeSpace* space = pending.space;
                if (box.contains(facing.x, facing.y, facing.z)) {
                    if (inside == nullptr) {
                        spaces.push_back({box, {}});
                        inside = &spaces.back();
                    }
                    space = inside;
                }

                candidates.clear();
                if (pending.depth != config.size) {
                    candidates.assign(pool->weighted.begin(), pool->weighted.end());
                    shuffle(candidates, random);
                }
                const usize own = candidates.size();
                candidates.insert(candidates.end(), fallback->weighted.begin(),
                                  fallback->weighted.end());
                {
                    // The fallback's list is shuffled on its own, after the pool's.
                    std::vector<const PoolElement*> tail(candidates.begin() +
                                                             static_cast<std::ptrdiff_t>(own),
                                                         candidates.end());
                    shuffle(tail, random);
                    std::ranges::copy(tail, candidates.begin() + static_cast<std::ptrdiff_t>(own));
                }

                bool placed = false;
                for (const PoolElement* candidate : candidates) {
                    if (candidate->type == PoolElementType::Empty) {
                        break;
                    }
                    rotations = {Rotation::None, Rotation::Clockwise90, Rotation::Clockwise180,
                                 Rotation::CounterClockwise90};
                    for (usize index = rotations.size(); index > 1; --index) {
                        const auto drawn =
                            static_cast<usize>(random.next_int(static_cast<i32>(index)));
                        std::swap(rotations[index - 1], rotations[drawn]);
                    }
                    for (const Rotation turn : rotations) {
                        connectors_of(*candidate, {0, 0, 0}, turn, random, child_connectors);
                        const BoundingBox local = candidate->box({0, 0, 0}, turn);

                        // The expansion hack: a short piece reserves room above
                        // it for the tallest thing its own jigsaw blocks could
                        // bring (the villages' decorations on the roads).
                        i32 headroom = 0;
                        if (config.expansion_hack && expansion_hack_enabled() &&
                            local.max_y - local.min_y + 1 <= 16) {
                            for (const PlacedConnector& own_connector : child_connectors) {
                                const Vec3i    out  = direction_offset(own_connector.front);
                                const BlockPos next = own_connector.pos.offset(out.x, out.y, out.z);
                                if (!local.contains(next.x, next.y, next.z)) {
                                    continue;
                                }
                                const TemplatePool* target = this->pool(own_connector.data->pool);
                                if (target == nullptr) {
                                    continue;
                                }
                                const TemplatePool* target_fallback = this->pool(target->fallback);
                                headroom = std::max({headroom, target->max_size,
                                                     target_fallback != nullptr
                                                         ? target_fallback->max_size
                                                         : 0});
                            }
                        }

                        for (const PlacedConnector& child : child_connectors) {
                            if (!can_attach(parent, child)) {
                                continue;
                            }
                            const BlockPos child_at{facing.x - child.pos.x, facing.y - child.pos.y,
                                                    facing.z - child.pos.z};
                            BoundingBox    child_box = candidate->box(child_at, turn);
                            const bool     child_rigid =
                                candidate->projection == Projection::Rigid;
                            const i32 child_y = child.pos.y;
                            const i32 delta   = rel_y - child_y + step.y;
                            i32       bottom  = 0;
                            if (rigid && child_rigid) {
                                bottom = min_y + delta;
                            } else {
                                if (ground == -1) {
                                    ground = surface(parent.pos.x, parent.pos.z);
                                }
                                bottom = ground - child_y;
                            }
                            const i32 shift = bottom - child_box.min_y;
                            child_box.move(0, shift, 0);
                            const BlockPos placed_at{child_at.x, child_at.y + shift, child_at.z};
                            if (headroom > 0) {
                                const i32 tall =
                                    std::max(headroom + 1, child_box.max_y - child_box.min_y);
                                child_box.max_y = std::max(child_box.max_y, child_box.min_y + tall);
                            }
                            if (!space->fits(child_box)) {
                                continue;
                            }
                            space->taken.push_back(child_box);

                            const i32 parent_delta = pieces[pending.piece].ground_level_delta;
                            const i32 child_delta  = child_rigid ? parent_delta - delta : 1;
                            i32       joint_y      = 0;
                            if (rigid) {
                                joint_y = min_y + rel_y;
                            } else if (child_rigid) {
                                joint_y = bottom + child_y;
                            } else {
                                if (ground == -1) {
                                    ground = surface(parent.pos.x, parent.pos.z);
                                }
                                joint_y = ground + delta / 2;
                            }
                            pieces[pending.piece].junctions.push_back(
                                {facing.x, joint_y - rel_y + parent_delta, facing.z, delta,
                                 candidate->projection});
                            Grown grown;
                            grown.element            = candidate;
                            grown.position           = placed_at;
                            grown.rotation           = turn;
                            grown.box                = child_box;
                            grown.ground_level_delta = child_delta;
                            grown.junctions.push_back({parent.pos.x,
                                                       joint_y - child_y + child_delta,
                                                       parent.pos.z, -delta, element.projection});
                            pieces.push_back(std::move(grown));
                            if (pending.depth + 1 <= config.size) {
                                queue.push_back({pieces.size() - 1, space, pending.depth + 1});
                            }
                            placed = true;
                            break;
                        }
                        if (placed) {
                            break;
                        }
                    }
                    if (placed) {
                        break;
                    }
                }
            }
        }
    }
    if (no_sampler) {
        return std::unexpected(config.name + ": terrain matching joints and no sampler");
    }

    StructureStart start;
    start.structure = config.name;
    start.chunk_x   = chunk_x;
    start.chunk_z   = chunk_z;
    start.pieces.reserve(pieces.size());
    for (Grown& grown : pieces) {
        StructurePiece piece;
        piece.kind               = PieceKind::Jigsaw;
        piece.template_name      = grown.element->location;
        piece.origin             = grown.position;
        piece.generated_origin   = grown.position;
        piece.rotation           = grown.rotation;
        piece.box                = grown.box;
        piece.height_settled     = true;
        piece.element            = grown.element;
        piece.ground_level_delta = grown.ground_level_delta;
        piece.junctions          = std::move(grown.junctions);
        start.pieces.push_back(std::move(piece));
    }
    start.box = start.pieces.front().box;
    for (const StructurePiece& piece : start.pieces) {
        start.box.encapsulate(piece.box);
    }
    return start;
}

}  // namespace ov::worldgen
