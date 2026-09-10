// The random tick, against the real game's numbers.
//
// The growth rate of a crop is statistical, so what is compared is a
// **distribution**: the same tile layouts the oracle campaign built on a real
// 1.20.1 server (scripts/measure_agriculture.py growth), the same speed times
// ticks, and the histogram of ages at the end. The picking runs through
// `RandomTicks::tick_section` over real `ChunkSection`s — the code the server
// runs — and the rules through `gameplay::Plants`.
//
// Two comparisons per layout, as docs/provenance/agriculture.md reports them:
// our histogram against the game's (a χ² test of homogeneity), and our
// histogram against the same model with a **wrong** growth chance, which must
// be rejected — the control that shows the test can tell anything apart.
//
// Then the path the server takes for leaves: `ServerLevel` and `WorldTicks`,
// the drain and its notifications, a log removed and the leaves falling.
#include "../src/agriculture.hpp"
#include "../src/world_ticks.hpp"

#include "ov/world/chunk_section.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Packs out;
        auto  blocks = registry::BlockRegistry::load(path);
        auto  regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] const registry::BlockRegistry& blocks() { return *packs().blocks; }

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view name, std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks().default_state(*id);
    for (const auto& [key, value] : props) {
        const auto property = blocks().find_property(*id, key);
        REQUIRE(property.has_value());
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks().with_property(state, *property, static_cast<u16>(i));
            }
        }
    }
    return state;
}

[[nodiscard]] i32 age_of(registry::BlockStateId state) {
    const auto p = blocks().find_property(blocks().block_of(state), "age");
    return p ? std::stoi(std::string{blocks().property_value(state, *p)}) : -1;
}

/// A level made of real sections, so the driver reads what the rules write.
class SectionLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = sections_.find(key(pos));
        if (found == sections_.end()) {
            return registry::kAirState;
        }
        return found->second->get_block(static_cast<usize>(floor_mod(pos.x, 16)),
                                         static_cast<usize>(floor_mod(pos.y, 16)),
                                         static_cast<usize>(floor_mod(pos.z, 16)));
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return {}; }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        auto& section = sections_[key(pos)];
        if (!section) {
            section = std::make_unique<world::ChunkSection>(world::AirStates::from(::blocks()));
        }
        section->set_block(static_cast<usize>(floor_mod(pos.x, 16)),
                           static_cast<usize>(floor_mod(pos.y, 16)),
                           static_cast<usize>(floor_mod(pos.z, 16)), state);
    }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

    /// One tick: `speed` picks in every section, in a fixed order.
    void tick(RandomTicks& ticks, const gameplay::Plants& plants, gameplay::PlantEnvironment& env) {
        RandomTickStats stats;
        for (auto& [k, section] : sections_) {
            const BlockPos origin{std::get<0>(k) * 16, std::get<1>(k) * 16, std::get<2>(k) * 16};
            ticks.tick_section(*this, *section, origin, plants, env, stats);
        }
    }

private:
    using Key = std::tuple<i32, i32, i32>;
    [[nodiscard]] static Key key(BlockPos p) {
        return {floor_div(p.x, 16), floor_div(p.y, 16), floor_div(p.z, 16)};
    }
    std::map<Key, std::unique_ptr<world::ChunkSection>> sections_;
};

class Daylight final : public gameplay::PlantEnvironment {
public:
    [[nodiscard]] u8   block_light(BlockPos) const override { return 0; }
    [[nodiscard]] u8   sky_light(BlockPos) const override { return 15; }
    [[nodiscard]] u8   sky_darken() const override { return 0; }
    [[nodiscard]] bool is_raining_at(BlockPos) const override { return false; }
    void               drop_block(BlockPos, registry::BlockStateId) override { ++drops; }
    bool grow_tree(world::LevelWriter&, BlockPos, registry::BlockStateId,
                   gameplay::PlantRandom&) override {
        return false;
    }
    usize drops{0};
};

// ── The layouts of the oracle campaign ──────────────────────────────────────

struct Field {
    SectionLevel          level;
    std::vector<BlockPos> sparse_wheat;   // A
    std::vector<BlockPos> dense_wheat;    // C
    std::vector<BlockPos> sparse_beet;    // B
    std::vector<BlockPos> dry_rows;       // R
};

/// The grid scripts/measure_agriculture.py builds: nine by four wet tiles of
/// 9x9, kinds A C B by column, a fifth row of crop-less wet tiles, and eight
/// dry 12x12 tiles of alternating wheat and carrot rows.
void build(Field& f) {
    const auto farmland_wet = state_of("minecraft:farmland", {{"moisture", "7"}});
    const auto farmland_dry = state_of("minecraft:farmland", {{"moisture", "0"}});
    const auto water        = state_of("minecraft:water");
    const auto wheat        = state_of("minecraft:wheat");
    const auto beet         = state_of("minecraft:beetroots");
    const auto carrots      = state_of("minecraft:carrots");
    constexpr i32 kFarm = 0;

    for (i32 j = 0; j < 5; ++j) {
        for (i32 i = 0; i < 9; ++i) {
            const i32  ox   = -58 + i * 9 + 4;
            const i32  oz   = -58 + j * 9 + 4;
            const char kind = j == 4 ? 'X' : "ACB"[i % 3];
            for (i32 dz = -4; dz <= 4; ++dz) {
                for (i32 dx = -4; dx <= 4; ++dx) {
                    f.level.set_block({ox + dx, kFarm, oz + dz}, farmland_wet);
                }
            }
            f.level.set_block({ox, kFarm, oz}, water);
            if (kind == 'X') {
                continue;
            }
            for (i32 dz = -4; dz <= 4; ++dz) {
                for (i32 dx = -4; dx <= 4; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const bool sparse_spot = dx % 2 != 0 && dz % 2 != 0 && std::abs(dx) <= 3 &&
                                             std::abs(dz) <= 3;
                    const BlockPos at{ox + dx, kFarm + 1, oz + dz};
                    if (kind == 'C') {
                        f.level.set_block(at, wheat);
                        f.dense_wheat.push_back(at);
                    } else if (sparse_spot) {
                        f.level.set_block(at, kind == 'A' ? wheat : beet);
                        (kind == 'A' ? f.sparse_wheat : f.sparse_beet).push_back(at);
                    }
                }
            }
        }
    }
    for (i32 j = 0; j < 2; ++j) {
        for (i32 i = 0; i < 4; ++i) {
            const i32 ox = -58 + i * 14;
            const i32 oz = j * 14;
            for (i32 dz = 0; dz < 12; ++dz) {
                for (i32 dx = 0; dx < 12; ++dx) {
                    f.level.set_block({ox + dx, kFarm, oz + dz}, farmland_dry);
                    const bool row_of_wheat = dz % 2 == 0;
                    const BlockPos at{ox + dx, kFarm + 1, oz + dz};
                    f.level.set_block(at, row_of_wheat ? wheat : carrots);
                    if (row_of_wheat && dx > 0 && dx < 11 && dz > 0 && dz < 11) {
                        f.dry_rows.push_back(at);
                    }
                }
            }
        }
    }
    // Grass around the dry tiles stands in for the flat world's: the edge
    // crops' neighbours are not farmland. Not counted either way.
}

template <usize N>
void histogram(const SectionLevel& level, const std::vector<BlockPos>& cells, std::array<u64, N>& out) {
    for (const BlockPos p : cells) {
        const i32 age = age_of(level.block_at(p));
        REQUIRE(age >= 0);
        REQUIRE(age < static_cast<i32>(N));
        ++out[static_cast<usize>(age)];
    }
}

// ── χ², written out ─────────────────────────────────────────────────────────

/// Upper regularised gamma Q(a, x): the survival function of χ² with 2a
/// degrees of freedom at 2x.
[[nodiscard]] f64 gamma_q(f64 a, f64 x) {
    if (x <= 0.0) {
        return 1.0;
    }
    const f64 log_front = -x + a * std::log(x) - std::lgamma(a);
    if (x < a + 1.0) {
        f64 term = 1.0 / a;
        f64 sum  = term;
        for (i32 n = 1; n < 1000 && term > sum * 1e-14; ++n) {
            term *= x / (a + n);
            sum += term;
        }
        return 1.0 - sum * std::exp(log_front);
    }
    f64 b = x + 1.0 - a;
    f64 c = 1e300;
    f64 d = 1.0 / b;
    f64 h = d;
    for (i32 i = 1; i < 1000; ++i) {
        const f64 an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        d = std::abs(d) < 1e-300 ? 1e-300 : d;
        c = b + an / c;
        c = std::abs(c) < 1e-300 ? 1e-300 : c;
        d = 1.0 / d;
        const f64 step = d * c;
        h *= step;
        if (std::abs(step - 1.0) < 1e-14) {
            break;
        }
    }
    return std::exp(log_front) * h;
}

/// χ² of homogeneity between two histograms, adjacent bins merged from the top
/// until every expected count is at least five. Returns the p-value.
template <usize N>
[[nodiscard]] f64 homogeneity(const std::array<u64, N>& ours, const std::array<f64, N>& theirs) {
    const f64 n1 = [&] { f64 s = 0; for (auto v : ours) s += static_cast<f64>(v); return s; }();
    const f64 n2 = [&] { f64 s = 0; for (auto v : theirs) s += v; return s; }();
    std::vector<std::pair<f64, f64>> bins;
    std::pair<f64, f64>              acc{0.0, 0.0};
    for (usize i = 0; i < N; ++i) {
        acc.first += static_cast<f64>(ours[i]);
        acc.second += theirs[i];
        const f64 col = acc.first + acc.second;
        if (col * std::min(n1, n2) / (n1 + n2) >= 5.0) {
            bins.push_back(acc);
            acc = {0.0, 0.0};
        }
    }
    if (acc.first + acc.second > 0.0) {
        if (bins.empty()) {
            bins.push_back(acc);
        } else {
            bins.back().first += acc.first;
            bins.back().second += acc.second;
        }
    }
    f64 stat = 0.0;
    for (const auto& [a, b] : bins) {
        const f64 col = a + b;
        const f64 e1  = col * n1 / (n1 + n2);
        const f64 e2  = col * n2 / (n1 + n2);
        stat += (a - e1) * (a - e1) / e1 + (b - e2) * (b - e2) / e2;
    }
    const f64 df = static_cast<f64>(bins.size()) - 1.0;
    return df < 1.0 ? 1.0 : gamma_q(df / 2.0, stat / 2.0);
}

/// min(cap, Binomial(n, q)) as a distribution, scaled to a total.
template <usize N>
[[nodiscard]] std::array<f64, N> capped_binomial(f64 n, f64 q, f64 total) {
    std::array<f64, N> out{};
    f64                sum = 0.0;
    for (usize k = 0; k + 1 < N; ++k) {
        const f64 kk  = static_cast<f64>(k);
        const f64 log = std::lgamma(n + 1) - std::lgamma(kk + 1) - std::lgamma(n - kk + 1) +
                        kk * std::log(q) + (n - kk) * std::log1p(-q);
        out[k] = std::exp(log);
        sum += out[k];
    }
    out[N - 1] = 1.0 - sum;
    for (auto& v : out) {
        v *= total;
    }
    return out;
}

// The real game, both campaigns summed (K·T = 60 600 and 60 400). From
// docs/provenance/agriculture.md § croissance — measured, not modelled.
constexpr std::array<f64, 8> kVanillaSparseWheat{5, 14, 26, 60, 60, 72, 55, 92};
constexpr std::array<f64, 8> kVanillaDenseWheat{169, 425, 503, 394, 231, 119, 40, 30};
constexpr std::array<f64, 8> kVanillaDryRows{93, 195, 223, 160, 90, 26, 8, 5};
constexpr std::array<f64, 4> kVanillaBeetroot{13, 50, 78, 243};

}  // namespace

TEST_CASE("crops grow at the real game's rate, by distribution", "[server][agriculture]") {
    REQUIRE(packs().blocks.has_value());
    const gameplay::Plants plants{*packs().blocks, *packs().registries};

    std::array<u64, 8> sparse{};
    std::array<u64, 8> dense{};
    std::array<u64, 8> rows{};
    std::array<u64, 4> beet{};

    // Five independent fields, 100 picks per section for 605 ticks: the same
    // K·T as the oracle's runs, and five times its sample.
    for (u64 seed = 1; seed <= 5; ++seed) {
        Field f;
        build(f);
        RandomTicks ticks{seed};
        ticks.set_speed(100);
        Daylight env;
        for (i32 t = 0; t < 605; ++t) {
            f.level.tick(ticks, plants, env);
        }
        histogram(f.level, f.sparse_wheat, sparse);
        histogram(f.level, f.dense_wheat, dense);
        histogram(f.level, f.dry_rows, rows);
        histogram(f.level, f.sparse_beet, beet);
        REQUIRE(env.drops == 0);
    }

    // Against the real game.
    const f64 p_sparse = homogeneity(sparse, kVanillaSparseWheat);
    const f64 p_dense  = homogeneity(dense, kVanillaDenseWheat);
    const f64 p_rows   = homogeneity(rows, kVanillaDryRows);
    const f64 p_beet   = homogeneity(beet, kVanillaBeetroot);
    // WARN, not INFO: the p-values are the result, and they are printed on
    // every run rather than only when something fails.
    WARN("homogeneity with vanilla: sparse " << p_sparse << " dense " << p_dense << " rows "
                                             << p_rows << " beetroot " << p_beet);
    REQUIRE(p_sparse > 0.001);
    REQUIRE(p_dense > 0.001);
    REQUIRE(p_rows > 0.001);
    REQUIRE(p_beet > 0.001);

    // The control: the same histograms against a wrong chance must fail. If
    // they did not, the four passes above would mean nothing.
    const f64 kt = 100.0 * 605.0;
    const auto total = [](const auto& h) {
        f64 s = 0;
        for (auto v : h) s += static_cast<f64>(v);
        return s;
    };
    const f64 wrong_sparse =
        homogeneity(sparse, capped_binomial<8>(kt, 1.0 / 6.0 / 4096.0, total(sparse) * 100.0));
    const f64 wrong_dense =
        homogeneity(dense, capped_binomial<8>(kt, 1.0 / 3.0 / 4096.0, total(dense) * 100.0));
    const f64 wrong_beet =
        homogeneity(beet, capped_binomial<4>(kt, 1.0 / 3.0 / 4096.0, total(beet) * 100.0));
    WARN("controls: " << wrong_sparse << " " << wrong_dense << " " << wrong_beet);
    REQUIRE(wrong_sparse < 1e-6);
    REQUIRE(wrong_dense < 1e-6);
    REQUIRE(wrong_beet < 1e-6);
}

TEST_CASE("randomTickSpeed zero grows nothing, and the picks follow the speed",
          "[server][agriculture]") {
    const gameplay::Plants plants{*packs().blocks, *packs().registries};
    SectionLevel           level;
    level.set_block({0, 0, 0}, state_of("minecraft:farmland", {{"moisture", "7"}}));
    level.set_block({1, 0, 0}, state_of("minecraft:water"));
    level.set_block({0, 1, 0}, state_of("minecraft:wheat"));
    Daylight    env;
    RandomTicks ticks{42};
    ticks.set_speed(0);
    for (i32 t = 0; t < 10000; ++t) {
        level.tick(ticks, plants, env);
    }
    REQUIRE(age_of(level.block_at({0, 1, 0})) == 0);

    ticks.set_speed(-5);
    REQUIRE(ticks.speed() == 0);
    ticks.set_speed(3);
    REQUIRE(ticks.speed() == 3);
}

namespace {

/// A server level over a map, with the real drain behind it.
struct Rig {
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells;
    std::optional<ServerLevel>                                  level;
    std::optional<WorldTicks>                                   ticks;
    gameplay::Plants plants{*packs().blocks, *packs().registries};
    Daylight         env;

    Rig() {
        LevelHooks hooks;
        hooks.block_at = [this](BlockPos p) {
            const auto found = cells.find({p.x, p.y, p.z});
            return found == cells.end() ? registry::kAirState : found->second;
        };
        hooks.is_loaded = [](BlockPos) { return true; };
        hooks.set_block = [this](BlockPos p, registry::BlockStateId s) { cells[{p.x, p.y, p.z}] = s; };
        level.emplace(*packs().blocks, std::move(hooks));
        ticks.emplace(*packs().blocks, *packs().registries);
        ticks->attach_plants(plants, env);
    }

    /// A player's edit: written, then notified around, as the server does.
    void place(BlockPos p, registry::BlockStateId s) {
        cells[{p.x, p.y, p.z}] = s;
        (void)ticks->notify(*level, p);
    }

    void run(i64 from, i64 to) {
        for (i64 t = from; t < to; ++t) {
            (void)ticks->run(*level, t);
        }
    }
};

}  // namespace

TEST_CASE("leaves learn their distance through the drain and fall when the log goes",
          "[server][agriculture]") {
    Rig rig;
    rig.level->set_game_time(0);
    rig.place({0, 0, 0}, state_of("minecraft:oak_log"));
    // Placed as a worldgen tree would leave them: every leaf at 7.
    for (i32 i = 1; i <= 8; ++i) {
        rig.place({i, 0, 0}, state_of("minecraft:oak_leaves", {{"persistent", "false"}}));
    }
    rig.run(1, 20);
    for (i32 i = 1; i <= 8; ++i) {
        const auto leaf = rig.cells[{i, 0, 0}];
        const auto p    = blocks().find_property(blocks().block_of(leaf), "distance");
        REQUIRE(std::string{blocks().property_value(leaf, *p)} == std::to_string(std::min(i, 7)));
    }

    // The log goes; the distance climbs to 7 one tick per block.
    rig.place({0, 0, 0}, registry::kAirState);
    rig.run(20, 40);
    for (i32 i = 1; i <= 8; ++i) {
        REQUIRE(rig.plants.ticks_randomly(rig.cells[{i, 0, 0}]));
    }
    gameplay::PlantRandom random{3};
    for (i32 i = 1; i <= 8; ++i) {
        rig.plants.random_tick(*rig.level, rig.env, {i, 0, 0}, rig.cells[{i, 0, 0}], random);
    }
    for (i32 i = 1; i <= 8; ++i) {
        REQUIRE(rig.cells[{i, 0, 0}] == registry::kAirState);
    }
    REQUIRE(rig.env.drops == 8);
}

TEST_CASE("a crop falls when its farmland is dug out, through the drain", "[server][agriculture]") {
    Rig rig;
    rig.place({0, 0, 0}, state_of("minecraft:farmland"));
    rig.place({0, 1, 0}, state_of("minecraft:wheat", {{"age", "7"}}));
    REQUIRE(blocks().block_of(rig.cells[{0, 1, 0}]) == *blocks().find_block("minecraft:wheat"));
    rig.place({0, 0, 0}, state_of("minecraft:dirt"));
    REQUIRE(rig.cells[{0, 1, 0}] == registry::kAirState);
    REQUIRE(rig.env.drops == 1);

    // Farmland under a placed stone turns to dirt a tick later.
    rig.place({5, 0, 0}, state_of("minecraft:farmland"));
    rig.place({5, 1, 0}, state_of("minecraft:stone"));
    rig.run(0, 3);
    REQUIRE(blocks().block_of(rig.cells[{5, 0, 0}]) == *blocks().find_block("minecraft:dirt"));
}
