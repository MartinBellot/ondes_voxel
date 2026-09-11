// Fire. See fire.hpp for the shape of it and docs/provenance/feu.md for where
// every number comes from, and which of them vanilla has confirmed.
#include "ov/gameplay/fire.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace ov::gameplay {

namespace {

constexpr registry::BlockId kNoBlock{0xFFFF};

// ── The table ───────────────────────────────────────────────────────────────
//
// The Minecraft Wiki's "Fire" article, table "Flammable blocks", restricted to
// the blocks 1.20.1 has (the pale garden rows are 1.21's). Ignite odds, burn
// odds, and the article's "catches from lava" column, which is vanilla's
// `ignitedByLava` block property for these rows. The `blocks` campaign puts a
// sample of ten rows in front of a real fire: see docs/provenance/feu.md.

struct Row {
    u8   ignite;
    u8   burn;
    bool lava;
};

constexpr std::array<std::string_view, 16> kColours{
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

/// Overworld woods: the nether's two do not burn.
constexpr std::array<std::string_view, 9> kWoods{"oak",      "spruce",   "birch",
                                                 "jungle",   "acacia",   "dark_oak",
                                                 "mangrove", "cherry",   "bamboo"};

struct Named {
    std::string_view name;
    Row              row;
};

constexpr std::array kNamed{
    Named{"bamboo_block", {5, 5, true}},
    Named{"stripped_bamboo_block", {5, 5, true}},
    Named{"coal_block", {5, 5, true}},
    Named{"bamboo_mosaic", {5, 20, true}},
    Named{"bamboo_mosaic_slab", {5, 20, true}},
    Named{"bamboo_mosaic_stairs", {5, 20, true}},
    Named{"mangrove_roots", {5, 20, true}},
    Named{"composter", {5, 20, true}},
    Named{"beehive", {5, 20, true}},
    Named{"target", {15, 20, true}},
    Named{"cave_vines", {15, 60, false}},
    Named{"cave_vines_plant", {15, 60, false}},
    Named{"tnt", {15, 100, true}},
    Named{"vine", {15, 100, true}},
    Named{"glow_lichen", {15, 100, true}},
    Named{"bookshelf", {30, 20, true}},
    Named{"lectern", {30, 20, true}},
    Named{"bee_nest", {30, 20, true}},
    Named{"azalea_leaves", {30, 60, true}},
    Named{"flowering_azalea_leaves", {30, 60, true}},
    Named{"hanging_roots", {30, 60, true}},
    Named{"azalea", {30, 60, false}},
    Named{"flowering_azalea", {30, 60, false}},
    Named{"dried_kelp_block", {30, 60, false}},
    Named{"hay_block", {60, 20, false}},
    Named{"bamboo", {60, 60, true}},
    Named{"scaffolding", {60, 60, false}},
    // one-block flowers and the berry bush: no lava
    Named{"dandelion", {60, 100, false}},
    Named{"poppy", {60, 100, false}},
    Named{"blue_orchid", {60, 100, false}},
    Named{"allium", {60, 100, false}},
    Named{"azure_bluet", {60, 100, false}},
    Named{"red_tulip", {60, 100, false}},
    Named{"orange_tulip", {60, 100, false}},
    Named{"white_tulip", {60, 100, false}},
    Named{"pink_tulip", {60, 100, false}},
    Named{"oxeye_daisy", {60, 100, false}},
    Named{"cornflower", {60, 100, false}},
    Named{"lily_of_the_valley", {60, 100, false}},
    Named{"wither_rose", {60, 100, false}},
    Named{"torchflower", {60, 100, false}},
    Named{"sweet_berry_bush", {60, 100, false}},
    // two-block flowers, grasses, ferns: lava lights them
    Named{"sunflower", {60, 100, true}},
    Named{"lilac", {60, 100, true}},
    Named{"rose_bush", {60, 100, true}},
    Named{"peony", {60, 100, true}},
    Named{"pitcher_plant", {60, 100, true}},
    Named{"grass", {60, 100, true}},
    Named{"tall_grass", {60, 100, true}},
    Named{"fern", {60, 100, true}},
    Named{"large_fern", {60, 100, true}},
    Named{"dead_bush", {60, 100, true}},
    Named{"big_dripleaf", {60, 100, false}},
    Named{"big_dripleaf_stem", {60, 100, false}},
    Named{"small_dripleaf", {60, 100, false}},
    Named{"spore_blossom", {60, 100, false}},
    Named{"pink_petals", {60, 100, false}},
};

/// Every row, expanded into full names.
[[nodiscard]] std::vector<std::pair<std::string, Row>> table_rows() {
    std::vector<std::pair<std::string, Row>> out;
    const auto add = [&](std::string name, Row row) {
        out.emplace_back("minecraft:" + std::move(name), row);
    };
    for (const std::string_view wood : kWoods) {
        const std::string w{wood};
        add(w + "_planks", {5, 20, true});
        add(w + "_slab", {5, 20, true});
        add(w + "_fence_gate", {5, 20, true});
        add(w + "_fence", {5, 20, true});
        add(w + "_stairs", {5, 20, true});
        if (wood == "bamboo") {
            continue;  // bamboo has blocks, not logs: in kNamed
        }
        add(w + "_log", {5, 5, true});
        add(w + "_wood", {5, 5, true});
        add("stripped_" + w + "_log", {5, 5, true});
        add("stripped_" + w + "_wood", {5, 5, true});
        add(w + "_leaves", {30, 60, true});
    }
    for (const std::string_view colour : kColours) {
        const std::string c{colour};
        add(c + "_wool", {30, 60, true});
        add(c + "_carpet", {60, 20, true});
    }
    for (const Named& named : kNamed) {
        add(std::string{named.name}, named.row);
    }
    return out;
}

// Blocks lava lights that fire itself does not burn. Vanilla's `ignitedByLava`
// is a property of its own and may reach wooden blocks outside the table (a
// crafting table, a chest). Empty until the `lava` campaign's crafting-table
// rig says so: a guessed list would light fires vanilla does not.
constexpr std::array<std::string_view, 0> kLavaOnly{};

[[nodiscard]] Direction dir(u8 i) { return static_cast<Direction>(i); }

}  // namespace

struct FireRules::Impl {
    const registry::BlockRegistry* blocks{nullptr};
    const registry::Registries*    registries{nullptr};

    std::vector<FireOdds> odds;
    std::vector<u8>       lava;
    std::vector<u8>       infiniburn[3];
    std::vector<u8>       soul_base;
    /// The `waterlogged` property of each block, stride 0 when it has none.
    std::vector<registry::PropertyView> waterlogged;

    registry::BlockId fire{kNoBlock}, soul_fire{kNoBlock}, lava_block{kNoBlock}, tnt{kNoBlock},
        campfire{kNoBlock}, soul_campfire{kNoBlock};
    registry::PropertyView age, north, east, south, west, up;
    registry::BlockStateId fire_default{}, soul_fire_default{}, air{registry::kAirState};
    usize                  unknown{0};

    [[nodiscard]] registry::BlockId of(registry::BlockStateId state) const noexcept {
        return blocks->block_of(state);
    }

    [[nodiscard]] bool flag(const std::vector<u8>& set, registry::BlockId block) const noexcept {
        return block.value() < set.size() && set[block.value()] != 0;
    }

    [[nodiscard]] bool is_waterlogged(registry::BlockStateId state) const noexcept {
        const registry::BlockId block = of(state);
        if (block.value() >= waterlogged.size() || waterlogged[block.value()].stride == 0) {
            return false;
        }
        return blocks->property_value(state, waterlogged[block.value()]) == "true";
    }

    [[nodiscard]] FireOdds odds_of(registry::BlockStateId state) const noexcept {
        const registry::BlockId block = of(state);
        if (block.value() >= odds.size()) {
            return {};
        }
        const FireOdds found = odds[block.value()];
        if ((found.ignite == 0 && found.burn == 0) || is_waterlogged(state)) {
            return {};
        }
        return found;
    }

    [[nodiscard]] bool can_burn(registry::BlockStateId state) const noexcept {
        return odds_of(state).ignite > 0;
    }

    [[nodiscard]] bool is_air(registry::BlockStateId state) const noexcept {
        return blocks->is_air(of(state));
    }

    [[nodiscard]] bool sturdy_top(registry::BlockStateId state) const noexcept {
        return blocks->face_is_sturdy(state, registry::BlockRegistry::Face::Up);
    }

    [[nodiscard]] bool valid_location(const world::LevelView& level, BlockPos pos) const {
        for (u8 i = 0; i < kDirectionCount; ++i) {
            if (can_burn(level.block_at(pos.offset(dir(i))))) {
                return true;
            }
        }
        return false;
    }

    /// `getIgniteOdds`: an empty cell's strongest neighbour, else zero.
    [[nodiscard]] i32 ignite_odds_at(const world::LevelView& level, BlockPos pos) const {
        if (!is_air(level.block_at(pos))) {
            return 0;
        }
        i32 best = 0;
        for (u8 i = 0; i < kDirectionCount; ++i) {
            best = std::max<i32>(best, odds_of(level.block_at(pos.offset(dir(i)))).ignite);
        }
        return best;
    }

    [[nodiscard]] registry::BlockStateId set_bool(registry::BlockStateId state,
                                                  const registry::PropertyView& property,
                                                  bool value) const noexcept {
        if (property.stride == 0) {
            return state;
        }
        for (u16 i = 0; i < property.values.size(); ++i) {
            if (property.values[i] == (value ? "true" : "false")) {
                return blocks->with_property(state, property, i);
            }
        }
        return state;
    }

    [[nodiscard]] registry::BlockStateId with_age(registry::BlockStateId state,
                                                  i32 value) const noexcept {
        if (age.stride == 0) {
            return state;
        }
        const std::string text = std::to_string(std::clamp(value, 0, 15));
        for (u16 i = 0; i < age.values.size(); ++i) {
            if (age.values[i] == text) {
                return blocks->with_property(state, age, i);
            }
        }
        return state;
    }

    [[nodiscard]] i32 age_of(registry::BlockStateId state) const noexcept {
        if (of(state) != fire || age.stride == 0) {
            return -1;
        }
        const std::string_view text = blocks->property_value(state, age);
        i32 value = 0;
        for (const char c : text) {
            value = value * 10 + (c - '0');
        }
        return value;
    }

    [[nodiscard]] registry::BlockStateId state_for(const world::LevelView& level,
                                                   BlockPos               pos) const noexcept {
        const registry::BlockStateId below = level.block_at(pos.offset(Direction::Down));
        if (flag(soul_base, of(below))) {
            return soul_fire_default;
        }
        if (can_burn(below) || sturdy_top(below)) {
            return fire_default;
        }
        registry::BlockStateId out = fire_default;
        out = set_bool(out, north, can_burn(level.block_at(pos.offset(Direction::North))));
        out = set_bool(out, east, can_burn(level.block_at(pos.offset(Direction::East))));
        out = set_bool(out, south, can_burn(level.block_at(pos.offset(Direction::South))));
        out = set_bool(out, west, can_burn(level.block_at(pos.offset(Direction::West))));
        out = set_bool(out, up, can_burn(level.block_at(pos.offset(Direction::Up))));
        return out;
    }

    [[nodiscard]] registry::BlockStateId state_with_age(const world::LevelView& level,
                                                        BlockPos pos, i32 value) const noexcept {
        const registry::BlockStateId state = state_for(level, pos);
        return of(state) == fire ? with_age(state, value) : state;
    }

    [[nodiscard]] bool survives(const world::LevelView& level, BlockPos pos,
                                registry::BlockStateId state) const noexcept {
        const registry::BlockStateId below = level.block_at(pos.offset(Direction::Down));
        if (of(state) == soul_fire) {
            return flag(soul_base, of(below));
        }
        return sturdy_top(below) || valid_location(level, pos);
    }

    [[nodiscard]] bool raining_near(const FireEnvironment& env, BlockPos pos) const {
        return env.is_raining_at(pos) || env.is_raining_at(pos.offset(Direction::West)) ||
               env.is_raining_at(pos.offset(Direction::East)) ||
               env.is_raining_at(pos.offset(Direction::North)) ||
               env.is_raining_at(pos.offset(Direction::South));
    }
};

// ── Construction ────────────────────────────────────────────────────────────

FireRules::FireRules(const registry::BlockRegistry& blocks, const registry::Registries& registries)
    : impl_{new Impl} {
    Impl& in      = *impl_;
    in.blocks     = &blocks;
    in.registries = &registries;

    const usize count = blocks.block_count();
    in.odds.assign(count, FireOdds{});
    in.lava.assign(count, 0);
    in.soul_base.assign(count, 0);
    for (auto& set : in.infiniburn) {
        set.assign(count, 0);
    }
    in.waterlogged.assign(count, registry::PropertyView{});
    for (usize i = 0; i < count; ++i) {
        const registry::BlockId block{static_cast<u16>(i)};
        if (const auto property = blocks.find_property(block, "waterlogged")) {
            in.waterlogged[i] = *property;
        }
    }

    for (const auto& [name, row] : table_rows()) {
        const auto block = blocks.find_block(name);
        if (!block) {
            ++in.unknown;
            continue;
        }
        in.odds[block->value()] = FireOdds{row.ignite, row.burn};
        in.lava[block->value()] = row.lava ? 1 : 0;
    }
    for (const std::string_view name : kLavaOnly) {
        if (const auto block = blocks.find_block(name)) {
            in.lava[block->value()] = 1;
        } else {
            ++in.unknown;
        }
    }

    // Tags, through the registry's wire ids, as the plant rules read theirs.
    if (const auto block_registry = registries.find("minecraft:block")) {
        const auto fill = [&](std::string_view tag, std::vector<u8>& into) {
            const auto id = registries.find_tag(*block_registry, tag);
            if (!id) {
                return;
            }
            for (usize i = 0; i < count; ++i) {
                const auto wire = registries.protocol_id(
                    *block_registry, blocks.block_name(registry::BlockId{static_cast<u16>(i)}));
                if (wire && registries.tag_contains(*id, *wire)) {
                    into[i] = 1;
                }
            }
        };
        fill("minecraft:infiniburn_overworld", in.infiniburn[0]);
        fill("minecraft:infiniburn_nether", in.infiniburn[1]);
        fill("minecraft:infiniburn_end", in.infiniburn[2]);
        fill("minecraft:soul_fire_base_blocks", in.soul_base);
    }

    const auto find = [&](std::string_view name) {
        const auto id = blocks.find_block(name);
        return id ? *id : kNoBlock;
    };
    in.fire          = find("minecraft:fire");
    in.soul_fire     = find("minecraft:soul_fire");
    in.lava_block    = find("minecraft:lava");
    in.tnt           = find("minecraft:tnt");
    in.campfire      = find("minecraft:campfire");
    in.soul_campfire = find("minecraft:soul_campfire");
    if (in.fire != kNoBlock) {
        in.fire_default = blocks.default_state(in.fire);
        const auto prop = [&](std::string_view name) {
            const auto found = blocks.find_property(in.fire, name);
            return found ? *found : registry::PropertyView{};
        };
        in.age   = prop("age");
        in.north = prop("north");
        in.east  = prop("east");
        in.south = prop("south");
        in.west  = prop("west");
        in.up    = prop("up");
    }
    if (in.soul_fire != kNoBlock) {
        in.soul_fire_default = blocks.default_state(in.soul_fire);
    }
}

FireRules::~FireRules() { delete impl_; }

FireRules::FireRules(FireRules&& other) noexcept : impl_{std::exchange(other.impl_, nullptr)} {}

FireRules& FireRules::operator=(FireRules&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

i64 FireRules::tick_delay(FireRandom& random) noexcept {
    return kTickBase + random.next_int(kTickSpread);
}

FireOdds FireRules::odds(registry::BlockId block) const noexcept {
    return block.value() < impl_->odds.size() ? impl_->odds[block.value()] : FireOdds{};
}

FireOdds FireRules::odds_of(registry::BlockStateId state) const noexcept {
    return impl_->odds_of(state);
}

bool FireRules::can_burn(registry::BlockStateId state) const noexcept {
    return impl_->can_burn(state);
}

bool FireRules::ignited_by_lava(registry::BlockStateId state) const noexcept {
    return impl_->flag(impl_->lava, impl_->of(state));
}

bool FireRules::is_fire(registry::BlockStateId state) const noexcept {
    return impl_->of(state) == impl_->fire;
}

bool FireRules::is_soul_fire(registry::BlockStateId state) const noexcept {
    return impl_->of(state) == impl_->soul_fire;
}

bool FireRules::is_lava(registry::BlockStateId state) const noexcept {
    return impl_->of(state) == impl_->lava_block;
}

bool FireRules::is_campfire(registry::BlockStateId state) const noexcept {
    return impl_->of(state) == impl_->campfire;
}

bool FireRules::is_soul_campfire(registry::BlockStateId state) const noexcept {
    return impl_->of(state) == impl_->soul_campfire;
}

i32 FireRules::age_of(registry::BlockStateId state) const noexcept { return impl_->age_of(state); }

usize FireRules::unknown_names() const noexcept { return impl_->unknown; }

registry::BlockStateId FireRules::state_for(const world::LevelView& level,
                                            BlockPos               pos) const noexcept {
    return impl_->state_for(level, pos);
}

registry::BlockStateId FireRules::state_with_age(const world::LevelView& level, BlockPos pos,
                                                 i32 age) const noexcept {
    return impl_->state_with_age(level, pos, age);
}

bool FireRules::survives(const world::LevelView& level, BlockPos pos,
                         registry::BlockStateId fire) const noexcept {
    return impl_->survives(level, pos, fire);
}

std::optional<registry::BlockStateId> FireRules::placement(const world::LevelView& level,
                                                           BlockPos pos) const noexcept {
    if (impl_->fire == kNoBlock || !impl_->is_air(level.block_at(pos))) {
        return std::nullopt;
    }
    const registry::BlockStateId state = impl_->state_for(level, pos);
    if (!impl_->survives(level, pos, state)) {
        return std::nullopt;
    }
    return state;
}

bool FireRules::ignite(world::LevelWriter& level, BlockPos pos) const {
    const auto state = placement(level, pos);
    if (!state) {
        return false;
    }
    level.set_block(pos, *state);
    return true;
}

void FireRules::neighbour_changed(world::LevelWriter& level, BlockPos pos,
                                  FireRandom& random) const {
    const Impl&                  in    = *impl_;
    const registry::BlockStateId state = level.block_at(pos);
    const registry::BlockId      block = in.of(state);
    if (block != in.fire && block != in.soul_fire) {
        return;
    }
    if (!in.survives(level, pos, state)) {
        level.set_block(pos, in.air);
        return;
    }
    // `updateShape`: a fire re-derives its face flags — and becomes soul fire
    // when soul soil appears under it. Soul fire has nothing to recompute.
    registry::BlockStateId now = state;
    if (block == in.fire) {
        now = in.state_with_age(level, pos, in.age_of(state));
        if (now != state) {
            level.set_block(pos, now);
        }
    }
    // `onPlace`: the first tick, for a fire any path wrote. Asked once — the
    // tick reschedules itself from then on.
    if (in.of(now) == in.fire &&
        !level.has_scheduled_tick(pos, "minecraft:fire", world::TickQueue::Block)) {
        level.schedule_tick(pos, "minecraft:fire", tick_delay(random), world::TickQueue::Block,
                            world::TickPriority::Normal);
    }
}

bool FireRules::scheduled_tick(world::LevelWriter& level, FireEnvironment& env, BlockPos pos,
                               FireRandom& random, FireTickResult* result) const {
    const Impl&                  in    = *impl_;
    const registry::BlockStateId state = level.block_at(pos);
    if (in.of(state) != in.fire) {
        return false;
    }
    FireTickResult scratch;
    FireTickResult& out = result != nullptr ? *result : scratch;

    level.schedule_tick(pos, "minecraft:fire", tick_delay(random), world::TickQueue::Block,
                        world::TickPriority::Normal);
    if (!env.fire_tick()) {
        return true;
    }
    if (!in.survives(level, pos, state)) {
        level.set_block(pos, in.air);
        out.extinguished = true;
        return true;
    }

    const BlockPos               below_pos  = pos.offset(Direction::Down);
    const registry::BlockStateId below      = level.block_at(below_pos);
    const auto                   dimension  = static_cast<usize>(env.dimension());
    const bool                   infiniburn = in.flag(in.infiniburn[dimension], in.of(below));
    const i32                    age        = in.age_of(state);

    if (!infiniburn && env.is_raining() && in.raining_near(env, pos)) {
        const f32 roll = random.next_float();
        if (roll < 0.2F + static_cast<f32>(age) * 0.03F) {
            level.set_block(pos, in.air);
            out.extinguished = true;
            out.rained_out   = true;
            return true;
        }
    }

    const i32 step    = random.next_int(3) / 2;
    const i32 new_age = std::min(15, age + step);
    if (new_age != age) {
        level.set_block(pos, in.with_age(state, new_age));
        out.aged = true;
    }

    if (!infiniburn) {
        if (!in.valid_location(level, pos)) {
            // On the age the tick began with, not the one it just grew to —
            // the wiki does not say which; the `blocks` campaign's age logs of
            // fires on stone (is age 4 ever seen before death?) decide it.
            if (!in.sturdy_top(below) || age > 3) {
                level.set_block(pos, in.air);
                out.extinguished = true;
            }
            return true;
        }
        if (age == 15) {
            const i32 roll = random.next_int(4);
            if (roll == 0 && !in.can_burn(below)) {
                level.set_block(pos, in.air);
                out.extinguished = true;
                return true;
            }
        }
    }

    const bool humid = env.increased_burnout(pos);
    const i32  k     = humid ? -50 : 0;

    const auto burn_out = [&](BlockPos at, i32 chance) {
        const registry::BlockStateId target = level.block_at(at);
        const i32                    odds   = in.odds_of(target).burn;
        const i32                    roll   = random.next_int(chance);
        if (roll >= odds) {
            return;
        }
        const bool was_tnt = in.of(target) == in.tnt;
        const i32  keep    = random.next_int(age + 10);
        if (keep < 5 && !env.is_raining_at(at)) {
            const i32 grown = std::min(age + random.next_int(5) / 4, 15);
            level.set_block(at, in.state_with_age(level, at, grown));
        } else {
            level.set_block(at, in.air);
        }
        ++out.burnt;
        if (was_tnt) {
            env.prime_tnt(at);
        }
    };
    burn_out(pos.offset(Direction::East), 300 + k);
    burn_out(pos.offset(Direction::West), 300 + k);
    burn_out(pos.offset(Direction::Down), 250 + k);
    burn_out(pos.offset(Direction::Up), 250 + k);
    burn_out(pos.offset(Direction::North), 300 + k);
    burn_out(pos.offset(Direction::South), 300 + k);

    const bool raining   = env.is_raining();
    const i32  difficulty = env.difficulty();
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dy = -1; dy <= 4; ++dy) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                const i32      reach = dy > 1 ? 100 + (dy - 1) * 100 : 100;
                const BlockPos cell  = pos.offset(dx, dy, dz);
                const i32      odds  = in.ignite_odds_at(level, cell);
                if (odds <= 0) {
                    continue;
                }
                i32 chance = (odds + 40 + difficulty * 7) / (age + 30);
                if (humid) {
                    chance /= 2;
                }
                if (chance <= 0) {
                    continue;
                }
                const i32 roll = random.next_int(reach);
                if (roll > chance || (raining && in.raining_near(env, cell))) {
                    continue;
                }
                const i32 grown = std::min(15, age + random.next_int(5) / 4);
                level.set_block(cell, in.state_with_age(level, cell, grown));
                ++out.spread;
            }
        }
    }
    return true;
}

i32 FireRules::lava_random_tick(world::LevelWriter& level, FireEnvironment& env, BlockPos pos,
                                FireRandom& random) const {
    const Impl& in = *impl_;
    if (!env.fire_tick()) {
        return 0;
    }
    const auto lava_lights = [&](BlockPos at) {
        return in.flag(in.lava, in.of(level.block_at(at)));
    };
    const i32 steps = random.next_int(3);
    if (steps > 0) {
        BlockPos at = pos;
        for (i32 j = 0; j < steps; ++j) {
            const i32 dx = random.next_int(3) - 1;
            const i32 dz = random.next_int(3) - 1;
            at           = at.offset(dx, 1, dz);
            if (!level.is_loaded(at)) {
                return 0;
            }
            const registry::BlockStateId here = level.block_at(at);
            if (in.is_air(here)) {
                for (u8 i = 0; i < kDirectionCount; ++i) {
                    if (lava_lights(at.offset(dir(i)))) {
                        level.set_block(at, in.state_for(level, at));
                        return 1;
                    }
                }
            } else if (level.blocks().blocks_motion(in.of(here))) {
                return 0;
            }
        }
        return 0;
    }
    i32 lit = 0;
    for (i32 k = 0; k < 3; ++k) {
        const i32      dx = random.next_int(3) - 1;
        const i32      dz = random.next_int(3) - 1;
        const BlockPos at = pos.offset(dx, 0, dz);
        if (!level.is_loaded(at)) {
            return lit;
        }
        const BlockPos above = at.offset(Direction::Up);
        if (in.is_air(level.block_at(above)) && lava_lights(at)) {
            // Vanilla asks for the fire's shape at the burning block, not at
            // the cell it lights; the neighbour notification the write makes
            // corrects the face flags straight away.
            level.set_block(above, in.state_for(level, at));
            ++lit;
        }
    }
    return lit;
}

// ── Things on fire ──────────────────────────────────────────────────────────

void set_on_fire(EntityFire& fire, i32 seconds) noexcept {
    const i32 ticks = seconds * 20;
    if (fire.remaining < ticks) {
        fire.remaining = ticks;
    }
}

FireDamage tick_entity_fire(EntityFire& fire, const FireContact& contact,
                            const EntityFireConstants& constants) noexcept {
    FireDamage out;
    if (contact.in_water) {
        fire.remaining = 0;
    }
    if (fire.remaining > 0) {
        if (fire.fire_immune) {
            fire.remaining = std::max(0, fire.remaining - 4);
        } else {
            if (fire.remaining % constants.on_fire_interval == 0 && !contact.in_lava) {
                out.on_fire = constants.on_fire_damage;
            }
            --fire.remaining;
        }
    }
    if (contact.in_lava && !fire.fire_immune) {
        set_on_fire(fire, constants.lava_seconds);
        out.lava = constants.lava_damage;
    }
    const bool hot = contact.in_fire || contact.in_soul_fire || contact.in_lava;
    if ((contact.in_fire || contact.in_soul_fire) && !fire.fire_immune) {
        ++fire.remaining;
        if (fire.remaining == 0) {
            set_on_fire(fire, constants.fire_block_seconds);
        }
        out.in_fire = contact.in_soul_fire ? constants.soul_fire_damage : constants.fire_damage;
    }
    if (contact.campfire != 0 && !fire.fire_immune) {
        const f32 hit =
            contact.campfire == 2 ? constants.soul_campfire_damage : constants.campfire_damage;
        out.in_fire = std::max(out.in_fire, hit);
    }
    if (!hot && fire.remaining <= 0) {
        fire.remaining = -fire.immune_ticks;
    }
    if (fire.on_fire() && contact.wet) {
        fire.remaining = -fire.immune_ticks;
    }
    return out;
}

bool fire_resistance_blocks(DamageKind kind) noexcept {
    return has(damage_type(kind).flags, DamageFlags::IsFire);
}

f32 light_magic_value(i32 light) noexcept {
    const f32 f = static_cast<f32>(std::clamp(light, 0, 15)) / 15.0F;
    return f / (4.0F - 3.0F * f);
}

bool sun_burns(i32 light, i32 sky_darken, bool sees_sky, bool wet, FireRandom& random) noexcept {
    if (sky_darken >= 4) {
        return false;
    }
    const f32 value = light_magic_value(light);
    if (value <= 0.5F) {
        return false;
    }
    const f32 roll = random.next_float() * 30.0F;
    return roll < (value - 0.4F) * 2.0F && !wet && sees_sky;
}

u8 tick_campfire(std::span<CampfireSlot, 4> slots, bool lit) noexcept {
    u8 finished = 0;
    for (usize i = 0; i < slots.size(); ++i) {
        CampfireSlot& slot = slots[i];
        if (slot.item < 0) {
            continue;
        }
        if (lit) {
            ++slot.progress;
            if (slot.progress >= slot.total) {
                finished = static_cast<u8>(finished | (1U << i));
            }
        } else if (slot.progress > 0) {
            slot.progress = std::clamp(slot.progress - 2, 0, slot.total);
        }
    }
    return finished;
}

}  // namespace ov::gameplay
