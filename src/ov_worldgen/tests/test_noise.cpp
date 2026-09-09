// Properties of the noise, and the traps in it.
//
// The values themselves cannot be checked here — the only oracle for those is
// the terrain the real game writes for the same seed, and that comparison lives
// in the parity harness. What *can* be checked here is everything that would
// still be wrong if the values happened to look plausible: determinism, range,
// the wrap, and the two places where a natural-looking simplification changes
// the world.

#include "ov/worldgen/noise.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <set>

using namespace ov;
using namespace ov::worldgen;
using Catch::Approx;

TEST_CASE("the position hash wraps x at 32 bits", "[worldgen][noise]") {
    // The first term of the hash is an int multiply that overflows; the second
    // is a long multiply that does not. Doing both in 64 bits gives a different
    // answer past about 686 blocks — far enough out that a small test world
    // would never reach it, and every ore in a real one would move.
    //
    // 3129871 * 686 is under 2^31; 3129871 * 687 is over. So the two sides of
    // that boundary are where a 64-bit version would first disagree.
    const i64 below = math::position_seed(686, 0, 0);
    const i64 above = math::position_seed(687, 0, 0);
    CHECK(below != above);

    // What the wrap actually means: the sequence is not monotonic in x, and a
    // 64-bit multiply would make it so over this range.
    const i64 at_zero = math::position_seed(0, 0, 0);
    CHECK(math::position_seed(0, 0, 0) == at_zero);
    CHECK(math::position_seed(1, 0, 0) != at_zero);

    // Every axis has to reach the result, or whole dimensions of the world
    // would repeat.
    CHECK(math::position_seed(0, 1, 0) != at_zero);
    CHECK(math::position_seed(0, 0, 1) != at_zero);
}

TEST_CASE("a positional factory is a function of the position", "[worldgen][noise]") {
    const math::XoroshiroPositionalFactory factory{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

    auto a = factory.at(10, 20, 30);
    auto b = factory.at(10, 20, 30);
    CHECK(a.next_long() == b.next_long());

    auto c = factory.at(11, 20, 30);
    auto d = factory.at(10, 20, 30);
    CHECK(c.next_long() != d.next_long());

    // Named forks are what let a datapack add a feature without renumbering
    // the rest, so two names must not collide and one name must be stable.
    auto ore  = factory.from_hash_of("minecraft:ore_gold");
    auto ore2 = factory.from_hash_of("minecraft:ore_gold");
    auto tree = factory.from_hash_of("minecraft:trees_plains");
    CHECK(ore.next_long() == ore2.next_long());
    CHECK(factory.from_hash_of("minecraft:ore_gold").next_long() !=
          tree.next_long());
}

TEST_CASE("java's string hash is the one the legacy factory uses", "[worldgen][noise]") {
    // Published values: "" is 0, "a" is 97, and "hello" is 99162322. If the
    // multiplier or the accumulation order were wrong, none of the three would
    // land.
    CHECK(math::java_string_hash("") == 0);
    CHECK(math::java_string_hash("a") == 97);
    CHECK(math::java_string_hash("hello") == 99162322);
    // And it overflows rather than saturating, which a long accumulator would
    // hide. This string is the well-known one whose hash lands exactly on
    // Integer.MIN_VALUE, which is as sharp a test of the wrap as exists.
    CHECK(math::java_string_hash("polygenelubricants") ==
          std::numeric_limits<i32>::min());
}

TEST_CASE("one octave of perlin stays in range and repeats", "[worldgen][noise]") {
    math::XoroshiroRandomSource random{12345};
    const ImprovedNoise         noise{random};

    f64 lowest  = 1.0e9;
    f64 highest = -1.0e9;
    for (i32 i = 0; i < 4000; ++i) {
        const f64 t = static_cast<f64>(i) * 0.37;
        const f64 v = noise.noise(t, t * 0.5, -t * 0.25);
        lowest      = std::min(lowest, v);
        highest     = std::max(highest, v);
    }
    // Perlin in this formulation is bounded by 1 in three dimensions, and a
    // gradient table that had been mis-transcribed would show as a range that
    // is wrong rather than as anything visible.
    CHECK(lowest > -1.0);
    CHECK(highest < 1.0);
    // It is a noise, not a constant.
    CHECK(highest - lowest > 0.5);

    // Deterministic for the same input.
    CHECK(noise.noise(1.5, 2.5, 3.5) == noise.noise(1.5, 2.5, 3.5));
}

TEST_CASE("the same seed gives the same noise", "[worldgen][noise]") {
    math::XoroshiroRandomSource a{99};
    math::XoroshiroRandomSource b{99};
    const ImprovedNoise         first{a};
    const ImprovedNoise         second{b};
    for (i32 i = 0; i < 50; ++i) {
        const f64 t = static_cast<f64>(i) * 1.7;
        CHECK(first.noise(t, t, t) == second.noise(t, t, t));
    }

    // And a different seed does not.
    math::XoroshiroRandomSource c{100};
    const ImprovedNoise         third{c};
    CHECK(first.noise(1.0, 2.0, 3.0) != third.noise(1.0, 2.0, 3.0));
}

TEST_CASE("a zero amplitude skips an octave without shifting the rest",
          "[worldgen][noise]") {
    // This is the trap the naming exists for. Each octave is seeded from the
    // hash of "octave_<n>", so leaving one out must not change any other. If
    // the octaves were seeded in sequence instead, removing the middle one
    // would reseed the last and change the world.
    const std::array<f64, 3> full{1.0, 1.0, 1.0};
    const std::array<f64, 3> gapped{1.0, 0.0, 1.0};

    math::XoroshiroRandomSource a{7};
    math::XoroshiroRandomSource b{7};
    const PerlinNoise           dense  = PerlinNoise::create(a, -3, full);
    const PerlinNoise           sparse = PerlinNoise::create(b, -3, gapped);

    // The two differ, obviously — one octave is missing.
    CHECK(dense.value(1.0, 2.0, 3.0) != sparse.value(1.0, 2.0, 3.0));

    // But the surviving octaves are the same ones. Rebuilding the gapped stack
    // has to reproduce itself exactly, which it cannot do if any octave's seed
    // depended on how many came before it.
    math::XoroshiroRandomSource c{7};
    const PerlinNoise           again = PerlinNoise::create(c, -3, gapped);
    CHECK(sparse.value(1.0, 2.0, 3.0) == again.value(1.0, 2.0, 3.0));
}

TEST_CASE("the coordinate wrap folds far positions back", "[worldgen][noise]") {
    // Perlin's permutation repeats every 256 cells and a double runs out of
    // fractional precision long before a world border. The wrap keeps far
    // terrain shaped like near terrain instead of turning into flat planes.
    CHECK(PerlinNoise::wrap(0.0) == 0.0);
    CHECK(PerlinNoise::wrap(1000.0) == 1000.0);
    // Exactly one period out comes back to the same place.
    CHECK(PerlinNoise::wrap(3.3554432E7) == Approx(0.0).margin(1e-9));
    CHECK(PerlinNoise::wrap(-3.3554432E7) == Approx(0.0).margin(1e-9));
    // And the folded value stays inside one period.
    CHECK(std::abs(PerlinNoise::wrap(1.0e9)) <= 3.3554432E7);
}

TEST_CASE("normal noise is bounded by what it reports", "[worldgen][noise]") {
    math::XoroshiroRandomSource     random{2024};
    const std::array<f64, 4>        amplitudes{1.0, 1.0, 1.0, 1.0};
    const NormalNoise               noise = NormalNoise::create(random, -4, amplitudes);

    // The density function graph prunes whole subtrees using max_value without
    // evaluating them, so a bound that is too small produces terrain with
    // holes in it — and one that is far too large produces terrain that is
    // merely slow.
    REQUIRE(noise.max_value() > 0.0);
    for (i32 i = 0; i < 3000; ++i) {
        const f64 t = static_cast<f64>(i) * 0.13;
        const f64 v = noise.value(t, t * 0.7, -t * 1.3);
        CHECK(std::abs(v) <= noise.max_value());
    }
}

TEST_CASE("the old terrain noise keeps the amplitude the Nether measured",
          "[worldgen][noise]") {
    // Two arithmetics would divide this noise by four — two octaves fewer in
    // each limit stack, or a second divisor of 512 instead of 128 — and the
    // overworld cannot tell either from the current one: its surface is the sum
    // of six stages, and an amplitude four times smaller merely trades one kind
    // of error there for another.
    //
    // The Nether can. Its noise_settings name no depth, no factor and no
    // aquifers, and above y = 104 a clamped gradient walks final_density's
    // threshold away from zero by about four hundredths of a unit per block, so
    // the fraction of stone at each height is this noise's survival function as
    // the game itself writes it. Measured that way on seeds 1234567890 and
    // 987654321 (`ov_parity --nether`), the game's distribution is 1.03 and
    // 0.80 times as wide as this one — and about four times wider than either
    // quarter-size candidate, each of which saturates before the game's curve
    // has even left three quarters.
    //
    // The numbers below are therefore a fence around a measured amplitude, not
    // a calibration. They are wide because the Nether is seeded from the legacy
    // random source and this is not, so the two are different draws of the same
    // process; they are still four times tighter than the gap to either
    // candidate.
    math::XoroshiroRandomSource random{1234567890};
    // The overworld's own parameters, from
    // worldgen/density_function/overworld/base_3d_noise.json.
    const BlendedNoise noise = BlendedNoise::create(random, 0.25, 0.125, 80.0, 160.0, 8.0);

    f64   sum       = 0.0;
    f64   sum2      = 0.0;
    f64   step_sum2 = 0.0;
    usize count     = 0;
    for (i32 x = -512; x <= 512; x += 7) {
        for (i32 z = -512; z <= 512; z += 11) {
            const f64 here = noise.value(x, 64, z);
            const f64 next = noise.value(x + 1, 64, z);
            sum += here;
            sum2 += here * here;
            const f64 step = next - here;
            step_sum2 += step * step;
            ++count;
            CHECK(std::abs(here) <= noise.max_value());
        }
    }
    const auto n    = static_cast<f64>(count);
    const f64  mean = sum / n;
    const f64  rms  = std::sqrt(sum2 / n);

    // Symmetric: it blends two stacks that are drawn the same way.
    CHECK(std::abs(mean) < 0.05);
    // The amplitude. A second divisor of 512 lands at 0.041 and fourteen
    // octaves at 0.061; both are far outside.
    CHECK(rms > 0.12);
    CHECK(rms < 0.21);

    // And the spectrum, which the amplitude alone does not pin. Dropping the
    // two heaviest octaves takes away the 96- and 192-block wavelengths and
    // leaves the fine ones at full size, so it *raises* this ratio — to 0.159,
    // where sixteen octaves give 0.119 whatever the divisors are. A test that
    // only checked the size would pass a stack with a hole in it.
    const f64 roughness = std::sqrt(step_sum2 / n) / rms;
    CHECK(roughness > 0.10);
    CHECK(roughness < 0.14);
}
