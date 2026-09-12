// The Great Pyramid's start: where it may stand, and every choice inside it.
// An original Ondes VOXEL structure — see great_pyramid.hpp.
#include "great_pyramid_impl.hpp"

#include "ov/worldgen/placement.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

namespace ov::worldgen {

using namespace pyramid;

namespace {

/// The overworld's. The sampler does not carry one, and a desert is only ever
/// in the overworld.
constexpr i32 kSeaLevel = 63;
constexpr i32 kScanTop  = 160;
constexpr i32 kScanLow  = kSeaLevel - 16;
constexpr std::string_view kDesert = "minecraft:desert";

constexpr std::array<std::string_view, 4> kFacings{"north", "east", "south", "west"};

/// The first free y above the generator's base terrain in a column, by a
/// coarse scan refined to the block. The noise alone — no carvers, no
/// surface: the question is where the ground is, not what it is made of.
[[nodiscard]] i32 terrain_height(const StructureWorldSampler& sampler, i32 x, i32 z) {
    if (!sampler.base_solid(x, kScanTop, z).has_value()) {
        return sampler.surface_height(x, z);
    }
    for (i32 y = kScanTop; y >= kScanLow; y -= 4) {
        if (sampler.base_solid(x, y, z).value_or(false)) {
            for (i32 above = std::min(y + 3, kScanTop); above > y; --above) {
                if (sampler.base_solid(x, above, z).value_or(false)) {
                    return above + 1;
                }
            }
            return y + 1;
        }
    }
    return kScanLow;
}

/// The placer's biome gate, asked with the heights answered by one number: the
/// plinth's. A vanilla structure's anchor is the chunk's surface, and the
/// footprint's surface is within `kMaxSpread` of it by the time this is asked —
/// close enough for the biome, which moves in cells of four.
class LevelledSampler final : public StructureWorldSampler {
public:
    LevelledSampler(const StructureWorldSampler& inner, i32 y) : inner_(&inner), y_(y) {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return inner_->biome_at(x, y, z);
    }
    [[nodiscard]] i32 surface_height(i32 /*x*/, i32 /*z*/) const override { return y_; }
    [[nodiscard]] i32 ocean_floor_height(i32 /*x*/, i32 /*z*/) const override { return y_; }
    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return inner_->base_solid(x, y, z);
    }

private:
    const StructureWorldSampler* inner_;
    i32                          y_;
};

/// Would a vanilla structure start near this site? The placer's own verdict,
/// chunk by chunk: villages and desert pyramids within ten chunks, a
/// mineshaft under the footprint, anything else within two chunks of it.
[[nodiscard]] bool near_vanilla(const StructurePlacer& placer, const StructureWorldSampler& sampler,
                                i64 seed, i32 chunk_x, i32 chunk_z, i32 base_y) {
    const LevelledSampler levelled{sampler, base_y};
    constexpr i32         kZone      = 10;
    constexpr i32         kFootprint = 4;
    constexpr i32         kMargin    = 2;
    for (i32 dz = -kZone; dz <= kZone; ++dz) {
        for (i32 dx = -kZone; dx <= kZone; ++dx) {
            const i32 reach = std::max(std::abs(dx), std::abs(dz));
            for (const StructurePlacementResult& result :
                 placer.decide(seed, chunk_x + dx, chunk_z + dz, &levelled)) {
                if (result.decision != PlacementDecision::PlacedByPlacement) {
                    continue;
                }
                if (result.set == "minecraft:villages" || result.set == "minecraft:desert_pyramids") {
                    return true;
                }
                const i32 limit =
                    result.set == "minecraft:mineshafts" ? kFootprint : kFootprint + kMargin;
                if (reach <= limit) {
                    return true;
                }
            }
        }
    }
    return false;
}

[[nodiscard]] std::expected<registry::BlockStateId, std::string> resolve(
    const registry::BlockRegistry& blocks, std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string_view>> properties = {}) {
    const auto block = blocks.find_block(name);
    if (!block) {
        return std::unexpected("unknown block " + std::string{name});
    }
    registry::BlockStateId state = blocks.default_state(*block);
    for (const auto& [key, value] : properties) {
        const auto property = blocks.find_property(*block, key);
        if (!property) {
            return std::unexpected(std::string{name} + " has no property " + std::string{key});
        }
        bool found = false;
        for (u16 index = 0; index < property->values.size(); ++index) {
            if (property->values[index] == value) {
                state = blocks.with_property(state, *property, index);
                found = true;
                break;
            }
        }
        if (!found) {
            return std::unexpected(std::string{name} + "[" + std::string{key} + "=" +
                                   std::string{value} + "] does not exist");
        }
    }
    return state;
}

[[nodiscard]] std::expected<Palette, std::string> resolve_palette(
    const registry::BlockRegistry& blocks) {
    Palette     out;
    std::string error;
    const auto  get = [&](registry::BlockStateId& slot, std::string_view name,
                         std::initializer_list<std::pair<std::string_view, std::string_view>>
                             properties = {}) {
        auto state = resolve(blocks, name, properties);
        if (!state) {
            if (error.empty()) {
                error = state.error();
            }
            return;
        }
        slot = *state;
    };
    get(out.air, "minecraft:air");
    get(out.sandstone, "minecraft:sandstone");
    get(out.smooth, "minecraft:smooth_sandstone");
    get(out.cut, "minecraft:cut_sandstone");
    get(out.chiseled, "minecraft:chiseled_sandstone");
    get(out.sand, "minecraft:sand");
    get(out.orange, "minecraft:orange_terracotta");
    get(out.blue, "minecraft:blue_terracotta");
    get(out.gold, "minecraft:gold_block");
    get(out.lantern, "minecraft:lantern", {{"hanging", "false"}, {"waterlogged", "false"}});
    get(out.lantern_hanging, "minecraft:lantern", {{"hanging", "true"}, {"waterlogged", "false"}});
    get(out.soul_lantern, "minecraft:soul_lantern", {{"hanging", "false"}, {"waterlogged", "false"}});
    get(out.soul_lantern_hanging, "minecraft:soul_lantern",
        {{"hanging", "true"}, {"waterlogged", "false"}});
    get(out.blackstone_bricks, "minecraft:polished_blackstone_bricks");
    get(out.quartz, "minecraft:smooth_quartz");
    get(out.chiseled_quartz, "minecraft:chiseled_quartz_block");
    get(out.spawner, "minecraft:spawner");
    get(out.tnt, "minecraft:tnt", {{"unstable", "false"}});
    get(out.plate, "minecraft:stone_pressure_plate", {{"powered", "false"}});
    get(out.suspicious_sand, "minecraft:suspicious_sand", {{"dusted", "0"}});
    get(out.tripwire_ns, "minecraft:tripwire",
        {{"attached", "true"}, {"disarmed", "false"}, {"powered", "false"}, {"north", "true"},
         {"south", "true"}, {"east", "false"}, {"west", "false"}});
    get(out.tripwire_ew, "minecraft:tripwire",
        {{"attached", "true"}, {"disarmed", "false"}, {"powered", "false"}, {"north", "false"},
         {"south", "false"}, {"east", "true"}, {"west", "true"}});
    get(out.trapdoor, "minecraft:birch_trapdoor",
        {{"facing", "north"}, {"half", "top"}, {"open", "false"}, {"powered", "false"},
         {"waterlogged", "false"}});
    for (usize f = 0; f < 4; ++f) {
        const std::string_view facing = kFacings[f];
        get(out.stairs[f], "minecraft:sandstone_stairs",
            {{"facing", facing}, {"half", "bottom"}, {"shape", "straight"}, {"waterlogged", "false"}});
        get(out.chest[f], "minecraft:chest",
            {{"facing", facing}, {"type", "single"}, {"waterlogged", "false"}});
        get(out.dispenser[f], "minecraft:dispenser", {{"facing", facing}, {"triggered", "false"}});
        get(out.hook[f], "minecraft:tripwire_hook",
            {{"facing", facing}, {"attached", "true"}, {"powered", "false"}});
        get(out.sticky_extended[f], "minecraft:sticky_piston",
            {{"facing", facing}, {"extended", "true"}});
        get(out.sticky_head[f], "minecraft:piston_head",
            {{"facing", facing}, {"type", "sticky"}, {"short", "false"}});
        get(out.lever_on[f], "minecraft:lever",
            {{"face", "wall"}, {"facing", facing}, {"powered", "true"}});
    }
    if (!error.empty()) {
        return std::unexpected(error);
    }
    return out;
}

/// The labyrinth: a perfect maze over the ring cells by the recursive
/// backtracker, iterative, from the cell the stair arrives in.
void carve_maze(GreatPyramidLayout& layout, FeatureRandom& random) {
    constexpr i32 kN = GreatPyramidLayout::kMazeCells;
    layout.maze.assign(static_cast<usize>(kN * kN), 0);
    for (i32 j = 0; j < kN; ++j) {
        for (i32 i = 0; i < kN; ++i) {
            const i32 u = -29 + 2 * i;
            const i32 v = -29 + 2 * j;
            if (std::max(std::abs(u), std::abs(v)) > kMazeCore) {
                layout.maze[GreatPyramidLayout::cell(i, j)] = 4;
            }
        }
    }
    std::vector<bool>                visited(layout.maze.size(), false);
    std::vector<std::array<i32, 2>>  stack;
    stack.reserve(layout.maze.size());
    stack.push_back(kMazeEntry);
    visited[GreatPyramidLayout::cell(kMazeEntry[0], kMazeEntry[1])] = true;
    constexpr std::array<std::array<i32, 2>, 4> kStep{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        std::array<u8, 4> choices{};
        i32               count = 0;
        for (u8 dir = 0; dir < 4; ++dir) {
            const i32 ni = i + kStep[dir][0];
            const i32 nj = j + kStep[dir][1];
            if (ni < 0 || nj < 0 || ni >= kN || nj >= kN || !layout.in_maze(ni, nj) ||
                visited[GreatPyramidLayout::cell(ni, nj)]) {
                continue;
            }
            choices[static_cast<usize>(count++)] = dir;
        }
        if (count == 0) {
            stack.pop_back();
            continue;
        }
        const u8  dir = choices[static_cast<usize>(random.next_int(count))];
        const i32 ni  = i + kStep[dir][0];
        const i32 nj  = j + kStep[dir][1];
        switch (dir) {
            case 0: layout.maze[GreatPyramidLayout::cell(ni, nj)] |= 2; break;
            case 1: layout.maze[GreatPyramidLayout::cell(i, j)] |= 1; break;
            case 2: layout.maze[GreatPyramidLayout::cell(i, j)] |= 2; break;
            default: layout.maze[GreatPyramidLayout::cell(ni, nj)] |= 1; break;
        }
        visited[GreatPyramidLayout::cell(ni, nj)] = true;
        stack.push_back({ni, nj});
    }
}

/// What each dead end holds, and the loot seeds, from the same random.
void furnish_dead_ends(GreatPyramidLayout& layout, FeatureRandom& random) {
    constexpr i32 kN = GreatPyramidLayout::kMazeCells;
    layout.dead_end_kind.assign(layout.maze.size(), DeadEnd::Empty);
    layout.dead_end_seed.assign(layout.maze.size(), 0);
    i32 tnt = 0;
    i32 chests = 0;
    i32 arrows = 0;
    for (i32 j = 0; j < kN; ++j) {
        for (i32 i = 0; i < kN; ++i) {
            if (!layout.in_maze(i, j) || (i == kMazeEntry[0] && j == kMazeEntry[1])) {
                continue;
            }
            i32 exits = 0;
            for (u8 dir = 0; dir < 4; ++dir) {
                exits += layout.open(i, j, dir) ? 1 : 0;
            }
            if (exits != 1) {
                continue;
            }
            const i32 roll = random.next_int(100);
            DeadEnd&  kind = layout.dead_end_kind[GreatPyramidLayout::cell(i, j)];
            if (roll < 20 && tnt < 6) {
                kind = DeadEnd::TntTrap;
                ++tnt;
            } else if (roll < 45 && chests < 8) {
                kind = DeadEnd::Chest;
                layout.dead_end_seed[GreatPyramidLayout::cell(i, j)] = random.next_long();
                ++chests;
            } else if (roll < 60 && arrows < 5) {
                kind = DeadEnd::ArrowTrap;
                ++arrows;
            }
        }
    }
}

[[nodiscard]] BoundingBox world_box(const GreatPyramidLayout& layout, const CBox& box) {
    const BlockPos a = layout.world(box.u0, box.y0, box.v0);
    const BlockPos b = layout.world(box.u1, box.y1, box.v1);
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z),
            std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

void put(nbt::Tag& compound, std::string name, nbt::Tag value) {
    compound.compound()->push_back(nbt::CompoundEntry{std::move(name), std::move(value)});
}

}  // namespace

RandomSpreadPlacement great_pyramid_placement() noexcept {
    RandomSpreadPlacement placement;
    placement.spacing    = 48;
    placement.separation = 24;
    placement.spread     = SpreadType::Linear;
    placement.salt       = 20260911;
    placement.frequency  = 1.0F;
    return placement;
}

std::string_view to_string(PyramidDecision decision) noexcept {
    switch (decision) {
        case PyramidDecision::NotCandidate: return "not_candidate";
        case PyramidDecision::NoSampler: return "no_sampler";
        case PyramidDecision::NotDesert: return "not_desert";
        case PyramidDecision::TooSteep: return "too_steep";
        case PyramidDecision::TooLow: return "too_low";
        case PyramidDecision::NearVanilla: return "near_vanilla";
        case PyramidDecision::Placed: return "placed";
    }
    return "unknown";
}

BlockPos GreatPyramidLayout::world(i32 u, i32 y, i32 v) const noexcept {
    // A clockwise quarter turn per step of `facing`, so the frame is never
    // mirrored and a stair's `facing` turns with it.
    i32 dx = u;
    i32 dz = v;
    switch (facing & 3U) {
        case 1: dx = -v; dz = u; break;
        case 2: dx = -u; dz = -v; break;
        case 3: dx = v; dz = -u; break;
        default: break;
    }
    return {centre_x + dx, base_y + y, centre_z + dz};
}

bool GreatPyramidLayout::in_maze(i32 i, i32 j) const noexcept {
    if (i < 0 || j < 0 || i >= kMazeCells || j >= kMazeCells || maze.empty()) {
        return false;
    }
    return (maze[cell(i, j)] & 4U) != 0;
}

bool GreatPyramidLayout::open(i32 i, i32 j, u8 dir) const noexcept {
    if (!in_maze(i, j)) {
        return false;
    }
    switch (dir) {
        case 0: return j > 0 && (maze[cell(i, j - 1)] & 2U) != 0;
        case 1: return i + 1 < kMazeCells && (maze[cell(i, j)] & 1U) != 0;
        case 2: return j + 1 < kMazeCells && (maze[cell(i, j)] & 2U) != 0;
        default: return i > 0 && (maze[cell(i - 1, j)] & 1U) != 0;
    }
}

GreatPyramid::GreatPyramid() : impl_(std::make_unique<Impl>()) {}
GreatPyramid::GreatPyramid(GreatPyramid&&) noexcept            = default;
GreatPyramid& GreatPyramid::operator=(GreatPyramid&&) noexcept = default;
GreatPyramid::~GreatPyramid()                                  = default;

std::expected<GreatPyramid, std::string> GreatPyramid::create(
    const registry::BlockRegistry& blocks) {
    auto palette = resolve_palette(blocks);
    if (!palette) {
        return std::unexpected("great pyramid: " + palette.error());
    }
    GreatPyramid out;
    out.impl_->blocks  = &blocks;
    out.impl_->palette = *palette;
    out.impl_->opaque.assign(blocks.block_count(), false);
    for (usize block = 0; block < blocks.block_count(); ++block) {
        const registry::BlockId id{static_cast<u16>(block)};
        // Ground: it stops movement, it is not leaves, and it is at least as
        // hard as sand (0.5) — which keeps cactus (0.4), glass and every
        // plant out, and water, which stops nothing.
        out.impl_->opaque[block] = !blocks.is_air(id) && blocks.blocks_motion(id) &&
                                   !blocks.is_leaves(id) && blocks.hardness(id) >= 0.5F;
    }
    return out;
}

PyramidDecision GreatPyramid::decide(i64 level_seed, i32 chunk_x, i32 chunk_z,
                                     const StructureWorldSampler* sampler,
                                     const StructurePlacer* placer,
                                     GreatPyramidLayout*    out) const {
    if (!great_pyramid_placement().is_candidate_chunk(level_seed, chunk_x, chunk_z)) {
        return PyramidDecision::NotCandidate;
    }
    if (sampler == nullptr) {
        return PyramidDecision::NoSampler;
    }
    const i32 x0 = chunk_x * 16 + 8;
    const i32 z0 = chunk_z * 16 + 8;

    // The biome at the chunk's middle, at its surface — the vanilla anchor —
    // and then most of the footprint.
    const i32 centre = terrain_height(*sampler, x0, z0);
    if (sampler->biome_at(x0, centre, z0) != kDesert) {
        return PyramidDecision::NotDesert;
    }
    i32 desert = 0;
    for (i32 a = -2; a <= 2; ++a) {
        for (i32 b = -2; b <= 2; ++b) {
            desert += sampler->biome_at(x0 + a * 25, centre, z0 + b * 25) == kDesert ? 1 : 0;
        }
    }
    if (desert < 22) {
        return PyramidDecision::NotDesert;
    }

    // Thirteen samples: the centre, the corners, the edge midpoints and the
    // four quarter points. The plinth sits at their median.
    constexpr std::array<std::array<i32, 2>, 13> kSamples{{{0, 0},
                                                           {-50, -50},
                                                           {50, -50},
                                                           {-50, 50},
                                                           {50, 50},
                                                           {0, -50},
                                                           {0, 50},
                                                           {-50, 0},
                                                           {50, 0},
                                                           {-25, -25},
                                                           {25, -25},
                                                           {-25, 25},
                                                           {25, 25}}};
    std::array<i32, 13> samples{};
    samples[0] = centre;
    for (usize k = 1; k < kSamples.size(); ++k) {
        samples[k] = terrain_height(*sampler, x0 + kSamples[k][0], z0 + kSamples[k][1]);
    }
    const auto [low, high] = std::minmax_element(samples.begin(), samples.end());
    if (*high - *low > kMaxSpread) {
        return PyramidDecision::TooSteep;
    }
    std::array<i32, 13> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const i32 base_y = sorted[6];
    if (base_y < kSeaLevel + 2) {
        return PyramidDecision::TooLow;
    }
    if (placer != nullptr && near_vanilla(*placer, *sampler, level_seed, chunk_x, chunk_z, base_y)) {
        return PyramidDecision::NearVanilla;
    }
    if (out == nullptr) {
        return PyramidDecision::Placed;
    }

    GreatPyramidLayout& layout = *out;
    layout                     = GreatPyramidLayout{};
    layout.chunk_x             = chunk_x;
    layout.chunk_z             = chunk_z;
    layout.centre_x            = x0;
    layout.centre_z            = z0;
    layout.base_y              = base_y;
    layout.samples             = samples;

    // Every choice from here on is the start's own random, in a fixed order.
    FeatureRandom random{FeatureRandom::Kind::Legacy,
                         large_feature_seed(level_seed, chunk_x, chunk_z)};
    layout.facing = static_cast<u8>(random.next_int(4));

    // The causeway runs down from the plinth to where the sand is, in front.
    const BlockPos foot = layout.world(0, 0, -kPlinthHalf - 4);
    const i32      top  = terrain_height(*sampler, foot.x, foot.z) - 1 - base_y;
    layout.causeway_steps = std::clamp(-2 - top, 0, kMaxCauseway);

    carve_maze(layout, random);
    furnish_dead_ends(layout, random);
    for (i64& seed : layout.treasure_seeds) {
        seed = random.next_long();
    }
    for (i64& seed : layout.queen_seeds) {
        seed = random.next_long();
    }
    for (i64& seed : layout.crypt_seeds) {
        seed = random.next_long();
    }
    std::vector<std::array<i32, 2>> pit;
    for (i32 v = kSandPit.v0; v <= kSandPit.v1; ++v) {
        for (i32 u = kSandPit.u0; u <= kSandPit.u1; ++u) {
            pit.push_back({u, v});
        }
    }
    for (i32 k = 0; k < kSuspiciousCount; ++k) {
        const i32 pick = k + random.next_int(static_cast<i32>(pit.size()) - k);
        std::swap(pit[static_cast<usize>(k)], pit[static_cast<usize>(pick)]);
        layout.suspicious.push_back(pit[static_cast<usize>(k)]);
    }
    for (i32 k = 0; k < kSuspiciousCount; ++k) {
        layout.suspicious_seeds.push_back(random.next_long());
    }

    const i32 front = kPlinthHalf + std::max(kFeather, layout.causeway_steps);
    layout.box      = world_box(layout, {-kPlinthHalf - kFeather, base_y - 4 - kFillDepth,
                                         -front, kPlinthHalf + kFeather, base_y + kTop,
                                         kPlinthHalf + kFeather});
    // `world_box` adds base_y to both y bounds: undo it for the absolute ones.
    layout.box.min_y = base_y - 4 - kFillDepth;
    layout.box.max_y = base_y + kTop;
    return PyramidDecision::Placed;
}

nbt::Tag GreatPyramid::start_to_nbt(const GreatPyramidLayout& layout) {
    // `Direction.get2DDataValue`: south 0, west 1, north 2, east 3.
    constexpr std::array<i32, 4> kOrientation{2, 3, 0, 1};
    struct Room {
        std::string_view name;
        CBox             box;
    };
    const std::array<Room, 6> rooms{{
        {"hall", {-16, -1, -31, 16, 12, 1}},
        {"gallery", {-3, 0, kGalleryFirst, 3, 33, 3}},
        {"queen", {2, 12, -15, 17, 18, -5}},
        {"pharaoh", {-8, 25, 3, 8, 35, 13}},
        {"labyrinth", {-kMazeOuter, kMazeFloor, -kMazeOuter, kMazeOuter, 17, kMazeOuter}},
        {"crypt", {-12, -14, -12, 15, -1, 12}},
    }};
    const auto child = [&](std::string_view name, const BoundingBox& box) {
        nbt::Tag piece = nbt::Tag::make_compound();
        put(piece, "id", nbt::Tag{"ondes_voxel:great_pyramid/" + std::string{name}});
        put(piece, "BB",
            nbt::Tag{nbt::Tag::IntArray{box.min_x, box.min_y, box.min_z, box.max_x, box.max_y,
                                        box.max_z}});
        put(piece, "GD", nbt::Tag{i32{0}});
        put(piece, "O", nbt::Tag{kOrientation[layout.facing & 3U]});
        return piece;
    };
    nbt::Tag children = nbt::Tag::make_list(nbt::TagType::Compound);
    children.list()->push_back(child("shell", layout.box));
    for (const Room& room : rooms) {
        children.list()->push_back(child(room.name, world_box(layout, room.box)));
    }
    nbt::Tag out = nbt::Tag::make_compound();
    put(out, "id", nbt::Tag{std::string{kGreatPyramidId}});
    put(out, "ChunkX", nbt::Tag{layout.chunk_x});
    put(out, "ChunkZ", nbt::Tag{layout.chunk_z});
    put(out, "references", nbt::Tag{i32{0}});
    put(out, "Children", std::move(children));
    return out;
}

}  // namespace ov::worldgen
