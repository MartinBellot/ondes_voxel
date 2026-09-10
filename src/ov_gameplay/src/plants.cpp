#include "ov/gameplay/plants.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <utility>

namespace ov::gameplay {

namespace {

enum class Kind : u8 {
    None,
    /// wheat, carrots, potatoes: eight stages, the farmland formula.
    Crop,
    /// beetroots: four stages, and a random tick that only reaches the crop
    /// rule part of the time.
    Beetroot,
    /// torchflower_crop: two stages, then it *is* a torchflower.
    Torchflower,
    NetherWart,
    Stem,
    AttachedStem,
    Farmland,
    SugarCane,
    Cactus,
    Kelp,
    SweetBerry,
    Cocoa,
    /// Every `#saplings` member with a `stage` property.
    Sapling,
    /// azalea and flowering_azalea: bone meal grows a tree, nothing ticks.
    Azalea,
    Leaves,
    /// grass_block and mycelium: spread onto dirt, die under a lid.
    Spreading,
    /// podzol: only the `snowy` flag follows the block above.
    Snowy,
    Ice,
    SnowLayer,
    /// grass and fern: bone meal makes them double.
    ShortGrass,
    /// pitcher_crop: recognised, not grown. See `kUnanswered`.
    Pitcher,
};

constexpr registry::BlockId kNoBlock{0xFFFF};

/// The four horizontal directions, in the order the game lists them.
struct Horizontal {
    i32              dx;
    i32              dz;
    std::string_view name;
};
constexpr std::array<Horizontal, 4> kHorizontal{{
    {0, -1, "north"},
    {1, 0, "east"},
    {0, 1, "south"},
    {-1, 0, "west"},
}};

constexpr std::array<std::array<i32, 3>, 6> kSix{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
}};

/// A crop, a stem and a sapling need this much light to grow. From the wiki
/// (Wheat Seeds, Sapling); the growth campaign ran in full daylight and so
/// does not test it.
constexpr u8 kGrowthLight = 9;

/// A beetroot's random tick reaches the crop rule only when a draw of
/// `next_int(3)` is not zero — two times in three. **Measured**: see
/// docs/provenance/agriculture.md § croissance, where the beetroot
/// distribution is compared against the wheat one on identical tiles.
constexpr i32 kBeetrootOneIn = 3;

/// Ice melts when the block light stored **in the ice cell** exceeds this.
/// **Measured** against `light[level=N]` sources, reading the light the game
/// wrote into the cell: ice at 10 held for 603 ticks at speed 200, ice at 11
/// melted, on stone and over air alike — one lower than the "above 11" the
/// wiki gives, because ice takes a level off the light it lets in and the
/// threshold is counted after. See § fonte.
constexpr u8 kIceMeltsAbove = 10;

/// The same for a snow layer: 11 held, 12 melted. **Measured** alongside.
constexpr u8 kSnowMeltsAbove = 11;

/// Bone meal on a sapling advances it with this chance (wiki, Bone Meal;
/// measured in § poudre d'os).
constexpr f32 kSaplingBoneMeal = 0.45F;

/// A kelp head grows with this chance per random tick (wiki, Kelp). Not
/// measured.
constexpr f64 kKelpGrowth = 0.14;

/// Sugar cane and cactus stop at three blocks.
constexpr i32 kColumnHeight = 3;

constexpr UnansweredTicker kUnanswered[] = {
    {"minecraft:bamboo", "growth and the leaf property are not modelled"},
    {"minecraft:bamboo_sapling", "does not become bamboo"},
    {"minecraft:mangrove_propagule", "a hanging propagule's age does not advance"},
    {"minecraft:vine", "vines do not spread"},
    {"minecraft:cave_vines", "cave vines do not grow or fruit"},
    {"minecraft:twisting_vines", "do not grow"},
    {"minecraft:weeping_vines", "do not grow"},
    {"minecraft:chorus_flower", "does not grow"},
    {"minecraft:turtle_egg", "does not crack"},
    {"minecraft:pointed_dripstone", "does not drip or grow"},
    {"minecraft:budding_amethyst", "does not bud"},
    {"minecraft:brown_mushroom", "mushrooms do not spread"},
    {"minecraft:red_mushroom", "mushrooms do not spread"},
    {"minecraft:crimson_nylium", "nylium does not die under a lid"},
    {"minecraft:warped_nylium", "nylium does not die under a lid"},
    {"minecraft:frosted_ice", "frost walker ice does not age"},
    {"minecraft:redstone_ore", "a lit ore does not go out on its random tick"},
    {"minecraft:deepslate_redstone_ore", "a lit ore does not go out on its random tick"},
    {"minecraft:nether_portal", "no zombified piglins"},
    {"minecraft:copper_block", "copper does not oxidise (every weathering copper block)"},
    {"minecraft:lava", "the fluid's random tick — lava lighting fire — is not modelled"},
    {"(chunk)", "precipitation: water freezing and snow settling in cold biomes, lightning"},
};

/// What a hand can plant, and on what.
enum class Support : u8 { Farmland, SoulSand, DirtOrFarmland, JungleLogSide, Cane, Cactus };

struct Seed {
    std::string_view item;
    std::string_view block;
    Support          support;
};

constexpr Seed kSeeds[] = {
    {"minecraft:wheat_seeds", "minecraft:wheat", Support::Farmland},
    {"minecraft:carrot", "minecraft:carrots", Support::Farmland},
    {"minecraft:potato", "minecraft:potatoes", Support::Farmland},
    {"minecraft:beetroot_seeds", "minecraft:beetroots", Support::Farmland},
    {"minecraft:torchflower_seeds", "minecraft:torchflower_crop", Support::Farmland},
    {"minecraft:pitcher_pod", "minecraft:pitcher_crop", Support::Farmland},
    {"minecraft:melon_seeds", "minecraft:melon_stem", Support::Farmland},
    {"minecraft:pumpkin_seeds", "minecraft:pumpkin_stem", Support::Farmland},
    {"minecraft:nether_wart", "minecraft:nether_wart", Support::SoulSand},
    {"minecraft:sweet_berries", "minecraft:sweet_berry_bush", Support::DirtOrFarmland},
    {"minecraft:cocoa_beans", "minecraft:cocoa", Support::JungleLogSide},
    {"minecraft:oak_sapling", "minecraft:oak_sapling", Support::DirtOrFarmland},
    {"minecraft:spruce_sapling", "minecraft:spruce_sapling", Support::DirtOrFarmland},
    {"minecraft:birch_sapling", "minecraft:birch_sapling", Support::DirtOrFarmland},
    {"minecraft:jungle_sapling", "minecraft:jungle_sapling", Support::DirtOrFarmland},
    {"minecraft:acacia_sapling", "minecraft:acacia_sapling", Support::DirtOrFarmland},
    {"minecraft:dark_oak_sapling", "minecraft:dark_oak_sapling", Support::DirtOrFarmland},
    {"minecraft:cherry_sapling", "minecraft:cherry_sapling", Support::DirtOrFarmland},
    {"minecraft:sugar_cane", "minecraft:sugar_cane", Support::Cane},
    {"minecraft:cactus", "minecraft:cactus", Support::Cactus},
};

[[nodiscard]] i32 parse_int(std::string_view text) noexcept {
    i32 value = -1;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace

// ── The resolved registry ───────────────────────────────────────────────────

struct Plants::Impl {
    const registry::BlockRegistry* blocks{nullptr};
    const registry::Registries*    registries{nullptr};

    std::vector<registry::ProtocolId> wire;
    std::vector<Kind>                 kind;
    std::vector<u8>                   ticks;

    std::optional<registry::TagId> logs, dirt, sand, maintains_farmland, jungle_logs, snow,
        replaceable;

    registry::BlockId air{kNoBlock}, dirt_block{kNoBlock}, farmland{kNoBlock}, water{kNoBlock},
        lava{kNoBlock}, sugar_cane{kNoBlock}, cactus{kNoBlock}, melon{kNoBlock},
        pumpkin{kNoBlock}, melon_stem{kNoBlock}, pumpkin_stem{kNoBlock},
        attached_melon_stem{kNoBlock}, attached_pumpkin_stem{kNoBlock}, kelp{kNoBlock},
        kelp_plant{kNoBlock}, frosted_ice{kNoBlock}, soul_sand{kNoBlock}, torchflower{kNoBlock},
        short_grass{kNoBlock}, tall_grass{kNoBlock}, fern{kNoBlock}, large_fern{kNoBlock},
        snow_layer{kNoBlock}, grass_block{kNoBlock}, mangrove_propagule{kNoBlock};

    [[nodiscard]] registry::BlockId find(std::string_view name) const {
        const auto id = blocks->find_block(name);
        return id ? *id : kNoBlock;
    }

    [[nodiscard]] bool in(const std::optional<registry::TagId>& tag,
                          registry::BlockId                     block) const noexcept {
        return tag && block.value() < wire.size() && wire[block.value()] >= 0 &&
               registries->tag_contains(*tag, wire[block.value()]);
    }

    [[nodiscard]] registry::BlockId of(registry::BlockStateId state) const noexcept {
        return blocks->block_of(state);
    }

    [[nodiscard]] Kind kind_of(registry::BlockId block) const noexcept {
        return block.value() < kind.size() ? kind[block.value()] : Kind::None;
    }

    [[nodiscard]] registry::BlockStateId def(registry::BlockId block) const noexcept {
        return blocks->default_state(block);
    }

    /// An integer property, or -1 when the block does not have it.
    [[nodiscard]] i32 get(registry::BlockStateId state, std::string_view name) const noexcept {
        const auto property = blocks->find_property(of(state), name);
        return property ? parse_int(blocks->property_value(state, *property)) : -1;
    }

    [[nodiscard]] bool flag(registry::BlockStateId state, std::string_view name) const noexcept {
        const auto property = blocks->find_property(of(state), name);
        return property && blocks->property_value(state, *property) == "true";
    }

    /// The top half of a two-block plant.
    [[nodiscard]] bool is_upper(registry::BlockStateId state) const noexcept {
        const auto property = blocks->find_property(of(state), "half");
        return property && blocks->property_value(state, *property) == "upper";
    }

    [[nodiscard]] i32 max_of(registry::BlockId block, std::string_view name) const noexcept {
        const auto property = blocks->find_property(block, name);
        return property ? static_cast<i32>(property->values.size()) - 1 : -1;
    }

    /// The same state with one property set by its text. Unchanged when the
    /// block has no such property or no such value — a bug, not a condition.
    [[nodiscard]] registry::BlockStateId set(registry::BlockStateId state, std::string_view name,
                                             std::string_view value) const noexcept {
        const auto property = blocks->find_property(of(state), name);
        if (!property) {
            return state;
        }
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                return blocks->with_property(state, *property, static_cast<u16>(i));
            }
        }
        return state;
    }

    [[nodiscard]] registry::BlockStateId set_int(registry::BlockStateId state,
                                                 std::string_view name, i32 value) const noexcept {
        std::array<char, 8> text{};
        const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{}) {
            return state;
        }
        return set(state, name, std::string_view{text.data(), static_cast<usize>(end - text.data())});
    }

    // ── Predicates ──────────────────────────────────────────────────────────

    /// Any water: a source, a flow, or a block holding it. Farmland and sugar
    /// cane both count a waterlogged slab as water.
    [[nodiscard]] bool is_water(registry::BlockStateId state) const noexcept {
        const registry::BlockId block = of(state);
        return block == water || (block != lava && blocks->holds_fluid(state));
    }

    /// Water whose amount is eight: a source, a falling column, or a block
    /// holding a source. What kills grass and what kelp grows into.
    [[nodiscard]] bool is_full_water(registry::BlockStateId state) const noexcept {
        const registry::BlockId block = of(state);
        if (block == water) {
            const i32 level = get(state, "level");
            return level == 0 || level >= 8;
        }
        return block != lava && blocks->holds_fluid(state);
    }

    /// A water or lava *block* — not a waterlogged one.
    [[nodiscard]] bool is_liquid_block(registry::BlockStateId state) const noexcept {
        const registry::BlockId block = of(state);
        return block == water || block == lava;
    }

    /// The game's legacy "solid" flag, which is Java code and in no report.
    /// Stood in for by the measured motion-blocking flag; the two agree on
    /// every block this file asks about (stone, glass, planks, a slab) and are
    /// named here because they are not the same property.
    [[nodiscard]] bool solid(registry::BlockStateId state) const noexcept {
        return blocks->blocks_motion(of(state));
    }

    [[nodiscard]] bool air_at(const world::LevelView& level, BlockPos pos) const {
        return blocks->is_air(of(level.block_at(pos)));
    }

    [[nodiscard]] bool supported(Kind k, const world::LevelView& level, BlockPos pos) const {
        const registry::BlockId below = of(level.block_at(pos.below()));
        switch (k) {
            case Kind::Crop:
            case Kind::Beetroot:
            case Kind::Torchflower:
            case Kind::Stem:
            case Kind::AttachedStem:
            case Kind::Pitcher: return below == farmland;
            case Kind::NetherWart: return below == soul_sand;
            case Kind::SweetBerry:
            case Kind::Sapling:
            case Kind::Azalea: return below == farmland || in(dirt, below);
            default: return true;
        }
    }

    [[nodiscard]] bool cane_survives(const world::LevelView& level, BlockPos pos) const {
        const BlockPos          ground = pos.below();
        const registry::BlockId below  = of(level.block_at(ground));
        if (below == sugar_cane) {
            return true;
        }
        if (!in(dirt, below) && !in(sand, below)) {
            return false;
        }
        for (const Horizontal& h : kHorizontal) {
            const registry::BlockStateId side = level.block_at(ground.offset(h.dx, 0, h.dz));
            if (is_water(side) || of(side) == frosted_ice) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool cactus_survives(const world::LevelView& level, BlockPos pos) const {
        for (const Horizontal& h : kHorizontal) {
            const registry::BlockStateId side = level.block_at(pos.offset(h.dx, 0, h.dz));
            if (solid(side) || of(side) == lava) {
                return false;
            }
        }
        const registry::BlockId below = of(level.block_at(pos.below()));
        return (below == cactus || in(sand, below)) && !is_liquid_block(level.block_at(pos.above()));
    }

    [[nodiscard]] bool farmland_survives(const world::LevelView& level, BlockPos pos) const {
        const registry::BlockStateId above = level.block_at(pos.above());
        if (!solid(above)) {
            return true;
        }
        const std::string_view name = blocks->block_name(of(above));
        return ends_with(name, "_fence_gate") || name == "minecraft:moving_piston";
    }

    /// Grass stays grass under this. A single snow layer is fine, a full water
    /// block is not, and otherwise anything that does not stop light.
    ///
    /// The game asks how much light the block above lets through its bottom
    /// face; the registry knows a per-block opacity measured with sky light, so
    /// a bottom slab — opaque as a block, open on its top face — reads as a lid
    /// here. Named, not hidden.
    [[nodiscard]] bool can_be_grass(const world::LevelView& level, BlockPos pos) const {
        const registry::BlockStateId above = level.block_at(pos.above());
        if (of(above) == snow_layer && get(above, "layers") == 1) {
            return true;
        }
        if (is_full_water(above)) {
            return false;
        }
        return blocks->light_opacity(of(above)) != registry::BlockRegistry::LightOpacity::Opaque;
    }

    [[nodiscard]] registry::BlockId fruit_of(registry::BlockId stem) const noexcept {
        return stem == melon_stem || stem == attached_melon_stem ? melon : pumpkin;
    }
    [[nodiscard]] registry::BlockId attached_of(registry::BlockId stem) const noexcept {
        return stem == melon_stem ? attached_melon_stem : attached_pumpkin_stem;
    }
    [[nodiscard]] registry::BlockId stem_of(registry::BlockId attached) const noexcept {
        return attached == attached_melon_stem ? melon_stem : pumpkin_stem;
    }

    /// Break a block the way a rule breaks one: its loot first, then air — or
    /// water, when it held some.
    void destroy(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                 registry::BlockStateId state) const {
        env.drop_block(pos, state);
        level.set_block(pos, flag(state, "waterlogged") ? def(water) : def(air));
    }

    // ── Growth ──────────────────────────────────────────────────────────────

    [[nodiscard]] static bool roll(PlantRandom& random, f32 speed) {
        // `(int)(25.0F / speed) + 1`: float division, truncated. A speed of
        // 9.25 gives 2.70 and so the same one in three as 10.
        const i32 bound = static_cast<i32>(25.0F / speed) + 1;
        return random.next_int(bound) == 0;
    }

    void grow_crop(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                   registry::BlockStateId state, Kind k, PlantRandom& random,
                   f32 speed) const {
        const i32 age = get(state, "age");
        const i32 max = k == Kind::Torchflower ? 2 : max_of(of(state), "age");
        if (age < 0 || age >= max) {
            return;
        }
        if (env.raw_brightness(pos) < kGrowthLight) {
            return;
        }
        if (!roll(random, speed)) {
            return;
        }
        set_age(level, pos, state, k, age + 1);
    }

    /// Write a crop at a new age. A torchflower crop that reaches its third
    /// stage is not a crop any more — it is the flower.
    void set_age(world::LevelWriter& level, BlockPos pos, registry::BlockStateId state, Kind k,
                 i32 age) const {
        if (k == Kind::Torchflower && age >= 2) {
            level.set_block(pos, def(torchflower));
            return;
        }
        level.set_block(pos, set_int(state, "age", age));
    }

    void place_fruit(world::LevelWriter& level, BlockPos pos, registry::BlockId stem,
                     PlantRandom& random) const {
        const Horizontal& h      = kHorizontal[static_cast<usize>(random.next_int(4))];
        const BlockPos    target = pos.offset(h.dx, 0, h.dz);
        const registry::BlockId ground = of(level.block_at(target.below()));
        if (!air_at(level, target) || !(ground == farmland || in(dirt, ground))) {
            return;
        }
        level.set_block(target, def(fruit_of(stem)));
        level.set_block(pos, set(def(attached_of(stem)), "facing", h.name));
    }

    void advance_sapling(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                         registry::BlockStateId state, PlantRandom& random) const {
        if (get(state, "stage") == 0) {
            level.set_block(pos, set_int(state, "stage", 1));
            return;
        }
        (void)env.grow_tree(level, pos, state, random);
    }

    void grow_column(world::LevelWriter& level, BlockPos pos, registry::BlockStateId state,
                     registry::BlockId self) const {
        if (!air_at(level, pos.above())) {
            return;
        }
        i32 height = 1;
        while (height < kColumnHeight && of(level.block_at(pos.below(height))) == self) {
            ++height;
        }
        if (height >= kColumnHeight) {
            return;
        }
        const i32 age = get(state, "age");
        if (age == 15) {
            level.set_block(pos.above(), def(self));
            level.set_block(pos, set_int(state, "age", 0));
        } else {
            level.set_block(pos, set_int(state, "age", age + 1));
        }
    }
};

// ── Construction ────────────────────────────────────────────────────────────

Plants::Plants(const registry::BlockRegistry& blocks, const registry::Registries& registries)
    : impl_{new Impl} {
    Impl& in = *impl_;
    in.blocks     = &blocks;
    in.registries = &registries;

    const auto block_registry = registries.find("minecraft:block");
    in.wire.assign(blocks.block_count(), -1);
    if (block_registry) {
        for (usize i = 0; i < blocks.block_count(); ++i) {
            const auto id = registries.protocol_id(
                *block_registry, blocks.block_name(registry::BlockId{static_cast<u16>(i)}));
            in.wire[i] = id ? *id : -1;
        }
        const auto tag = [&](std::string_view name) {
            return registries.find_tag(*block_registry, name);
        };
        in.logs               = tag("minecraft:logs");
        in.dirt               = tag("minecraft:dirt");
        in.sand               = tag("minecraft:sand");
        in.maintains_farmland = tag("minecraft:maintains_farmland");
        in.jungle_logs        = tag("minecraft:jungle_logs");
        in.snow               = tag("minecraft:snow");
        in.replaceable        = tag("minecraft:replaceable");
    }

    in.air                   = in.find("minecraft:air");
    in.dirt_block            = in.find("minecraft:dirt");
    in.farmland              = in.find("minecraft:farmland");
    in.water                 = in.find("minecraft:water");
    in.lava                  = in.find("minecraft:lava");
    in.sugar_cane            = in.find("minecraft:sugar_cane");
    in.cactus                = in.find("minecraft:cactus");
    in.melon                 = in.find("minecraft:melon");
    in.pumpkin               = in.find("minecraft:pumpkin");
    in.melon_stem            = in.find("minecraft:melon_stem");
    in.pumpkin_stem          = in.find("minecraft:pumpkin_stem");
    in.attached_melon_stem   = in.find("minecraft:attached_melon_stem");
    in.attached_pumpkin_stem = in.find("minecraft:attached_pumpkin_stem");
    in.kelp                  = in.find("minecraft:kelp");
    in.kelp_plant            = in.find("minecraft:kelp_plant");
    in.frosted_ice           = in.find("minecraft:frosted_ice");
    in.soul_sand             = in.find("minecraft:soul_sand");
    in.torchflower           = in.find("minecraft:torchflower");
    in.short_grass           = in.find("minecraft:grass");
    in.tall_grass            = in.find("minecraft:tall_grass");
    in.fern                  = in.find("minecraft:fern");
    in.large_fern            = in.find("minecraft:large_fern");
    in.snow_layer            = in.find("minecraft:snow");
    in.grass_block           = in.find("minecraft:grass_block");
    in.mangrove_propagule    = in.find("minecraft:mangrove_propagule");

    in.kind.assign(blocks.block_count(), Kind::None);
    const auto mark = [&](std::string_view name, Kind k) {
        const registry::BlockId id = in.find(name);
        if (id != kNoBlock && id.value() < in.kind.size()) {
            in.kind[id.value()] = k;
        }
    };
    mark("minecraft:wheat", Kind::Crop);
    mark("minecraft:carrots", Kind::Crop);
    mark("minecraft:potatoes", Kind::Crop);
    mark("minecraft:beetroots", Kind::Beetroot);
    mark("minecraft:torchflower_crop", Kind::Torchflower);
    mark("minecraft:pitcher_crop", Kind::Pitcher);
    mark("minecraft:nether_wart", Kind::NetherWart);
    mark("minecraft:melon_stem", Kind::Stem);
    mark("minecraft:pumpkin_stem", Kind::Stem);
    mark("minecraft:attached_melon_stem", Kind::AttachedStem);
    mark("minecraft:attached_pumpkin_stem", Kind::AttachedStem);
    mark("minecraft:farmland", Kind::Farmland);
    mark("minecraft:sugar_cane", Kind::SugarCane);
    mark("minecraft:cactus", Kind::Cactus);
    mark("minecraft:kelp", Kind::Kelp);
    mark("minecraft:sweet_berry_bush", Kind::SweetBerry);
    mark("minecraft:cocoa", Kind::Cocoa);
    for (const std::string_view sapling :
         {"minecraft:oak_sapling", "minecraft:spruce_sapling", "minecraft:birch_sapling",
          "minecraft:jungle_sapling", "minecraft:acacia_sapling", "minecraft:dark_oak_sapling",
          "minecraft:cherry_sapling", "minecraft:mangrove_propagule"}) {
        mark(sapling, Kind::Sapling);
    }
    mark("minecraft:azalea", Kind::Azalea);
    mark("minecraft:flowering_azalea", Kind::Azalea);
    for (const std::string_view leaves :
         {"minecraft:oak_leaves", "minecraft:spruce_leaves", "minecraft:birch_leaves",
          "minecraft:jungle_leaves", "minecraft:acacia_leaves", "minecraft:dark_oak_leaves",
          "minecraft:mangrove_leaves", "minecraft:cherry_leaves", "minecraft:azalea_leaves",
          "minecraft:flowering_azalea_leaves"}) {
        mark(leaves, Kind::Leaves);
    }
    mark("minecraft:grass_block", Kind::Spreading);
    mark("minecraft:mycelium", Kind::Spreading);
    mark("minecraft:podzol", Kind::Snowy);
    mark("minecraft:ice", Kind::Ice);
    mark("minecraft:snow", Kind::SnowLayer);
    mark("minecraft:grass", Kind::ShortGrass);
    mark("minecraft:fern", Kind::ShortGrass);

    // Per state: could a random tick here do anything? Decided once, so the
    // question the driver asks four thousand times a second is an index.
    in.ticks.assign(blocks.state_count(), 0);
    for (usize s = 0; s < blocks.state_count(); ++s) {
        const registry::BlockStateId state{static_cast<u16>(s)};
        const registry::BlockId      block = in.of(state);
        bool                         yes   = false;
        switch (in.kind_of(block)) {
            case Kind::Crop:
            case Kind::Beetroot: yes = in.get(state, "age") < in.max_of(block, "age"); break;
            case Kind::Torchflower:
            case Kind::Stem:
            case Kind::Farmland:
            case Kind::SugarCane:
            case Kind::Cactus:
            case Kind::Spreading:
            case Kind::Ice:
            case Kind::SnowLayer: yes = true; break;
            case Kind::NetherWart:
            case Kind::SweetBerry: yes = in.get(state, "age") < 3; break;
            case Kind::Kelp: yes = in.get(state, "age") < 25; break;
            case Kind::Cocoa: yes = in.get(state, "age") < 2; break;
            case Kind::Pitcher: yes = !in.is_upper(state) && in.get(state, "age") < 4; break;
            case Kind::Sapling: yes = !in.flag(state, "hanging"); break;
            case Kind::Leaves:
                yes = !in.flag(state, "persistent") && in.get(state, "distance") == 7;
                break;
            default: break;
        }
        in.ticks[s] = yes ? 1 : 0;
    }
}

Plants::~Plants() { delete impl_; }

Plants::Plants(Plants&& other) noexcept : impl_{std::exchange(other.impl_, nullptr)} {}

Plants& Plants::operator=(Plants&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

std::span<const UnansweredTicker> Plants::unanswered() noexcept { return kUnanswered; }

bool Plants::ticks_randomly(registry::BlockStateId state) const noexcept {
    return state.value() < impl_->ticks.size() && impl_->ticks[state.value()] != 0;
}

// ── Formulas ────────────────────────────────────────────────────────────────

f32 Plants::growth_speed(const world::LevelView& level, BlockPos pos) const {
    const Impl&             in    = *impl_;
    const registry::BlockId crop  = in.of(level.block_at(pos));
    const BlockPos          below = pos.below();

    // The farmland under the crop is worth 1 dry and 3 moist, on top of a base
    // of 1; each of the eight around it a quarter of that. Wiki, Tutorials/Crop
    // farming: "a base speed level of 2 if dry or 4 if hydrated", "each
    // surrounding dry farmland adds 0.25, hydrated 0.75". Every term is a
    // multiple of a quarter, so the float sum is exact.
    f32 speed = 1.0F;
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            const registry::BlockStateId under = level.block_at(below.offset(dx, 0, dz));
            f32                          value = 0.0F;
            if (in.of(under) == in.farmland) {
                value = in.get(under, "moisture") > 0 ? 3.0F : 1.0F;
            }
            if (dx != 0 || dz != 0) {
                value /= 4.0F;
            }
            speed += value;
        }
    }

    // "If the same crop is planted on a diagonal or if the same crop is found
    // in both the north-south and east-west directions, the speed level is
    // halved."
    const auto same = [&](i32 dx, i32 dz) { return in.of(level.block_at(pos.offset(dx, 0, dz))) == crop; };
    const bool north_south = same(0, -1) || same(0, 1);
    const bool west_east   = same(-1, 0) || same(1, 0);
    const bool diagonal    = same(-1, -1) || same(1, -1) || same(1, 1) || same(-1, 1);
    if ((west_east && north_south) || diagonal) {
        speed /= 2.0F;
    }
    return speed;
}

i32 Plants::leaf_distance(const world::LevelView& level, BlockPos pos) const {
    const Impl& in   = *impl_;
    i32         best = 7;
    for (const auto& d : kSix) {
        const registry::BlockStateId next  = level.block_at(pos.offset(d[0], d[1], d[2]));
        const registry::BlockId      block = in.of(next);
        i32                          here  = 7;
        if (in.in(in.logs, block)) {
            // Measured: oak_log, stripped_oak_log, oak_wood and crimson_stem
            // all hold leaves; mangrove_roots and oak_planks hold none.
            // Exactly `#minecraft:logs`.
            here = 0;
        } else if (in.kind_of(block) == Kind::Leaves) {
            // Any leaves, whatever the kind: measured on a line of ten
            // different leaf types, distances 1..6 then decay.
            here = in.get(next, "distance");
        }
        best = std::min(best, here + 1);
    }
    return std::min(best, 7);
}

bool Plants::farmland_near_water(const world::LevelView& level, BlockPos pos) const {
    // Measured, 169 cells per plot: hydrated exactly within four blocks
    // horizontally, diagonals included, with the water at the farmland's own
    // level or one above. One below and two above hydrate nothing.
    for (i32 dy = 0; dy <= 1; ++dy) {
        for (i32 dz = -4; dz <= 4; ++dz) {
            for (i32 dx = -4; dx <= 4; ++dx) {
                if (impl_->is_water(level.block_at(pos.offset(dx, dy, dz)))) {
                    return true;
                }
            }
        }
    }
    return false;
}

// ── The random tick ─────────────────────────────────────────────────────────

void Plants::random_tick(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                         registry::BlockStateId state, PlantRandom& random) const {
    const Impl&             in    = *impl_;
    const registry::BlockId block = in.of(state);
    const Kind              k     = in.kind_of(block);

    switch (k) {
        case Kind::Crop:
        case Kind::Torchflower: {
            // The torchflower was first written like the beetroot below and the
            // measurement rejected it (p = 0.008 against 2/9, p = 1.0 against
            // 1/3): it runs the ordinary crop rule, no extra draw.
            in.grow_crop(level, env, pos, state, k, random, growth_speed(level, pos));
            break;
        }
        case Kind::Beetroot: {
            // The draw comes first and always happens: two times in three the
            // crop rule runs, the third it does not.
            if (random.next_int(kBeetrootOneIn) == 0) {
                break;
            }
            in.grow_crop(level, env, pos, state, k, random, growth_speed(level, pos));
            break;
        }
        case Kind::Pitcher: {
            // Measured: the ordinary crop rule at 1/3 on the sparse tiles
            // (p = 0.33), five stages, and from the fourth (age 3) a second
            // block on top carrying the same age. Only the lower half ticks.
            if (in.is_upper(state)) {
                break;
            }
            const i32 age = in.get(state, "age");
            if (age >= 4 || env.raw_brightness(pos) < kGrowthLight ||
                !Impl::roll(random, growth_speed(level, pos))) {
                break;
            }
            const i32 grown = age + 1;
            if (grown >= 3) {
                // The upper half needs room. Not measured: every sampled
                // pitcher had air above it.
                const registry::BlockStateId above = level.block_at(pos.above());
                if (!in.blocks->is_air(in.of(above)) && in.of(above) != block) {
                    break;
                }
                level.set_block(pos.above(), in.set_int(in.set(state, "half", "upper"), "age", grown));
            }
            level.set_block(pos, in.set_int(state, "age", grown));
            break;
        }
        case Kind::NetherWart: {
            const i32 age = in.get(state, "age");
            if (age < 3 && random.next_int(10) == 0) {
                level.set_block(pos, in.set_int(state, "age", age + 1));
            }
            break;
        }
        case Kind::SweetBerry: {
            const i32 age = in.get(state, "age");
            if (age < 3 && random.next_int(5) == 0 &&
                env.raw_brightness(pos.above()) >= kGrowthLight) {
                level.set_block(pos, in.set_int(state, "age", age + 1));
            }
            break;
        }
        case Kind::Cocoa: {
            // The draw first, unconditionally, then the age.
            const bool lucky = random.next_int(5) == 0;
            const i32  age   = in.get(state, "age");
            if (lucky && age < 2) {
                level.set_block(pos, in.set_int(state, "age", age + 1));
            }
            break;
        }
        case Kind::Stem: {
            if (env.raw_brightness(pos) < kGrowthLight) {
                break;
            }
            if (!Impl::roll(random, growth_speed(level, pos))) {
                break;
            }
            const i32 age = in.get(state, "age");
            if (age < 7) {
                level.set_block(pos, in.set_int(state, "age", age + 1));
            } else {
                in.place_fruit(level, pos, block, random);
            }
            break;
        }
        case Kind::Farmland: {
            const i32 moisture = in.get(state, "moisture");
            if (!farmland_near_water(level, pos) && !env.is_raining_at(pos.above())) {
                if (moisture > 0) {
                    level.set_block(pos, in.set_int(state, "moisture", moisture - 1));
                } else if (!in.in(in.maintains_farmland, in.of(level.block_at(pos.above())))) {
                    level.set_block(pos, in.def(in.dirt_block));
                }
            } else if (moisture < 7) {
                level.set_block(pos, in.set_int(state, "moisture", 7));
            }
            break;
        }
        case Kind::SugarCane: {
            if (!in.cane_survives(level, pos)) {
                in.destroy(level, env, pos, state);
                break;
            }
            in.grow_column(level, pos, state, block);
            break;
        }
        case Kind::Cactus: {
            in.grow_column(level, pos, state, block);
            // A cactus grows even where the new block cannot stand, and the
            // new block breaks at once. The check is a scheduled tick so that
            // it happens through the same path a neighbour change takes.
            const BlockPos top = pos.above();
            if (in.of(level.block_at(top)) == block && !in.cactus_survives(level, top)) {
                level.schedule_tick(top, level.blocks().block_name(block), 1,
                                    world::TickQueue::Block);
            }
            break;
        }
        case Kind::Kelp: {
            const i32 age = in.get(state, "age");
            if (age >= 25 || random.next_double() >= kKelpGrowth) {
                break;
            }
            const registry::BlockStateId above = level.block_at(pos.above());
            if (in.of(above) != in.water || !in.is_full_water(above)) {
                break;
            }
            level.set_block(pos.above(), in.set_int(in.def(in.kelp), "age", age + 1));
            level.set_block(pos, in.def(in.kelp_plant));
            break;
        }
        case Kind::Sapling: {
            if (in.flag(state, "hanging")) {
                break;
            }
            if (env.local_brightness(pos.above()) >= kGrowthLight && random.next_int(7) == 0) {
                in.advance_sapling(level, env, pos, state, random);
            }
            break;
        }
        case Kind::Leaves: {
            if (!in.flag(state, "persistent") && in.get(state, "distance") == 7) {
                in.destroy(level, env, pos, state);
            }
            break;
        }
        case Kind::Spreading: {
            if (!in.can_be_grass(level, pos)) {
                level.set_block(pos, in.def(in.dirt_block));
                break;
            }
            if (env.local_brightness(pos.above()) < kGrowthLight) {
                break;
            }
            const registry::BlockStateId base = in.def(block);
            for (i32 i = 0; i < 4; ++i) {
                // Three draws in this order. Named locals: C++ would evaluate
                // the operands of one expression in any order it liked.
                const i32 dx = random.next_int(3) - 1;
                const i32 dy = random.next_int(5) - 3;
                const i32 dz = random.next_int(3) - 1;
                const BlockPos target = pos.offset(dx, dy, dz);
                if (in.of(level.block_at(target)) != in.dirt_block) {
                    continue;
                }
                if (!in.can_be_grass(level, target) ||
                    in.is_water(level.block_at(target.above()))) {
                    continue;
                }
                const bool snowy = in.in(in.snow, in.of(level.block_at(target.above())));
                level.set_block(target, in.set(base, "snowy", snowy ? "true" : "false"));
            }
            break;
        }
        case Kind::Ice: {
            if (env.block_light(pos) > kIceMeltsAbove) {
                level.set_block(pos, level.traits().ultrawarm ? in.def(in.air) : in.def(in.water));
            }
            break;
        }
        case Kind::SnowLayer: {
            if (env.block_light(pos) > kSnowMeltsAbove) {
                in.destroy(level, env, pos, state);
            }
            break;
        }
        default: break;
    }
}

// ── Scheduled ticks ─────────────────────────────────────────────────────────

bool Plants::scheduled_tick(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                            registry::BlockId block) const {
    const Impl& in = *impl_;
    const Kind  k  = in.kind_of(block);
    if (k != Kind::Leaves && k != Kind::Cactus && k != Kind::SugarCane && k != Kind::Farmland) {
        return false;
    }
    const registry::BlockStateId state = level.block_at(pos);
    if (in.of(state) != block) {
        // The block changed since the tick was asked for. The tick was ours;
        // it has nothing left to do.
        return true;
    }
    switch (k) {
        case Kind::Leaves: {
            const i32 distance = leaf_distance(level, pos);
            if (distance != in.get(state, "distance")) {
                level.set_block(pos, in.set_int(state, "distance", distance));
            }
            break;
        }
        case Kind::Cactus:
            if (!in.cactus_survives(level, pos)) {
                in.destroy(level, env, pos, state);
            }
            break;
        case Kind::SugarCane:
            if (!in.cane_survives(level, pos)) {
                in.destroy(level, env, pos, state);
            }
            break;
        case Kind::Farmland:
            if (!in.farmland_survives(level, pos)) {
                level.set_block(pos, in.def(in.dirt_block));
            }
            break;
        default: break;
    }
    return true;
}

// ── Neighbour changes ───────────────────────────────────────────────────────

void Plants::neighbour_changed(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                               BlockPos from) const {
    const Impl&                  in    = *impl_;
    const registry::BlockStateId state = level.block_at(pos);
    const registry::BlockId      block = in.of(state);
    const Kind                   k     = in.kind_of(block);
    const auto                   ask   = [&] {
        const std::string_view name = level.blocks().block_name(block);
        if (!level.has_scheduled_tick(pos, name, world::TickQueue::Block)) {
            level.schedule_tick(pos, name, 1, world::TickQueue::Block);
        }
    };

    switch (k) {
        case Kind::Crop:
        case Kind::Beetroot:
        case Kind::Torchflower:
        case Kind::Stem:
        case Kind::NetherWart:
        case Kind::SweetBerry:
        case Kind::Sapling:
        case Kind::Azalea:
            if (!in.supported(k, level, pos)) {
                in.destroy(level, env, pos, state);
            }
            break;
        case Kind::Pitcher: {
            if (in.is_upper(state)) {
                // The top half stands on its own lower half and nothing else.
                // It carries no loot of its own: the lower half's drop is the
                // plant's.
                if (in.of(level.block_at(pos.below())) != block) {
                    level.set_block(pos, in.def(in.air));
                }
            } else if (!in.supported(k, level, pos)) {
                in.destroy(level, env, pos, state);
            }
            break;
        }
        case Kind::AttachedStem: {
            if (!in.supported(k, level, pos)) {
                in.destroy(level, env, pos, state);
                break;
            }
            const auto facing = in.blocks->find_property(block, "facing");
            if (!facing) {
                break;
            }
            const std::string_view side = in.blocks->property_value(state, *facing);
            for (const Horizontal& h : kHorizontal) {
                if (h.name == side &&
                    in.of(level.block_at(pos.offset(h.dx, 0, h.dz))) != in.fruit_of(block)) {
                    // The fruit is gone: the stem straightens, fully grown.
                    level.set_block(pos, in.set_int(in.def(in.stem_of(block)), "age", 7));
                }
            }
            break;
        }
        case Kind::Cocoa: {
            const auto facing = in.blocks->find_property(block, "facing");
            if (!facing) {
                break;
            }
            const std::string_view side = in.blocks->property_value(state, *facing);
            for (const Horizontal& h : kHorizontal) {
                if (h.name == side &&
                    !in.in(in.jungle_logs, in.of(level.block_at(pos.offset(h.dx, 0, h.dz))))) {
                    in.destroy(level, env, pos, state);
                }
            }
            break;
        }
        case Kind::Leaves:
            if (leaf_distance(level, pos) != in.get(state, "distance")) {
                ask();
            }
            break;
        case Kind::Farmland:
            if (from == pos.above() && !in.farmland_survives(level, pos)) {
                ask();
            }
            break;
        case Kind::SugarCane:
            if (!in.cane_survives(level, pos)) {
                ask();
            }
            break;
        case Kind::Cactus:
            if (!in.cactus_survives(level, pos)) {
                ask();
            }
            break;
        case Kind::Spreading:
        case Kind::Snowy:
            if (from == pos.above()) {
                const bool snowy = in.in(in.snow, in.of(level.block_at(from)));
                if (snowy != in.flag(state, "snowy")) {
                    level.set_block(pos, in.set(state, "snowy", snowy ? "true" : "false"));
                }
            }
            break;
        default: break;
    }
}

// ── Bone meal ───────────────────────────────────────────────────────────────

UseOutcome Plants::bone_meal(world::LevelWriter& level, PlantEnvironment& env, BlockPos pos,
                             PlantRandom& random) const {
    const Impl&                  in    = *impl_;
    const registry::BlockStateId state = level.block_at(pos);
    const registry::BlockId      block = in.of(state);
    const Kind                   k     = in.kind_of(block);

    UseOutcome used;
    used.result      = UseResult::Success;
    used.consume_one = true;

    switch (k) {
        case Kind::Crop:
        case Kind::Beetroot: {
            const i32 age = in.get(state, "age");
            const i32 max = in.max_of(block, "age");
            if (age >= max) {
                return {};
            }
            // Two to five stages, uniformly, clamped at the top. Measured on
            // wheat: 24 / 25 / 25 / 26 of 100 at +2 / +3 / +4 / +5.
            //
            // A beetroot takes the same draw divided by three: +0 once in four,
            // +1 three times in four, never +2 — the wiki's "75 % chance of
            // growing to the next stage". Measured: 1111 of 1500 beetroots,
            // every dispenser checked to have fired, went up one stage (0.741;
            // z = -0.8 against 3/4, +6.1 against 2/3) and none went up two.
            const i32 draw  = 2 + random.next_int(4);
            const i32 step  = k == Kind::Beetroot ? draw / 3 : draw;
            const i32 grown = std::min(max, age + step);
            level.set_block(pos, in.set_int(state, "age", grown));
            return used;
        }
        case Kind::Torchflower: {
            in.set_age(level, pos, state, k, in.get(state, "age") + 1);
            return used;
        }
        case Kind::Stem: {
            const i32 age = in.get(state, "age");
            if (age >= 7) {
                return {};
            }
            const i32 grown = std::min(7, age + 2 + random.next_int(4));
            level.set_block(pos, in.set_int(state, "age", grown));
            if (grown == 7) {
                // A stem brought to 7 by bone meal takes a random tick on the
                // spot. Measured: 100 stems at age 5, each one bone meal — all
                // 100 reached 7 and **7** had set a melon at once, every one on
                // the only free side. One ordinary roll gives 1/5 here times
                // 1/4 for the side: 5 %.
                random_tick(level, env, pos, level.block_at(pos), random);
            }
            return used;
        }
        case Kind::Sapling:
        case Kind::Azalea: {
            if (in.flag(state, "hanging")) {
                used.result      = UseResult::Pass;
                used.consume_one = false;
                used.unsupported = "bone meal on a hanging mangrove propagule";
                return used;
            }
            // The bone meal is spent whether or not the roll succeeds.
            if (random.next_float() < kSaplingBoneMeal) {
                if (k == Kind::Azalea) {
                    (void)env.grow_tree(level, pos, state, random);
                } else {
                    in.advance_sapling(level, env, pos, state, random);
                }
            }
            return used;
        }
        case Kind::SweetBerry: {
            const i32 age = in.get(state, "age");
            if (age >= 3) {
                return {};
            }
            level.set_block(pos, in.set_int(state, "age", age + 1));
            return used;
        }
        case Kind::Cocoa: {
            const i32 age = in.get(state, "age");
            if (age >= 2) {
                return {};
            }
            level.set_block(pos, in.set_int(state, "age", age + 1));
            return used;
        }
        case Kind::ShortGrass: {
            const BlockPos above = pos.above();
            if (!in.air_at(level, above)) {
                return {};
            }
            const registry::BlockId tall = block == in.fern ? in.large_fern : in.tall_grass;
            level.set_block(pos, in.set(in.def(tall), "half", "lower"));
            level.set_block(above, in.set(in.def(tall), "half", "upper"));
            return used;
        }
        case Kind::Spreading: {
            if (block != in.grass_block) {
                return {};
            }
            // Grass on grass, around the target. Specified from the wiki's
            // description — grass and flowers sprout on the grass blocks near
            // the one clicked — as 128 short random walks from the block
            // above; the flowers are **not** placed (they come from the
            // biome's flower feature, and a biome is not on this interface),
            // and the shape of the patch is not measured.
            if (!in.air_at(level, pos.above())) {
                return {};
            }
            // Where a flower would have gone. In the game the flower fills the
            // cell and every later walk that lands there does nothing; left as
            // air here, the same cell was filled with grass by a later walk,
            // and the patch came out a third too dense — 18.8 grass against
            // the real game's 14.45. Remembered, so it stays empty.
            std::array<BlockPos, 128> flower_cells{};
            usize                     flowers = 0;
            const auto flower_at = [&](BlockPos at) {
                return std::ranges::find(flower_cells.begin(),
                                         flower_cells.begin() + static_cast<std::ptrdiff_t>(flowers),
                                         at) != flower_cells.begin() + static_cast<std::ptrdiff_t>(flowers);
            };
            for (i32 attempt = 0; attempt < 128; ++attempt) {
                BlockPos at    = pos.above();
                bool     valid = true;
                for (i32 step = 0; step < attempt / 16; ++step) {
                    const i32 dx     = random.next_int(3) - 1;
                    const i32 dy_raw = random.next_int(3) - 1;
                    const i32 dy_mul = random.next_int(3);
                    const i32 dz     = random.next_int(3) - 1;
                    at = at.offset(dx, dy_raw * dy_mul / 2, dz);
                    if (in.of(level.block_at(at.below())) != in.grass_block ||
                        in.solid(level.block_at(at))) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    continue;
                }
                const registry::BlockStateId here = level.block_at(at);
                if (in.of(here) == in.short_grass) {
                    if (random.next_int(10) == 0 && in.air_at(level, at.above())) {
                        level.set_block(at, in.set(in.def(in.tall_grass), "half", "lower"));
                        level.set_block(at.above(), in.set(in.def(in.tall_grass), "half", "upper"));
                    }
                    continue;
                }
                if (!in.blocks->is_air(in.of(here)) || flower_at(at)) {
                    continue;
                }
                if (random.next_int(8) == 0) {
                    // A flower would go here. Named in `unsupported` below
                    // rather than replaced by grass.
                    flower_cells[flowers++] = at;
                    continue;
                }
                level.set_block(at, in.def(in.short_grass));
            }
            used.unsupported = "bone meal on grass: flowers are not placed";
            return used;
        }
        case Kind::Kelp:
            used.result      = UseResult::Pass;
            used.consume_one = false;
            used.unsupported = "bone meal on kelp";
            return used;
        case Kind::Pitcher:
            used.result      = UseResult::Pass;
            used.consume_one = false;
            used.unsupported = "bone meal on a pitcher crop";
            return used;
        default: break;
    }

    // Other things bone meal works on, named so a click on one says so.
    const std::string_view name = in.blocks->block_name(block);
    for (const std::string_view other :
         {"minecraft:moss_block", "minecraft:brown_mushroom", "minecraft:red_mushroom",
          "minecraft:bamboo", "minecraft:bamboo_sapling", "minecraft:cave_vines",
          "minecraft:cave_vines_plant", "minecraft:glow_lichen", "minecraft:big_dripleaf",
          "minecraft:small_dripleaf", "minecraft:sea_pickle", "minecraft:seagrass",
          "minecraft:twisting_vines", "minecraft:weeping_vines", "minecraft:crimson_nylium",
          "minecraft:warped_nylium", "minecraft:rooted_dirt", "minecraft:mangrove_leaves",
          "minecraft:pink_petals", "minecraft:kelp_plant", "minecraft:crimson_fungus",
          "minecraft:warped_fungus", "minecraft:dandelion", "minecraft:poppy",
          "minecraft:sunflower", "minecraft:lilac", "minecraft:rose_bush", "minecraft:peony"}) {
        if (name == other) {
            UseOutcome out;
            out.result      = UseResult::Pass;
            out.unsupported = "bone meal on this block is not modelled";
            return out;
        }
    }
    return {};
}

// ── Planting ────────────────────────────────────────────────────────────────

bool Plants::is_plantable(std::string_view item) const noexcept {
    return std::ranges::any_of(kSeeds, [&](const Seed& seed) { return seed.item == item; });
}

PlantOutcome Plants::plant(world::LevelWriter& level, BlockPos clicked, i32 face,
                           std::string_view item) const {
    const Impl& in   = *impl_;
    const auto  seed = std::ranges::find_if(kSeeds, [&](const Seed& s) { return s.item == item; });
    if (seed == std::end(kSeeds)) {
        return {};
    }
    const registry::BlockId block = in.find(seed->block);
    if (block == kNoBlock) {
        return {};
    }

    const auto replaceable = [&](registry::BlockStateId state) {
        const registry::BlockId b = in.of(state);
        return in.blocks->is_air(b) || (in.in(in.replaceable, b) && !in.is_liquid_block(state));
    };

    // A clicked tuft of grass is replaced rather than built against.
    const registry::BlockStateId clicked_state = level.block_at(clicked);
    BlockPos                     target        = clicked;
    if (in.blocks->is_air(in.of(clicked_state)) || !replaceable(clicked_state) ||
        seed->support == Support::JungleLogSide) {
        target = ItemUse::offset_by_face(clicked, face);
    }
    if (!replaceable(level.block_at(target))) {
        return {};
    }

    registry::BlockStateId state = in.def(block);
    const registry::BlockId below = in.of(level.block_at(target.below()));
    bool                    ok    = false;
    switch (seed->support) {
        case Support::Farmland: ok = below == in.farmland; break;
        case Support::SoulSand: ok = below == in.soul_sand; break;
        case Support::DirtOrFarmland: ok = below == in.farmland || in.in(in.dirt, below); break;
        case Support::Cane: ok = in.cane_survives(level, target); break;
        case Support::Cactus: ok = in.cactus_survives(level, target); break;
        case Support::JungleLogSide: {
            // Faces 2..5 are north, south, west, east. The pod hangs on the
            // side that was clicked and faces back towards the log.
            constexpr std::string_view kTowardsLog[] = {"", "", "south", "north", "east", "west"};
            ok = face >= 2 && face <= 5 && in.in(in.jungle_logs, in.of(clicked_state));
            if (ok) {
                state = in.set(state, "facing", kTowardsLog[static_cast<usize>(face)]);
            }
            break;
        }
    }
    if (!ok) {
        return {};
    }
    level.set_block(target, state);
    return PlantOutcome{.planted = true, .at = target};
}

}  // namespace ov::gameplay
