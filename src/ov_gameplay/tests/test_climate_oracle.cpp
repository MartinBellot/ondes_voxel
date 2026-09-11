// The climate against the real game's world generation.
//
// `freeze_top_layer`, the game's last feature step, puts ice on cold water and
// snow on cold ground at the top of every column — the same two rules the
// weather's chunk tick uses, without the "edge only" condition. A world the
// real 1.20.1 server generated is therefore a map of where its temperature is
// under 0.15, and `scripts/measure_climate.py` extracts that map from
// run/reference-1234567890. This test replays every extracted column through
// `ClimateNoise` and `PrecipitationRules` — the code the server and the client
// run — and prints how many agree.
//
// It is skipped unless OV_CLIMATE_ORACLE names the extracted file: the world
// is a local, gitignored artefact of the real game.
//
// The witness (briefing, trap 14): the same columns with the noise sampled ten
// thousand blocks away. A frozen ocean's patches and a mountain's snow line are
// only reproduced by the right noise; the witness must do markedly worse, or
// the agreement measures nothing.
#include "ov/gameplay/weather.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
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

/// "minecraft:water[level=0]" to a state, or nothing.
[[nodiscard]] std::optional<registry::BlockStateId> parse_state(const registry::BlockRegistry& blocks,
                                                                const std::string&             text) {
    const auto open = text.find('[');
    const auto name = text.substr(0, open);
    const auto id   = blocks.find_block(name);
    if (!id) {
        return std::nullopt;
    }
    if (open == std::string::npos) {
        return blocks.default_state(*id);
    }
    std::vector<std::pair<std::string, std::string>> owned;
    std::string body = text.substr(open + 1, text.size() - open - 2);
    std::stringstream stream{body};
    std::string pair;
    while (std::getline(stream, pair, ',')) {
        const auto eq = pair.find('=');
        owned.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
    }
    std::vector<std::pair<std::string_view, std::string_view>> views;
    for (const auto& [k, v] : owned) {
        views.emplace_back(k, v);
    }
    return blocks.state_for(*id, views);
}

/// Two cells and air.
class TwoCells final : public world::LevelView {
public:
    explicit TwoCells(const registry::BlockRegistry& blocks) : blocks_{&blocks} {}
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        if (pos == below_pos) return below;
        if (pos == top_pos) return top;
        // Around the water the world generation pass does not look; around
        // the ground, snow only reads the block under it.
        return registry::kAirState;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }

    BlockPos               below_pos{};
    BlockPos               top_pos{};
    registry::BlockStateId below{};
    registry::BlockStateId top{};

private:
    const registry::BlockRegistry* blocks_;
};

struct Tally {
    usize agree{0};
    usize total{0};
    usize game_yes_ours_no{0};
    usize game_no_ours_yes{0};
    void add(bool game, bool ours) {
        ++total;
        agree += game == ours ? 1 : 0;
        game_yes_ours_no += game && !ours ? 1 : 0;
        game_no_ours_yes += !game && ours ? 1 : 0;
    }
    [[nodiscard]] f64 rate() const { return total == 0 ? 1.0 : static_cast<f64>(agree) / static_cast<f64>(total); }
    [[nodiscard]] std::string line() const {
        std::ostringstream o;
        o << agree << "/" << total << " (" << rate() * 100.0 << "%), game only " << game_yes_ours_no
          << ", ours only " << game_no_ours_yes;
        return o.str();
    }
};

}  // namespace

TEST_CASE("the climate agrees with the columns the real game froze and snowed") {
    const char* path = std::getenv("OV_CLIMATE_ORACLE");
    if (path == nullptr || !packs().blocks || !packs().registries) {
        SKIP("OV_CLIMATE_ORACLE is not set (scripts/measure_climate.py writes the file)");
    }
    std::ifstream in{path};
    REQUIRE(in.good());
    const registry::BlockRegistry& blocks = *packs().blocks;
    const PrecipitationRules       rules{blocks, *packs().registries};
    const ClimateNoise             climate;
    const auto water = blocks.default_state(*blocks.find_block("minecraft:water"));

    // By kind of column, by whether the biome neighbourhood is uniform, and
    // for the witness.
    std::map<std::string, Tally> tallies;
    std::map<std::string, Tally> witness;
    std::map<std::string, std::optional<BiomeClimate>> biome_cache;
    usize unparsed = 0;
    usize samples  = 0;

    std::string line;
    while (std::getline(in, line)) {
        std::stringstream fields{line};
        i32 x = 0, z = 0, top = 0, uniform = 0, light_top = 0, light_below = 0;
        std::string block_top, block_below, biome;
        fields >> x >> z >> top >> block_top >> block_below >> biome >> uniform >> light_top >>
            light_below;
        auto& cached = biome_cache[biome];
        if (!biome_cache.contains(biome) || !cached) {
            const auto index = blocks.find_biome(biome);
            cached = index ? std::optional<BiomeClimate>{climate_of(blocks.biome(*index))} : std::nullopt;
        }
        if (!cached) {
            ++unparsed;
            continue;
        }
        const BiomeClimate bc = *cached;
        // "margin": high enough, and near enough the snow line, that the
        // eight-block height noise decides between rain and snow. Only there
        // does the height noise have anything to prove.
        const auto near_line = [&](i32 y) {
            if (bc.frozen || y <= 80) {
                return false;
            }
            const f32 lowest  = bc.temperature - (8.0F + static_cast<f32>(y) - 80.0F) * 0.05F / 40.0F;
            const f32 highest = bc.temperature - (-8.0F + static_cast<f32>(y) - 80.0F) * 0.05F / 40.0F;
            return lowest < kRainTemperature && highest >= kRainTemperature;
        };
        const std::string kind = bc.frozen            ? "frozen"
                                 : near_line(top)     ? "margin"
                                 : top - 1 > 80       ? "altitude"
                                                      : "other";
        const std::string suffix = uniform != 0 ? " uniform" : " border";

        TwoCells level{blocks};
        level.below_pos = {x, top - 1, z};
        level.top_pos   = {x, top, z};

        const bool below_is_ice   = block_below == "minecraft:ice";
        const bool below_is_water = block_below == "minecraft:water[level=0]";
        if (below_is_ice || below_is_water) {
            level.below = water;
            level.top   = registry::kAirState;
            const BlockPos at{x, top - 1, z};
            const bool ours = rules.should_freeze(level, at, climate.temperature_at(bc, at),
                                                  static_cast<u8>(light_below), false);
            tallies["ice " + kind + suffix].add(below_is_ice, ours);
            const BlockPos far{x + 10000, top - 1, z};
            const bool shifted = rules.should_freeze(level, at, climate.temperature_at(bc, far),
                                                     static_cast<u8>(light_below), false);
            witness["ice " + kind + suffix].add(below_is_ice, shifted);
            continue;
        }
        const bool top_is_snow = block_top.starts_with("minecraft:snow[");
        if (!top_is_snow && block_top != "minecraft:air") {
            continue;
        }
        const auto ground = parse_state(blocks, block_below);
        if (!ground) {
            ++unparsed;
            continue;
        }
        level.below = *ground;
        level.top   = registry::kAirState;
        const BlockPos at{x, top, z};
        const bool ours = rules.should_snow(level, at, climate.temperature_at(bc, at),
                                            static_cast<u8>(light_top));
        tallies["snow " + kind + suffix].add(top_is_snow, ours);
        if (ours != top_is_snow && uniform != 0 && samples < 24) {
            ++samples;
            WARN("snow disagreement at " << x << " " << top << " " << z << ": game "
                                         << block_top << " on " << block_below << " in " << biome
                                         << ", light " << light_top);
        }
        const BlockPos far{x + 10000, top, z};
        witness["snow " + kind + suffix].add(
            top_is_snow, rules.should_snow(level, at, climate.temperature_at(bc, far),
                                           static_cast<u8>(light_top)));
    }

    for (const auto& [name, tally] : tallies) {
        WARN(name << ": " << tally.line() << "   witness: " << witness[name].line());
    }
    WARN("columns not read: " << unparsed);

    // What must hold: on uniform frozen water and on uniform high ground the
    // noise decides, and the right noise must beat the displaced one.
    for (const std::string key : {"ice frozen uniform", "snow margin uniform"}) {
        if (tallies[key].total > 100) {
            CHECK(tallies[key].rate() > witness[key].rate());
        }
    }
}
