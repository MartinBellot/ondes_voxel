// Structure placement: the grid, the salts, the frequency rolls, the biome
// tags.
//
// Everything here is arithmetic over the world seed, and every value in it was
// checked against a world the real game wrote — the numbers pinned below are
// the game's answers, not ours. `tools/ov_structparity` is the measurement;
// these tests are the freeze, so that a refactor that moves a structure by one
// chunk fails here instead of being noticed a month later as "the villages
// look different".
//
// The frozen cases come from the reference world at seed 1234567890
// (`run/reference-1234567890`), whose chunk NBT records exactly which
// structures the game started where. See docs/provenance/structures.md.

#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

[[nodiscard]] bool have_data() { return std::filesystem::is_directory(data_root()); }

constexpr i64 kSeed = 1234567890;

}  // namespace

TEST_CASE("the two seeding helpers are different functions", "[structure]") {
    // They take the same arguments and they must never be swapped. The
    // affine one is checkable by hand; the folding one is not, so it is frozen
    // against itself and against the property that it depends on the world seed
    // in a way the affine one does not.
    REQUIRE(large_feature_with_salt(0, 0, 0, 0) == 0);
    REQUIRE(large_feature_with_salt(0, 1, 0, 0) == 341873128712LL);
    REQUIRE(large_feature_with_salt(0, 0, 1, 0) == 132897987541LL);
    REQUIRE(large_feature_with_salt(7, 0, 0, 11) == 18);

    // Negative chunks wrap through the signed multiply rather than saturating.
    REQUIRE(large_feature_with_salt(0, -1, 0, 0) == -341873128712LL);

    REQUIRE(large_feature_seed(kSeed, 0, 0) == kSeed);
    REQUIRE(large_feature_seed(kSeed, 1, 0) != large_feature_with_salt(kSeed, 1, 0, 0));
}

TEST_CASE("the mineshaft reducer is the one the reference world uses", "[structure]") {
    // The mineshaft set has spacing 1, so every chunk is a candidate and the
    // frequency roll is the *whole* placement. That makes it the cleanest test
    // of the reducers there is: these five chunks carry a mineshaft start in
    // the reference world at seed 1234567890, and these five do not while being
    // their immediate neighbours.
    RandomSpreadPlacement mineshafts;
    mineshafts.spacing    = 1;
    mineshafts.separation = 0;
    mineshafts.salt       = 0;
    mineshafts.frequency  = 0.004F;
    mineshafts.reduction  = FrequencyReduction::LegacyType3;

    const std::vector<std::pair<i32, i32>> starts{
        {-6560, 441}, {-4370, 256}, {-7, -10}, {3248, -4249}, {5628, 1314},
    };
    for (const auto& [x, z] : starts) {
        INFO("chunk " << x << " " << z);
        CHECK(mineshafts.is_candidate_chunk(kSeed, x, z));
        CHECK(mineshafts.passes_frequency(kSeed, x, z));
    }
    for (const auto& [x, z] : starts) {
        INFO("neighbour of " << x << " " << z);
        CHECK_FALSE(mineshafts.passes_frequency(kSeed, x + 1, z));
        CHECK_FALSE(mineshafts.passes_frequency(kSeed, x, z + 1));
    }

    // The other three reducers reject at least one of the five. If they did
    // not, this test would not be discriminating between them — trap 14.
    for (const FrequencyReduction wrong :
         {FrequencyReduction::Default, FrequencyReduction::LegacyType1,
          FrequencyReduction::LegacyType2}) {
        RandomSpreadPlacement other = mineshafts;
        other.reduction             = wrong;
        const bool all_pass =
            std::all_of(starts.begin(), starts.end(), [&](const std::pair<i32, i32>& c) {
                return other.passes_frequency(kSeed, c.first, c.second);
            });
        INFO("reducer " << to_string(wrong));
        CHECK_FALSE(all_pass);
    }
}

TEST_CASE("a grid cell has exactly one candidate", "[structure]") {
    RandomSpreadPlacement pyramids;
    pyramids.spacing    = 32;
    pyramids.separation = 8;
    pyramids.salt       = 14357617;

    for (i32 grid_z = -3; grid_z <= 3; ++grid_z) {
        for (i32 grid_x = -3; grid_x <= 3; ++grid_x) {
            const ChunkPos chosen = pyramids.candidate(kSeed, grid_x, grid_z);
            // The offset stays inside `spacing - separation`, never inside the
            // whole cell: that is what the separation buys.
            CHECK(chosen.x - grid_x * 32 >= 0);
            CHECK(chosen.x - grid_x * 32 < 32 - 8);
            CHECK(chosen.z - grid_z * 32 >= 0);
            CHECK(chosen.z - grid_z * 32 < 32 - 8);

            i32 found = 0;
            for (i32 z = 0; z < 32; ++z) {
                for (i32 x = 0; x < 32; ++x) {
                    if (pyramids.is_candidate_chunk(kSeed, grid_x * 32 + x, grid_z * 32 + z)) {
                        ++found;
                    }
                }
            }
            INFO("cell " << grid_x << " " << grid_z);
            CHECK(found == 1);
        }
    }
}

TEST_CASE("triangular spread peaks in the middle of the cell", "[structure]") {
    // The mean of two draws, not one. The distinction is invisible on a single
    // cell and unmistakable over a thousand: linear is flat, triangular is a
    // tent. A harness that only checks "the offset is in range" passes for
    // both, which is why this looks at the shape.
    RandomSpreadPlacement linear;
    linear.spacing    = 32;
    linear.separation = 8;
    linear.salt       = 10387313;
    linear.spread     = SpreadType::Linear;

    RandomSpreadPlacement triangular = linear;
    triangular.spread                = SpreadType::Triangular;

    i32 linear_middle     = 0;
    i32 triangular_middle = 0;
    for (i32 grid = 0; grid < 2000; ++grid) {
        const i32 range = 32 - 8;
        const i32 low   = range / 3;
        const i32 high  = 2 * range / 3;
        const i32 lx    = linear.candidate(kSeed, grid, 0).x - grid * 32;
        const i32 tx    = triangular.candidate(kSeed, grid, 0).x - grid * 32;
        linear_middle += (lx >= low && lx < high) ? 1 : 0;
        triangular_middle += (tx >= low && tx < high) ? 1 : 0;
    }
    CHECK(triangular_middle > linear_middle);
    // A flat distribution puts a third in the middle third; a tent puts about
    // half. Anything between says the draw count is wrong.
    CHECK(linear_middle < 800);
    CHECK(triangular_middle > 850);
}

TEST_CASE("the pack's structure sets load and keep their salts", "[structure]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/generated is not present");
    }
    auto sets = StructureSetRegistry::load(data_root());
    REQUIRE(sets.has_value());
    CHECK(sets->sets().size() == 19);

    const StructureSet* villages = sets->find("minecraft:villages");
    REQUIRE(villages != nullptr);
    REQUIRE(villages->spread.has_value());
    CHECK(villages->spread->spacing == 34);
    CHECK(villages->spread->separation == 8);
    CHECK(villages->spread->salt == 10387312);
    CHECK(villages->spread->spread == SpreadType::Linear);
    CHECK(villages->entries.size() == 5);

    const StructureSet* mansions = sets->find("minecraft:woodland_mansions");
    REQUIRE(mansions != nullptr);
    REQUIRE(mansions->spread.has_value());
    CHECK(mansions->spread->spread == SpreadType::Triangular);

    // The strongholds are the one set whose placement is not arithmetic, and
    // the loader says so rather than pretending.
    const StructureSet* strongholds = sets->find("minecraft:strongholds");
    REQUIRE(strongholds != nullptr);
    CHECK_FALSE(strongholds->spread.has_value());
    REQUIRE(strongholds->concentric.has_value());
    CHECK(strongholds->concentric->count == 128);

    const StructureSet* outposts = sets->find("minecraft:pillager_outposts");
    REQUIRE(outposts != nullptr);
    REQUIRE(outposts->spread.has_value());
    REQUIRE(outposts->spread->exclusion.has_value());
    CHECK(outposts->spread->exclusion->other_set == "minecraft:villages");
    CHECK(outposts->spread->exclusion->chunk_count == 10);

    CHECK(sets->set_of("minecraft:village_taiga") == villages);
}

TEST_CASE("biome tags resolve their references", "[structure]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/generated is not present");
    }
    auto tags = BiomeTags::load(data_root());
    REQUIRE(tags.has_value());
    CHECK(tags->tag_count() > 30);

    // `has_structure/mineshaft` names `#minecraft:is_ocean` among others, so a
    // resolver that stopped at the first level would answer no here.
    CHECK(tags->contains("#minecraft:has_structure/mineshaft", "minecraft:warm_ocean"));
    CHECK(tags->contains("minecraft:has_structure/mineshaft", "minecraft:plains"));
    // The badlands go to the mesa variant, and only there.
    CHECK_FALSE(tags->contains("#minecraft:has_structure/mineshaft", "minecraft:badlands"));
    CHECK(tags->contains("#minecraft:has_structure/mineshaft_mesa", "minecraft:badlands"));
    CHECK(tags->contains("#minecraft:has_structure/desert_pyramid", "minecraft:desert"));
    CHECK_FALSE(tags->contains("#minecraft:has_structure/desert_pyramid", "minecraft:plains"));

    CHECK(tags->known("#minecraft:has_structure/igloo"));
    CHECK_FALSE(tags->known("#minecraft:has_structure/nothing_like_this"));
}

TEST_CASE("the placer names every structure of the pack", "[structure]") {
    if (!have_data()) {
        SKIP("data/vanilla/1.20.1/generated is not present");
    }
    auto sets = StructureSetRegistry::load(data_root());
    REQUIRE(sets.has_value());
    auto placer = StructurePlacer::load(data_root(), *sets);
    REQUIRE(placer.has_value());
    CHECK(placer->structures().size() == 33);

    const StructureDefinition* mineshaft = placer->find("minecraft:mineshaft");
    REQUIRE(mineshaft != nullptr);
    CHECK(mineshaft->kind == StructureKind::Mineshaft);
    CHECK(mineshaft->anchor == GenerationAnchor::FixedHeight);
    CHECK(mineshaft->anchor_height == 50);
    CHECK(mineshaft->step == "underground_structures");

    const StructureDefinition* village = placer->find("minecraft:village_plains");
    REQUIRE(village != nullptr);
    CHECK(village->kind == StructureKind::Jigsaw);

    // Without a sampler the biome gate is reported as unanswered, never as a
    // pass. A harness that read `BiomeUnknown` as "placed" would be measuring
    // its own missing sampler.
    const auto decisions = placer->decide(kSeed, -7, -10, nullptr);
    CHECK(decisions.size() == 19);
    const auto mineshafts =
        std::find_if(decisions.begin(), decisions.end(), [](const StructurePlacementResult& r) {
            return r.set == "minecraft:mineshafts";
        });
    REQUIRE(mineshafts != decisions.end());
    CHECK(mineshafts->decision == PlacementDecision::BiomeUnknown);

    const auto strongholds =
        std::find_if(decisions.begin(), decisions.end(), [](const StructurePlacementResult& r) {
            return r.set == "minecraft:strongholds";
        });
    REQUIRE(strongholds != decisions.end());
    CHECK(strongholds->decision == PlacementDecision::Unsupported);
}
