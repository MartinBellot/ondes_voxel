// The Great Pyramid's blocks, written one chunk column at a time.
// An original Ondes VOXEL structure — see great_pyramid.hpp.
//
// Three rules keep a chunk's part independent of every other chunk:
//   * every write is clipped to the column being placed;
//   * every read (the fill under the plinth, the feathered sand, the causeway's
//     footing) is of a position inside that same column;
//   * the operations run in one fixed order — terrain and shell, then the
//     linings of every room, then every room's air, then the furniture — so a
//     block two operations touch always ends up with the later one's.
#include "great_pyramid_impl.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace ov::worldgen {

using namespace pyramid;

namespace {

/// Canonical direction vectors, (du, dv): 0 -v, 1 +u, 2 +v, 3 -u.
constexpr std::array<std::array<i32, 2>, 4> kDir{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
constexpr u8 kNorth = 0;
constexpr u8 kEast  = 1;
constexpr u8 kSouth = 2;
constexpr u8 kWest  = 3;

void put(nbt::Tag& compound, std::string name, nbt::Tag value) {
    compound.compound()->push_back(nbt::CompoundEntry{std::move(name), std::move(value)});
}

[[nodiscard]] nbt::Tag loot_container(std::string_view id, std::string_view table, i64 seed) {
    nbt::Tag tag = nbt::Tag::make_compound();
    put(tag, "id", nbt::Tag{std::string{id}});
    put(tag, "LootTable", nbt::Tag{std::string{table}});
    put(tag, "LootTableSeed", nbt::Tag{seed});
    return tag;
}

[[nodiscard]] nbt::Tag arrow_dispenser() {
    nbt::Tag item = nbt::Tag::make_compound();
    put(item, "Slot", nbt::Tag{i8{0}});
    put(item, "id", nbt::Tag{std::string{"minecraft:arrow"}});
    put(item, "Count", nbt::Tag{i8{16}});
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    items.list()->push_back(std::move(item));
    nbt::Tag tag = nbt::Tag::make_compound();
    put(tag, "id", nbt::Tag{std::string{"minecraft:dispenser"}});
    put(tag, "Items", std::move(items));
    return tag;
}

[[nodiscard]] nbt::Tag husk_spawner() {
    nbt::Tag entity = nbt::Tag::make_compound();
    put(entity, "id", nbt::Tag{std::string{"minecraft:husk"}});
    nbt::Tag spawn_data = nbt::Tag::make_compound();
    put(spawn_data, "entity", std::move(entity));
    nbt::Tag tag = nbt::Tag::make_compound();
    put(tag, "id", nbt::Tag{std::string{"minecraft:mob_spawner"}});
    put(tag, "SpawnData", std::move(spawn_data));
    put(tag, "SpawnPotentials", nbt::Tag::make_list(nbt::TagType::Compound));
    put(tag, "Delay", nbt::Tag{i16{20}});
    put(tag, "MinSpawnDelay", nbt::Tag{i16{200}});
    put(tag, "MaxSpawnDelay", nbt::Tag{i16{800}});
    put(tag, "SpawnCount", nbt::Tag{i16{4}});
    put(tag, "MaxNearbyEntities", nbt::Tag{i16{6}});
    put(tag, "RequiredPlayerRange", nbt::Tag{i16{16}});
    put(tag, "SpawnRange", nbt::Tag{i16{4}});
    return tag;
}

constexpr std::string_view kTreasure    = "ondes_voxel:chests/great_pyramid/treasure";
constexpr std::string_view kQueenTable  = "ondes_voxel:chests/great_pyramid/queen";
constexpr std::string_view kCorridor    = "ondes_voxel:chests/great_pyramid/corridor";
constexpr std::string_view kCryptTable  = "ondes_voxel:chests/great_pyramid/crypt";
constexpr std::string_view kArchaeology = "ondes_voxel:archaeology/great_pyramid";

[[nodiscard]] constexpr bool inside(i32 u, i32 y, i32 v) noexcept {
    return y >= 0 && y <= kTop && std::max(std::abs(u), std::abs(v)) <= kHalf - y;
}

class Builder {
public:
    Builder(StructureLevel& level, const GreatPyramidLayout& layout, const GreatPyramid::Impl& impl,
            const BoundingBox& clip)
        : level_(&level), layout_(&layout), impl_(&impl), p_(&impl.palette), clip_(clip) {
        // The clip's corners in the canonical frame.
        const std::array<std::array<i32, 2>, 4> corners{{{clip.min_x, clip.min_z},
                                                         {clip.max_x, clip.min_z},
                                                         {clip.min_x, clip.max_z},
                                                         {clip.max_x, clip.max_z}}};
        cu0_ = cv0_ = std::numeric_limits<i32>::max();
        cu1_ = cv1_ = std::numeric_limits<i32>::min();
        for (const auto& [x, z] : corners) {
            const i32 dx = x - layout.centre_x;
            const i32 dz = z - layout.centre_z;
            i32       u  = dx;
            i32       v  = dz;
            switch (layout.facing & 3U) {
                case 1: u = dz; v = -dx; break;
                case 2: u = -dx; v = -dz; break;
                case 3: u = -dz; v = dx; break;
                default: break;
            }
            cu0_ = std::min(cu0_, u);
            cu1_ = std::max(cu1_, u);
            cv0_ = std::min(cv0_, v);
            cv1_ = std::max(cv1_, v);
        }
        cy0_ = clip.min_y - layout.base_y;
        cy1_ = clip.max_y - layout.base_y;
    }

    void run() {
        terrain_and_shell();
        causeway();
        linings();
        airs();
        furniture();
    }

    [[nodiscard]] u64 written() const noexcept { return written_; }

private:
    // ── Primitives ──────────────────────────────────────────────────────────

    [[nodiscard]] bool column_here(i32 u, i32 v) const noexcept {
        const BlockPos p = layout_->world(u, 0, v);
        return p.x >= clip_.min_x && p.x <= clip_.max_x && p.z >= clip_.min_z && p.z <= clip_.max_z;
    }

    void set(i32 u, i32 y, i32 v, registry::BlockStateId state) {
        const BlockPos p = layout_->world(u, y, v);
        if (!clip_.contains(p.x, p.y, p.z)) {
            return;
        }
        if (level_->set_block(p.x, p.y, p.z, state)) {
            ++written_;
        }
    }

    [[nodiscard]] registry::BlockStateId get(i32 u, i32 y, i32 v) const {
        const BlockPos p = layout_->world(u, y, v);
        return level_->block_at(p.x, p.y, p.z);
    }

    [[nodiscard]] bool opaque(registry::BlockStateId state) const {
        const usize block = impl_->blocks->block_of(state).value();
        return block < impl_->opaque.size() && impl_->opaque[block];
    }

    [[nodiscard]] bool is_air(registry::BlockStateId state) const {
        return impl_->blocks->is_air(impl_->blocks->block_of(state));
    }

    void entity(i32 u, i32 y, i32 v, nbt::Tag data) {
        const BlockPos p = layout_->world(u, y, v);
        if (clip_.contains(p.x, p.y, p.z)) {
            level_->set_block_entity(p.x, p.y, p.z, std::move(data));
        }
    }

    /// Fill a canonical box, clipped. `only_inside` skips what lies above
    /// ground outside the pyramid's steps, so a lining never juts out of a
    /// face.
    void fill(const CBox& box, registry::BlockStateId state, bool only_inside = false) {
        const i32 u0 = std::max(box.u0, cu0_);
        const i32 u1 = std::min(box.u1, cu1_);
        const i32 v0 = std::max(box.v0, cv0_);
        const i32 v1 = std::min(box.v1, cv1_);
        const i32 y0 = std::max(box.y0, cy0_);
        const i32 y1 = std::min(box.y1, cy1_);
        for (i32 v = v0; v <= v1; ++v) {
            for (i32 u = u0; u <= u1; ++u) {
                for (i32 y = y0; y <= y1; ++y) {
                    if (only_inside && y >= 0 && !inside(u, y, v)) {
                        continue;
                    }
                    set(u, y, v, state);
                }
            }
        }
    }

    void one(i32 u, i32 y, i32 v, registry::BlockStateId state) { fill({u, y, v, u, y, v}, state); }

    // ── Terrain and shell ───────────────────────────────────────────────────

    [[nodiscard]] registry::BlockStateId shell(i32 u, i32 v, i32 y) const {
        if (y >= kTop - 2) {
            return p_->gold;  // the capstone: 5 x 5, 3 x 3, 1 x 1
        }
        const i32 h  = kHalf - y;
        const i32 au = std::abs(u);
        const i32 av = std::abs(v);
        if (std::max(au, av) < h) {
            return p_->sandstone;
        }
        if (au == h && av == h) {
            return p_->chiseled;
        }
        if (y > 0 && y % 8 == 0) {
            return p_->cut;
        }
        // The motifs, mid-height between two bands: three diamonds a face.
        const i32 t = av == h ? u : v;
        for (const i32 centre : {0, -11, 11}) {
            const i32 distance = std::abs(t - centre) + std::abs(y - 28);
            if (distance <= 1) {
                return p_->blue;
            }
            if (distance <= 3) {
                return p_->orange;
            }
        }
        return p_->smooth;
    }

    void terrain_and_shell() {
        const i32 reach = kPlinthHalf + kFeather;
        for (i32 v = std::max(-reach, cv0_); v <= std::min(reach, cv1_); ++v) {
            for (i32 u = std::max(-reach, cu0_); u <= std::min(reach, cu1_); ++u) {
                if (!column_here(u, v)) {
                    continue;
                }
                const i32 d = std::max(std::abs(u), std::abs(v));
                if (d <= kPlinthHalf) {
                    plinth_column(u, v, d);
                } else {
                    feather_column(u, v, d - kPlinthHalf);
                }
            }
        }
    }

    void plinth_column(i32 u, i32 v, i32 d) {
        // Nothing floats: the column is filled down to the first opaque block.
        for (i32 y = -4; y > -4 - kFillDepth; --y) {
            if (opaque(get(u, y, v))) {
                break;
            }
            set(u, y, v, p_->sandstone);
        }
        set(u, -3, v, p_->sandstone);
        set(u, -2, v, p_->sandstone);
        set(u, -1, v, d > kHalf ? p_->smooth : p_->cut);
        for (i32 y = 0; y <= kTop; ++y) {
            if (d <= kHalf - y) {
                set(u, y, v, shell(u, v, y));
            } else if (y <= kClearAbove) {
                if (!is_air(get(u, y, v))) {
                    set(u, y, v, p_->air);
                }
            } else {
                break;
            }
        }
    }

    void feather_column(i32 u, i32 v, i32 ring) {
        const i32 target = -1 - ring;
        if (opaque(get(u, target, v))) {
            return;
        }
        i32 ground = target - 1;
        while (ground > -4 - kFillDepth && !opaque(get(u, ground, v))) {
            --ground;
        }
        for (i32 y = ground + 1; y <= target; ++y) {
            set(u, y, v, p_->sand);
        }
    }

    void causeway() {
        const registry::BlockStateId step = p_->stairs[layout_->world_facing(kSouth)];
        for (i32 s = 1; s <= layout_->causeway_steps; ++s) {
            const i32 v = -kPlinthHalf - s;
            const i32 y = -1 - s;
            for (i32 u = -3; u <= 3; ++u) {
                if (!column_here(u, v)) {
                    continue;
                }
                set(u, y, v, step);
                for (i32 below = y - 1; below > y - kFillDepth; --below) {
                    if (opaque(get(u, below, v))) {
                        break;
                    }
                    set(u, below, v, p_->sandstone);
                }
                for (i32 above = y + 1; above <= y + 4; ++above) {
                    if (!is_air(get(u, above, v))) {
                        set(u, above, v, p_->air);
                    }
                }
            }
        }
    }

    // ── Linings: the walls of every room, before any room is hollowed ───────

    void linings() {
        fill({-4, -1, -50, 4, 9, -31}, p_->cut, true);    // entrance tunnel
        fill({-16, -1, -31, 16, 11, 1}, p_->cut, true);   // hall walls and floor
        fill({-16, 12, -31, 16, 12, 1}, p_->smooth, true);  // hall ceiling
        fill({2, 12, -12, 7, 16, -8}, p_->cut, true);     // Queen's passage
        fill({7, 12, -15, 17, 18, -5}, p_->cut, true);    // Queen's chamber
        fill({-8, 25, 3, 8, 35, 13}, p_->cut, true);      // Pharaoh's chamber
        fill({-8, 30, 3, 8, 30, 13}, p_->blue, true);     // ... its frieze
        fill({-7, 25, 4, 7, 25, 12}, p_->smooth, true);   // ... its floor
        maze_slab();
        fill({-18, -1, -30, -16, 12, -11}, p_->cut, true);  // labyrinth stair
        fill({-18, 12, -16, -16, 17, -11}, p_->cut, true);  // ... its landing
        fill(kCryptShell, p_->cut);
        fill({13, -14, -4, 15, -1, 7}, p_->cut);   // crypt stair
        fill({11, -13, 4, 15, -9, 6}, p_->cut);    // ... and its passage
        gallery_lining();
    }

    void maze_slab() {
        for (i32 v = std::max(-kMazeOuter, cv0_); v <= std::min(kMazeOuter, cv1_); ++v) {
            for (i32 u = std::max(-kMazeOuter, cu0_); u <= std::min(kMazeOuter, cu1_); ++u) {
                if (std::max(std::abs(u), std::abs(v)) < kMazeCore) {
                    continue;
                }
                set(u, kMazeFloor, v, p_->cut);
                for (i32 y = kMazeFloor + 1; y <= kMazeFloor + 3; ++y) {
                    set(u, y, v, p_->smooth);
                }
                set(u, kMazeFloor + 4, v, p_->sandstone);
            }
        }
    }

    void gallery_lining() {
        // The corbelled vault: each pair of courses steps in by one. Above the
        // hall only — where the gallery runs through the hall there is no wall.
        for (i32 v = -10; v < kGalleryLast; ++v) {
            const i32 ys = gallery_step(v);
            fill({-3, ys + 1, v, -3, ys + 3, v}, p_->cut, true);
            fill({3, ys + 1, v, 3, ys + 3, v}, p_->cut, true);
            fill({-2, ys + 4, v, -2, ys + 6, v}, p_->smooth, true);
            fill({2, ys + 4, v, 2, ys + 6, v}, p_->smooth, true);
            one(-1, ys + 7, v, p_->smooth);
            one(1, ys + 7, v, p_->smooth);
            one(0, ys + 8, v, p_->smooth);
        }
    }

    // ── Airs ────────────────────────────────────────────────────────────────

    void airs() {
        fill(kTunnel, p_->air);
        fill(kHall, p_->air);
        gallery_air();
        fill(kQueenPassage, p_->air);
        fill(kQueen, p_->air);
        fill({17, 13, -11, 17, 15, -9}, p_->air);  // the Queen's niche
        fill(kPharaoh, p_->air);
        air_shafts();
        maze_air();
        maze_stair_air();
        fill(kCryptInside, p_->air);
        crypt_stair_air();
    }

    void gallery_air() {
        for (i32 v = kGalleryFirst; v <= kGalleryLast; ++v) {
            const i32 ys = gallery_step(v);
            if (v == kGalleryLast) {
                // The last step narrows to the door: the pistons beside it
                // stay out of sight.
                fill({0, ys + 1, v, 0, ys + 3, v}, p_->air);
                continue;
            }
            fill({-2, ys + 1, v, 2, ys + 3, v}, p_->air);
            fill({-1, ys + 4, v, 1, ys + 6, v}, p_->air);
            one(0, ys + 7, v, p_->air);
        }
    }

    void air_shafts() {
        // Two 1 x 1 shafts, stepped at 45 degrees, from the chamber's side
        // walls out through the shell.
        for (const i32 side : {-1, 1}) {
            for (i32 k = 0; k <= 12; ++k) {
                const i32 y = 30 + k;
                for (const i32 u : {side * (8 + k), side * (9 + k)}) {
                    if (inside(u, y, 8)) {
                        one(u, y, 8, p_->air);
                    }
                }
            }
        }
    }

    void maze_air() {
        const i32 lo = kMazeFloor + 1;
        const i32 hi = kMazeFloor + 3;
        for (i32 v = std::max(-29, cv0_); v <= std::min(29, cv1_); ++v) {
            for (i32 u = std::max(-29, cu0_); u <= std::min(29, cu1_); ++u) {
                const bool odd_u = (u & 1) != 0;
                const bool odd_v = (v & 1) != 0;
                bool       carve = false;
                if (odd_u && odd_v) {
                    carve = layout_->in_maze((u + 29) / 2, (v + 29) / 2);
                } else if (!odd_u && odd_v) {
                    carve = layout_->open((u + 28) / 2, (v + 29) / 2, kEast);
                } else if (odd_u && !odd_v) {
                    carve = layout_->open((u + 29) / 2, (v + 28) / 2, kSouth);
                }
                if (carve) {
                    fill({u, lo, v, u, hi, v}, p_->air);
                }
            }
        }
    }

    void maze_stair_air() {
        fill({-16, 0, -29, -16, 2, -29}, p_->air);         // the door in the hall's wall
        fill({kMazeStairU, 0, -29, kMazeStairU, 3, -29}, p_->air);
        for (i32 v = -28; v <= -15; ++v) {
            const i32 ys = maze_stair_step(v);
            fill({kMazeStairU, ys + 1, v, kMazeStairU, ys + 3, v}, p_->air);
        }
        fill({kMazeStairU, 14, -14, kMazeStairU, 16, -12}, p_->air);  // landing
        fill({-18, 14, -13, -18, 16, -13}, p_->air);                  // into cell (-19, -13)
    }

    void crypt_stair_air() {
        fill({kCryptStairU, -4, kHatchV, kCryptStairU, -2, kHatchV}, p_->air);
        for (i32 i = 1; i <= kCryptSteps; ++i) {
            const i32 ys = -5 - i;
            const i32 v  = kHatchV + i;
            fill({kCryptStairU, ys + 1, v, kCryptStairU, ys + 3, v}, p_->air);
        }
        fill({kCryptStairU, -12, 5, kCryptStairU, -10, 6}, p_->air);  // landing
        fill({11, -12, 5, 13, -10, 5}, p_->air);                      // into the crypt
    }

    // ── Furniture ───────────────────────────────────────────────────────────

    void furniture() {
        portal();
        hall();
        gallery();
        secret_door();
        queen();
        pharaoh();
        labyrinth();
        crypt();
    }

    void portal() {
        // Two chiseled pillars in front of the face, a lintel, two lanterns.
        fill({-6, 0, -52, -5, 10, -51}, p_->chiseled);
        fill({5, 0, -52, 6, 10, -51}, p_->chiseled);
        fill({-4, 10, -52, 4, 10, -51}, p_->cut);
        one(0, 10, -51, p_->chiseled);
        one(-5, 11, -51, p_->lantern);
        one(5, 11, -51, p_->lantern);
    }

    void hall() {
        // The floor: cut sandstone, a terracotta lattice down the nave, a blue
        // border.
        for (i32 v = kHall.v0; v <= kHall.v1; ++v) {
            for (i32 u = kHall.u0; u <= kHall.u1; ++u) {
                registry::BlockStateId s = p_->cut;
                const i32              au = std::abs(u);
                if (au <= 4) {
                    if (au == 4) {
                        s = p_->blue;
                    } else if (((u + v) & 3) == 0 || ((u - v) & 3) == 0) {
                        s = p_->orange;
                    }
                } else if (au == kHall.u1 || v == kHall.v0 || v == kHall.v1) {
                    s = p_->blue;
                }
                one(u, -1, v, s);
            }
        }
        // Twenty 3 x 3 pillars, every six blocks, either side of the nave.
        for (const i32 pu : {-12, -6, 6, 12}) {
            for (const i32 pv : {-27, -21, -15, -9, -3}) {
                fill({pu - 1, 0, pv - 1, pu + 1, 11, pv + 1}, p_->cut);
                fill({pu - 1, 0, pv - 1, pu + 1, 0, pv + 1}, p_->chiseled);
                fill({pu - 1, 6, pv - 1, pu + 1, 6, pv + 1}, p_->orange);
                fill({pu - 1, 11, pv - 1, pu + 1, 11, pv + 1}, p_->chiseled);
            }
        }
        for (const i32 lu : {-9, 9}) {
            for (const i32 lv : {-24, -18, -12, -6}) {
                one(lu, 11, lv, p_->lantern_hanging);
            }
        }
        one(0, 11, -28, p_->lantern_hanging);
        one(0, 11, -3, p_->lantern_hanging);
        // The pit: a plate in the nave, nine TNT under the floor. The plate
        // powers the tile it stands on; the tile, the TNT under it.
        one(0, 0, kHallPitV, p_->plate);
        fill({-1, -2, kHallPitV - 1, 1, -2, kHallPitV + 1}, p_->tnt);
    }

    void gallery() {
        const registry::BlockStateId up = p_->stairs[layout_->world_facing(kSouth)];
        for (i32 v = kGalleryFirst; v <= kGalleryLast; ++v) {
            const i32 ys = gallery_step(v);
            if (ys <= 12) {
                // Where it climbs through the hall: a solid ramp with ledges.
                if (ys > 0) {
                    fill({-1, 0, v, 1, ys - 1, v}, p_->sandstone);
                }
                fill({-2, 0, v, -2, ys, v}, p_->cut);
                fill({2, 0, v, 2, ys, v}, p_->cut);
            }
            fill({-1, ys, v, 1, ys, v}, up);
        }
        for (const i32 lv : {-19, -13, -7, -1}) {
            const i32 ys = gallery_step(lv);
            one(-2, ys + 1, lv, p_->lantern);
            one(2, ys + 1, lv, p_->lantern);
        }
    }

    void secret_door() {
        // Two sticky pistons in the chamber's front wall, extended, holding
        // the door shut. The lever is on, tucked into the corbel of the last
        // flight: it powers the block behind it, which powers the upper
        // piston, and the lower one through the block above it. Switching it
        // off retracts both and opens the door.
        const u8 east = layout_->world_facing(kEast);
        one(0, 25, 3, p_->stairs[layout_->world_facing(kSouth)]);
        one(0, 26, 3, p_->cut);
        one(0, 27, 3, p_->cut);
        one(-2, 26, 3, p_->sticky_extended[east]);
        one(-2, 27, 3, p_->sticky_extended[east]);
        one(-1, 26, 3, p_->sticky_head[east]);
        one(-1, 27, 3, p_->sticky_head[east]);
        one(-2, 27, 1, p_->lever_on[layout_->world_facing(kNorth)]);
    }

    void queen() {
        const u8 west = layout_->world_facing(kWest);
        one(16, 13, -12, p_->chest[west]);
        entity(16, 13, -12, loot_container("minecraft:chest", kQueenTable, layout_->queen_seeds[0]));
        one(16, 13, -8, p_->chest[west]);
        entity(16, 13, -8, loot_container("minecraft:chest", kQueenTable, layout_->queen_seeds[1]));
        one(8, 13, -14, p_->lantern);
        one(8, 13, -6, p_->lantern);
        one(17, 13, -10, p_->gold);
        one(17, 14, -10, p_->lantern);
        // The passage's trap: a tripwire across it, a hook on each wall, and
        // above each hook's wall block a dispenser of arrows aimed across.
        one(5, 13, -11, p_->hook[layout_->world_facing(kSouth)]);
        one(5, 13, -9, p_->hook[layout_->world_facing(kNorth)]);
        one(5, 13, -10, (layout_->facing & 1U) != 0 ? p_->tripwire_ew : p_->tripwire_ns);
        one(5, 14, -12, p_->dispenser[layout_->world_facing(kSouth)]);
        entity(5, 14, -12, arrow_dispenser());
        one(5, 14, -8, p_->dispenser[layout_->world_facing(kNorth)]);
        entity(5, 14, -8, arrow_dispenser());
    }

    void pharaoh() {
        // The sarcophagus: polished blackstone, gold at its corners, a quartz
        // lid with a chiseled head.
        fill({-1, 26, 6, 1, 26, 10}, p_->blackstone_bricks);
        for (const i32 u : {-1, 1}) {
            for (const i32 v : {6, 10}) {
                one(u, 26, v, p_->gold);
            }
        }
        fill({-1, 27, 6, 1, 27, 10}, p_->quartz);
        one(0, 27, 6, p_->chiseled_quartz);
        const u8 east = layout_->world_facing(kEast);
        const u8 west = layout_->world_facing(kWest);
        const std::array<std::array<i32, 2>, 4> chests{{{-6, 6}, {-6, 10}, {6, 6}, {6, 10}}};
        for (usize k = 0; k < chests.size(); ++k) {
            const auto [u, v] = chests[k];
            one(u, 26, v, p_->chest[u < 0 ? east : west]);
            entity(u, 26, v,
                   loot_container("minecraft:chest", kTreasure, layout_->treasure_seeds[k]));
        }
        for (const i32 u : {-7, 7}) {
            for (const i32 v : {4, 12}) {
                one(u, 26, v, p_->gold);
                one(u, 27, v, p_->lantern);
            }
        }
        one(-4, 34, 8, p_->lantern_hanging);
        one(4, 34, 8, p_->lantern_hanging);
        one(0, 34, 5, p_->lantern_hanging);
        one(0, 34, 11, p_->lantern_hanging);
    }

    void labyrinth() {
        const registry::BlockStateId up = p_->stairs[layout_->world_facing(kSouth)];
        for (i32 v = -28; v <= -15; ++v) {
            one(kMazeStairU, maze_stair_step(v), v, up);
        }
        constexpr i32 kN = GreatPyramidLayout::kMazeCells;
        for (i32 j = 0; j < kN; ++j) {
            for (i32 i = 0; i < kN; ++i) {
                const DeadEnd kind = layout_->dead_end_kind.empty()
                                         ? DeadEnd::Empty
                                         : layout_->dead_end_kind[GreatPyramidLayout::cell(i, j)];
                if (kind == DeadEnd::Empty) {
                    continue;
                }
                u8 exit = 0;
                for (u8 dir = 0; dir < 4; ++dir) {
                    if (layout_->open(i, j, dir)) {
                        exit = dir;
                    }
                }
                const i32 u = -29 + 2 * i;
                const i32 v = -29 + 2 * j;
                const i32 y = kMazeFloor + 1;
                switch (kind) {
                    case DeadEnd::TntTrap:
                        one(u, y, v, p_->plate);
                        one(u, y - 2, v, p_->tnt);
                        for (const auto& [du, dv] : kDir) {
                            one(u + du, y - 2, v + dv, p_->tnt);
                        }
                        break;
                    case DeadEnd::ArrowTrap: {
                        // The plate powers the dispenser beside it, in the
                        // wall at the end, aimed back down the corridor.
                        const u8  back = static_cast<u8>((exit + 2) & 3U);
                        const i32 wu   = u + kDir[back][0];
                        const i32 wv   = v + kDir[back][1];
                        one(u, y, v, p_->plate);
                        one(wu, y, wv, p_->dispenser[layout_->world_facing(exit)]);
                        entity(wu, y, wv, arrow_dispenser());
                        break;
                    }
                    case DeadEnd::Chest:
                        one(u, y, v, p_->chest[layout_->world_facing(exit)]);
                        entity(u, y, v,
                               loot_container("minecraft:chest", kCorridor,
                                              layout_->dead_end_seed[GreatPyramidLayout::cell(i, j)]));
                        break;
                    case DeadEnd::Empty: break;
                }
            }
        }
    }

    void crypt() {
        for (const i32 pu : {-5, 5}) {
            for (const i32 pv : {-5, 5}) {
                fill({pu - 1, -12, pv - 1, pu + 1, -6, pv + 1}, p_->cut);
                fill({pu - 1, -12, pv - 1, pu + 1, -12, pv + 1}, p_->chiseled);
                fill({pu - 1, -6, pv - 1, pu + 1, -6, pv + 1}, p_->chiseled);
            }
        }
        fill({-1, -13, -1, 1, -13, 1}, p_->blue);
        one(0, -13, 0, p_->cut);
        one(0, -12, 0, p_->spawner);
        entity(0, -12, 0, husk_spawner());
        one(-9, -12, 0, p_->chest[layout_->world_facing(kEast)]);
        entity(-9, -12, 0, loot_container("minecraft:chest", kCryptTable, layout_->crypt_seeds[0]));
        one(9, -12, 0, p_->chest[layout_->world_facing(kWest)]);
        entity(9, -12, 0, loot_container("minecraft:chest", kCryptTable, layout_->crypt_seeds[1]));
        fill(kSandPit, p_->sand);
        for (usize k = 0; k < layout_->suspicious.size(); ++k) {
            const auto [u, v] = layout_->suspicious[k];
            one(u, kSandPit.y0, v, p_->suspicious_sand);
            entity(u, kSandPit.y0, v,
                   loot_container("minecraft:brushable_block", kArchaeology,
                                  layout_->suspicious_seeds[k]));
        }
        one(0, -6, 7, p_->soul_lantern_hanging);
        one(0, -6, -7, p_->soul_lantern_hanging);
        for (const i32 u : {-9, 9}) {
            for (const i32 v : {-9, 9}) {
                one(u, -12, v, p_->soul_lantern);
            }
        }
        // The hidden stair: under a birch trapdoor in the hall's floor, behind
        // the last pillar on the right.
        const registry::BlockStateId up = p_->stairs[layout_->world_facing(kNorth)];
        for (i32 i = 1; i <= kCryptSteps; ++i) {
            one(kCryptStairU, -5 - i, kHatchV + i, up);
        }
        one(kCryptStairU, -1, kHatchV, p_->trapdoor);
    }

    StructureLevel*           level_;
    const GreatPyramidLayout* layout_;
    const GreatPyramid::Impl* impl_;
    const Palette*            p_;
    BoundingBox               clip_;
    i32                       cu0_{0}, cu1_{0}, cv0_{0}, cv1_{0}, cy0_{0}, cy1_{0};
    u64                       written_{0};
};

}  // namespace

u64 GreatPyramid::place(StructureLevel& level, const GreatPyramidLayout& layout,
                        const BoundingBox& clip) const {
    if (!layout.box.intersects(clip)) {
        return 0;
    }
    Builder builder{level, layout, *impl_, clip};
    builder.run();
    return builder.written();
}

}  // namespace ov::worldgen
