// The entity loot tables, against the game's own data and the server's draws.
//
// The interpreter is code; the tables are data, regenerated locally. So what is
// checked here is the interpreter: that it reads every table in the file, that
// the conditions gate what the datapack says they gate, and that the counts
// come out of the ranges the tables declare.
//
// The full comparison against the real server — several hundred draws per table
// — is scripts/check_entity_loot.py, which needs a running 1.20.1 jar and
// therefore cannot live in a unit test.
#include "ov/gameplay/loot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<RecipeBook>              recipes;
    std::optional<EntityLootTables>        entity_loot;
    bool                                   present{false};
};

[[nodiscard]] std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return {};
    }
    return std::vector<u8>{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
}

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto base = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1";
        Loaded     out;
        auto       blocks = registry::BlockRegistry::load(base / "registry.ovpack");
        auto       regs   = registry::Registries::load(base / "registry.ovpack");
        if (!blocks || !regs) {
            return out;
        }
        out.blocks     = std::move(*blocks);
        out.registries = std::move(*regs);
        out.recipes.emplace(*out.registries);

        auto bytes = read_file(base / "entity_loot.ovpack");
        if (bytes.empty()) {
            return out;
        }
        auto tables = EntityLootTables::from_bytes(std::move(bytes), *out.registries,
                                                   &*out.recipes);
        if (!tables) {
            return out;
        }
        out.entity_loot.emplace(std::move(*tables));
        out.present = true;
        return out;
    }();
    return state;
}

[[nodiscard]] registry::ProtocolId item_of(std::string_view name) {
    const auto registry = loaded().registries->find("minecraft:item");
    REQUIRE(registry.has_value());
    const auto id = loaded().registries->protocol_id(*registry, name);
    REQUIRE(id.has_value());
    return *id;
}

/// Draw many times and total what came out, by item.
[[nodiscard]] std::map<registry::ProtocolId, i64> draw(const KillContext& kill, int times,
                                                       u64 seed = 12345) {
    math::XoroshiroRandomSource        random{static_cast<i64>(seed)};
    std::vector<Drop>                  drops;
    std::map<registry::ProtocolId, i64> totals;
    for (int i = 0; i < times; ++i) {
        drops.clear();
        const DrawResult result = loaded().entity_loot->drops(kill, random, drops);
        REQUIRE(result.complete());
        for (const Drop& drop : drops) {
            totals[drop.item] += drop.count;
        }
    }
    return totals;
}

}  // namespace

TEST_CASE("the entity tables load", "[gameplay][loot][entity]") {
    if (!loaded().present) {
        WARN("data/vanilla/1.20.1/entity_loot.ovpack is missing; "
             "run tools/ov_datagen/entity_loot.py");
        return;
    }
    // Eighty-one entity types plus the sixteen sheep colours. The sheep ones
    // live in a subdirectory and a `*.json` glob misses every one of them.
    REQUIRE(loaded().entity_loot->table_count() == 97);
    REQUIRE(loaded().entity_loot->has_table("minecraft:zombie"));
    REQUIRE(loaded().entity_loot->has_table("minecraft:sheep/white"));
    // A boat has no table. A *player* does — `entities/player.json` exists and
    // is empty — which is the same distinction bedrock and air make for blocks:
    // "there is no table" and "the table is empty" are different answers to
    // "why did nothing drop".
    REQUIRE(loaded().entity_loot->has_table("minecraft:player"));
    REQUIRE_FALSE(loaded().entity_loot->has_table("minecraft:boat"));
}

TEST_CASE("a zombie drops rotten flesh in the declared range",
          "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    KillContext kill;
    kill.entity_type = "minecraft:zombie";

    constexpr int kDraws = 4000;
    const auto    totals = draw(kill, kDraws);
    REQUIRE(totals.size() == 1);
    const i64 flesh = totals.at(item_of("minecraft:rotten_flesh"));
    // set_count uniform(0, 2): a mean of one per kill, so four thousand draws
    // land near four thousand. Six standard deviations of the binomial-ish
    // spread is about 150.
    REQUIRE(flesh > kDraws - 200);
    REQUIRE(flesh < kDraws + 200);
}

TEST_CASE("killed_by_player gates the rare drops", "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    // A zombie's iron ingot, carrot and potato are all behind killed_by_player
    // *and* a 2.5% chance. Without a player they never appear at all, which is
    // why a mob burned by lava leaves only flesh.
    KillContext lava;
    lava.entity_type = "minecraft:zombie";
    REQUIRE(draw(lava, 3000).size() == 1);

    KillContext slain;
    slain.entity_type     = "minecraft:zombie";
    slain.killed_by_player = true;
    const auto totals      = draw(slain, 3000);
    REQUIRE(totals.size() > 1);
}

TEST_CASE("burning meat comes out cooked", "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    KillContext cold;
    cold.entity_type = "minecraft:cow";
    const auto raw   = draw(cold, 200);
    REQUIRE(raw.contains(item_of("minecraft:beef")));
    REQUIRE_FALSE(raw.contains(item_of("minecraft:cooked_beef")));

    KillContext burning = cold;
    burning.on_fire     = true;
    const auto cooked   = draw(burning, 200);
    REQUIRE(cooked.contains(item_of("minecraft:cooked_beef")));
    REQUIRE_FALSE(cooked.contains(item_of("minecraft:beef")));
    // The leather is not food and comes through untouched, which is the case
    // that would break a furnace_smelt applied to the whole pool.
    REQUIRE(cooked.contains(item_of("minecraft:leather")));
}

TEST_CASE("looting adds to the count and to the chance", "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    KillContext plain;
    plain.entity_type = "minecraft:zombie";
    KillContext looted = plain;
    looted.looting     = 3;

    constexpr int kDraws  = 4000;
    const i64     without = draw(plain, kDraws).at(item_of("minecraft:rotten_flesh"));
    const i64     with    = draw(looted, kDraws).at(item_of("minecraft:rotten_flesh"));
    // looting_enchant uniform(0,1) times three, rounded: about 1.5 more per
    // kill on average, so the total should be well over half again.
    REQUIRE(with > without + kDraws);

    // random_chance_with_looting raises a *chance*, not a count: a drowned's
    // copper ingot is 11% and climbs by two points a level. (Its trident is not
    // in the loot table at all — that one is an equipment drop, which is a
    // different system entirely.)
    KillContext drowned;
    drowned.entity_type      = "minecraft:drowned";
    drowned.killed_by_player = true;
    KillContext lucky        = drowned;
    lucky.looting            = 3;
    const i64 bare = draw(drowned, 6000)[item_of("minecraft:copper_ingot")];
    const i64 rich = draw(lucky, 6000)[item_of("minecraft:copper_ingot")];
    REQUIRE(bare > 500);
    REQUIRE(rich > bare);
}

TEST_CASE("a slime's size decides whether it drops anything",
          "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    KillContext tiny;
    tiny.entity_type = "minecraft:slime";
    tiny.slime_size  = 1;
    REQUIRE_FALSE(draw(tiny, 500).empty());

    KillContext big;
    big.entity_type = "minecraft:slime";
    big.slime_size  = 2;
    // Only size 1 drops slimeballs; a bigger one splits instead.
    REQUIRE(draw(big, 500).empty());
}

TEST_CASE("a creeper drops a disc only to a skeleton", "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    KillContext blown;
    blown.entity_type = "minecraft:creeper";
    const auto plain  = draw(blown, 200);
    REQUIRE(plain.size() == 1);  // gunpowder only

    KillContext shot;
    shot.entity_type = "minecraft:creeper";
    shot.killer_type = "minecraft:skeleton";
    const auto discs = draw(shot, 200);
    REQUIRE(discs.size() > 1);

    // A zombie killer is not in #minecraft:skeletons and gets nothing extra.
    KillContext bitten;
    bitten.entity_type = "minecraft:creeper";
    bitten.killer_type = "minecraft:zombie";
    REQUIRE(draw(bitten, 200).size() == 1);
}

TEST_CASE("a magma cube killed by the wrong frog drops nothing special",
          "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    // The one table in the game that asks which variety of frog ate it. Size 2
    // and up, and a warm frog, gives a pearlescent froglight.
    KillContext eaten;
    eaten.entity_type        = "minecraft:magma_cube";
    eaten.slime_size         = 2;
    eaten.source_entity_type = "minecraft:frog";
    eaten.source_frog_variant = "minecraft:warm";
    const auto warm           = draw(eaten, 100);
    REQUIRE(warm.contains(item_of("minecraft:pearlescent_froglight")));

    KillContext cold  = eaten;
    cold.source_frog_variant = "minecraft:cold";
    const auto chilled       = draw(cold, 100);
    REQUIRE(chilled.contains(item_of("minecraft:verdant_froglight")));
    REQUIRE_FALSE(chilled.contains(item_of("minecraft:pearlescent_froglight")));

    // Killed by anything else: only the magma cream, and only at size two or
    // more.
    KillContext stabbed;
    stabbed.entity_type = "minecraft:magma_cube";
    stabbed.slime_size  = 2;
    const auto ordinary = draw(stabbed, 200);
    REQUIRE(ordinary.contains(item_of("minecraft:magma_cream")));
    REQUIRE(ordinary.size() == 1);
}

TEST_CASE("a table that delegates says so rather than dropping nothing",
          "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    // A guardian's table points at minecraft:gameplay/fishing/fish, which is not
    // an entity table and is not in this file. The draw must report the gap
    // rather than produce a result that looks like a mob with fewer drops.
    // Two tables in the game do this and the guardian is one of them; the
    // dolphin, which sounds like it should, does not.
    math::XoroshiroRandomSource random{7};
    std::vector<Drop>           drops;
    KillContext                 kill;
    kill.entity_type     = "minecraft:guardian";
    kill.killed_by_player = true;
    u32 seen = 0;
    for (int i = 0; i < 200; ++i) {
        drops.clear();
        seen += loaded().entity_loot->drops(kill, random, drops).referenced_tables;
    }
    REQUIRE(seen > 0);
}

TEST_CASE("an entity with no table draws nothing and says nothing is wrong",
          "[gameplay][loot][entity]") {
    if (!loaded().present) {
        return;
    }
    math::XoroshiroRandomSource random{1};
    std::vector<Drop>           drops;
    KillContext                 kill;
    kill.entity_type        = "minecraft:boat";
    const DrawResult result = loaded().entity_loot->drops(kill, random, drops);
    REQUIRE(drops.empty());
    REQUIRE(result.complete());
}
