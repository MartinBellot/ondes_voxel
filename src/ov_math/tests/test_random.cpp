#include "ov/math/random.hpp"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <numeric>
#include <vector>

using namespace ov;
using namespace ov::math;

// Reference vectors produced by a real JVM, from scripts/gen_random_vectors.java.
//
// They are the point of this file. An implementation checked only against
// itself proves nothing here: a generator that consumes state in the wrong
// order still produces excellent-looking random numbers, and the world it
// builds shares no block with vanilla. The oracle has to come from outside.
//
// Floats and doubles are compared as raw bits, so no formatting or rounding
// sits between the JVM's answer and ours.
//
// Regenerate with:  java scripts/gen_random_vectors.java
namespace {

struct Vector {
    i64  seed;
    i32  next_int[8];
    i64  next_long[8];
    i32  next_int_100[8];
    i32  next_int_16[8];
    u32  next_float_bits[8];
    u64  next_double_bits[8];
    bool next_boolean[8];
    u64  next_gaussian_bits[8];
};

// clang-format off
const Vector kVectors[] = {
{0LL, {-1155484576,-723955400,1033096058,-1690734402,-1557280266,1327362106,-1930858313,502539523,}, {-4962768465676381896LL,4437113781045784766LL,-6688467811848818630LL,-8292973307042192125LL,-7423979211207825555LL,6146794652083548235LL,7105486291024734541LL,-279624296851435688LL,}, {60,48,29,47,15,53,91,61,}, {11,13,3,9,10,4,8,1,}, {0x3f3b20b4,0x3f54d951,0x3e764f2c,0x3f1b3970,0x3f232dc9,0x3e9e3be0,0x3f0ce970,0x3defa128,}, {0x3fe764168ea6ca89ULL,0x3fcec9e5b3672e14ULL,0x3fe465b93a78ef81ULL,0x3fe19d2e10efa128ULL,0x3fe31f174640953bULL,0x3fd55373440b5f04ULL,0x3fd8a6f089cefe94ULL,0x3fef83d267dcd07aULL,}, {true,true,false,true,true,false,true,false,}, {0x3fe9ae59d1d6f861ULL,0xbfecd9772eb2e0c8ULL,0x4000a5b9cca3a4b8ULL,0x3fe870cf65026a96ULL,0x3fef81a273668e4aULL,0xbffaef41b15175aaULL,0xbf9bf1fa8ac12503ULL,0x3fbd80be0ccc0326ULL,}},
    {1LL, {-1155869325,431529176,1761283695,1749940626,892128508,155629808,1429008869,-1465154083,}, {-4964420948893066024LL,7564655870752979346LL,3831662765844904176LL,6137546356583794141LL,-594798593157429144LL,112842269129291794LL,-669528114487223426LL,-1109287713991315740LL,}, {85,88,47,13,54,4,34,6,}, {11,1,6,6,3,0,5,10,}, {0x3f3b1ad5,0x3dcdc4e0,0x3ed1f61c,0x3ed09bf2,0x3e54b330,0x3d146b80,0x3eaa59e2,0x3f28ab85,}, {0x3fe7635aa8cdc4e6ULL,0x3fda3ec39684df98ULL,0x3fca96666128d71cULL,0x3fd54b3c7a8ab85cULL,0x3feef7db3daf9843ULL,0x3f790e549c66e000ULL,0x3feed6ab7146d0dbULL,0x3fee1360946ebd77ULL,}, {true,false,false,false,false,false,false,true,}, {0x3ff8fc3c669aa4c1ULL,0xbfe3763b5ee2e541ULL,0xbff175ab5e5bb186ULL,0xbfe3fc3b989cfc86ULL,0xbff1e47cef7b6c24ULL,0xbffa887c6af9fb4dULL,0xbffe1d5865029d92ULL,0x3fae56bc75554deeULL,}},
    {-1LL, {1155099827,1887904451,52699159,-1941176418,-1451336087,-1714570420,1788588954,1714930956,}, {4961115982468162243LL,226341162490527646LL,-6233441030884181172LL,7681931065131779340LL,-3206673117535979274LL,-3577981259754968255LL,1047579610836041353LL,1519476466405487669LL,}, {13,25,79,39,4,38,77,78,}, {4,7,0,8,10,9,6,6,}, {0x3e89b2d8,0x3ee10e44,0x3c490800,0x3f0c4bff,0x3f297e5e,0x3f19cdbb,0x3ed53766,0x3ecc6f8a,}, {0x3fd1365b2708722cULL,0x3f8921011897ff00ULL,0x3fe52fcbccce6ddaULL,0x3fdaa6ece6637c50ULL,0x3fea6ff37873c9c7ULL,0x3fe9cb0ea27a675aULL,0x3fad138008e97f40ULL,0x3fb51643ec2a8d08ULL,}, {false,false,false,true,true,true,false,false,}, {0x3ffc90b7b3790a3aULL,0xbfed740e27d7f93cULL,0x3fdf2a0338e45a13ULL,0x3fdd3daa8a1780edULL,0x3ffb32df66dd7991ULL,0x3fdcf0e9905364beULL,0x3fc4495fa2f644acULL,0x3fe5fd8492381630ULL,}},
    {42LL, {-1170105035,234785527,-1360544799,205897768,1325939940,-248792245,1190043011,-1255373459,}, {-5025562857975149833LL,-5843495416241995736LL,5694868678511409995LL,5111195811822994797LL,-6169532649852302182LL,-1782466964123969572LL,6802844026563419272LL,5086654115216342560LL,}, {30,63,48,84,70,25,5,18,}, {11,0,10,0,4,15,4,11,}, {0x3f3a419d,0x3d5fe8a0,0x3f2ee7bb,0x3d445c00,0x3e9e1078,0x3f712bbb,0x3e8ddd3a,0x3f352c85,}, {0x3fe74833a06ff457ULL,0x3fe5dcf778622e01ULL,0x3fd3c20f3f12bbb4ULL,0x3fd1bba76b52c856ULL,0x3fe54c2d50bb0864ULL,0x3fece86cf39c2cbeULL,0x3fd79a23a61b35c8ULL,0x3fd1a5db3b0bfbe2ULL,}, {true,false,true,false,false,true,false,true,}, {0x3ff2453e82115d86ULL,0x3fed6bca38120847ULL,0xbfee654eb7a040c2ULL,0xbff1b63b72513280ULL,0x3fd1fb89a19b83afULL,0x3fe5e86e10aad3bcULL,0xbfea26ad824bcbc5ULL,0xbff658a6c0aad25aULL,}},
    {1234567890123LL, {-8722476,-1977939436,-999828940,-741339138,-916778800,-1183211565,-1030857102,-187463566,}, {-37462751138084332LL,-4294232599635685378LL,-3937534964849336365LL,-4427497540126799758LL,3936690402237823603LL,-1760658433924376084LL,4898561677383822704LL,798860473662761487LL,}, {10,30,78,79,48,65,97,65,}, {15,8,12,13,12,11,12,15,}, {0x3f7f7ae7,0x3f0a1b0a,0x3f4467d2,0x3f53d00f,0x3f495b10,0x3f39799f,0x3f428e5e,0x3f74d388,}, {0x3fefef5cfc50d850ULL,0x3fe88cfa469e807fULL,0x3fe92b621dcbccfeULL,0x3fe851cbcfa69c43ULL,0x3fcb50f78702674cULL,0x3fecf21c4c9922cfULL,0x3fd0fecbf52cded6ULL,0x3fa62c3ea2e27500ULL,}, {true,true,true,true,true,true,true,true,}, {0x3fe80047f63a0efdULL,0x3fe5c6bf0cb92975ULL,0xbfbb3e6856119664ULL,0x3fc33a8f2030d783ULL,0xbffa14742a80ca1dULL,0x3fdebe7421f29af3ULL,0xbfbc318254631fc5ULL,0xbfebb83f67f16364ULL,}},
    {25214903917LL, {0,4232237,178803790,758674372,1565954732,392261992,396415378,2092582042,}, {4232237LL,767956431209526212LL,6725724361348706664LL,1702591086234059930LL,-8729916473397100619LL,4301382885493934562LL,-1259331885315892525LL,-8188614243507809791LL,}, {0,18,95,86,66,96,89,21,}, {0,0,0,2,5,1,1,7,}, {0x00000000,0x3a812800,0x3d2a8540,0x3e34e1cc,0x3ebaad24,0x3dbb0b88,0x3dbd0688,0x3ef97488,}, {0x3db0250800000000ULL,0x3fa550a8969c39e0ULL,0x3fd755a4a1761716ULL,0x3fb7a0d19f2e9120ULL,0x3fe0db2363a2adfdULL,0x3fcdd8ca7a99f13cULL,0x3fedd0be8c8b62d6ULL,0x3fe1cb8650681a90ULL,}, {false,false,false,false,false,false,false,false,}, {0xbfcf6e880764bd18ULL,0xbfe7a951ad374034ULL,0x3fc4283b285f629aULL,0xbff921dd26fd44e0ULL,0x3fe79d23004f2801ULL,0x3fb88b7eb98dc62dULL,0x3fea6af80e0627caULL,0xbfe87f65349e8ae3ULL,}},
    {-998877665544332211LL, {-2141513760,637211273,234373881,-819395422,1825824565,893031501,-2072990514,1261468008,}, {-9197731562496781687LL,1006628153112200354LL,7841856795801457741LL,-8903426461286762136LL,-5337935072573309896LL,-686758658671286600LL,5472169359492435992LL,6401326389558612056LL,}, {68,36,40,37,82,50,91,4,}, {8,2,0,12,6,3,8,4,}, {0x3f005b17,0x3e17ec48,0x3d5f8420,0x3f4f2904,0x3ed9a7be,0x3e54ea50,0x3f0470ac,0x3e9660f2,}, {0x3fe00b62f92fd894ULL,0x3fabf085e7948250ULL,0x3fdb34f7c353a944ULL,0x3fe08e159a5983cbULL,0x3fe6bd7b0a2959e1ULL,0x3feecf0493bbcb35ULL,0x3fd2fc4342752d00ULL,0x3fd63585184f9904ULL,}, {true,false,false,true,false,false,true,false,}, {0x3f61614768a671e2ULL,0xbfe5c19ce792058cULL,0xc0055453bcc03eb5ULL,0x3fe3c23c4eb8f6f7ULL,0xbff50406e5e525fdULL,0xbfef9ed59122d7d5ULL,0x3fac15646321b4fbULL,0xbff824eed2d39189ULL,}},
};
// clang-format on

}  // namespace

TEST_CASE("the legacy source matches a real JVM draw for draw", "[math][random]") {
    for (const Vector& v : kVectors) {
        {
            LegacyRandomSource r{v.seed};
            for (const i32 expected : v.next_int) {
                REQUIRE(r.next_int() == expected);
            }
        }
        {
            LegacyRandomSource r{v.seed};
            for (const i64 expected : v.next_long) {
                REQUIRE(r.next_long() == expected);
            }
        }
        {
            LegacyRandomSource r{v.seed};
            for (const bool expected : v.next_boolean) {
                REQUIRE(r.next_boolean() == expected);
            }
        }
    }
}

TEST_CASE("a bounded draw matches, both for powers of two and not", "[math][random]") {
    // Two different code paths in the specification. A power-of-two bound
    // takes the high bits of the draw; anything else rejects the uneven tail.
    // Getting the second one wrong shifts every ore vein in the world by an
    // amount too small to look like a bug.
    for (const Vector& v : kVectors) {
        {
            LegacyRandomSource r{v.seed};
            for (const i32 expected : v.next_int_100) {
                REQUIRE(r.next_int(100) == expected);
            }
        }
        {
            LegacyRandomSource r{v.seed};
            for (const i32 expected : v.next_int_16) {
                REQUIRE(r.next_int(16) == expected);
            }
        }
    }
}

TEST_CASE("floating point draws match to the bit", "[math][random]") {
    // Compared as bits rather than with a tolerance: "close enough" is exactly
    // the failure mode that makes a divergent world impossible to diagnose.
    for (const Vector& v : kVectors) {
        {
            LegacyRandomSource r{v.seed};
            for (const u32 expected : v.next_float_bits) {
                REQUIRE(std::bit_cast<u32>(r.next_float()) == expected);
            }
        }
        {
            LegacyRandomSource r{v.seed};
            for (const u64 expected : v.next_double_bits) {
                REQUIRE(std::bit_cast<u64>(r.next_double()) == expected);
            }
        }
    }
}

TEST_CASE("a bound of zero or less yields zero rather than trapping", "[math][random][malformed]") {
    // Reached from datapack data. A malformed pack must not be able to abort a
    // tick, and a modulo by zero would.
    LegacyRandomSource r{42};
    REQUIRE(r.next_int(0) == 0);
    REQUIRE(r.next_int(-5) == 0);
}

TEST_CASE("reseeding restarts the stream exactly", "[math][random]") {
    LegacyRandomSource r{1234};
    const i32          first = r.next_int();
    for (int i = 0; i < 100; ++i) {
        (void)r.next_int();
    }

    r.set_seed(1234);
    REQUIRE(r.next_int() == first);
}

TEST_CASE("reseeding drops the cached gaussian", "[math][random]") {
    // The polar method makes two values per pair of draws and keeps the spare.
    // A spare surviving a reseed would leak one world's value into the next,
    // and only ever on the first call — the kind of bug that survives years.
    LegacyRandomSource r{7};
    (void)r.next_gaussian();  // leaves a spare cached

    r.set_seed(7);
    LegacyRandomSource fresh{7};
    REQUIRE(std::bit_cast<u64>(r.next_gaussian()) == std::bit_cast<u64>(fresh.next_gaussian()));
}

TEST_CASE("the gaussian agrees with the JVM to within four ulp, not to the bit", "[math][random]") {
    // A measurement, pinned as a limit rather than asserted as a success.
    //
    // Java specifies StrictMath.log, which means fdlibm. The platform's libm is
    // free to differ in the last places and macOS does: 44 of 56 reference
    // values come back bit-identical, the rest within 4 ulp.
    //
    // Everything above this point IS bit-exact — every integer, float, double
    // and boolean draw — and those are what terrain, loot and block ticks are
    // built from. The gaussian is used for velocity spread on dropped items and
    // similar, where 4 ulp is invisible.
    //
    // This test exists so nobody later assumes the stronger claim. Closing the
    // gap needs an fdlibm log; it is on the roadmap, and until it lands the
    // honest statement is the one written here.
    int exact = 0;
    int total = 0;

    for (const Vector& v : kVectors) {
        LegacyRandomSource r{v.seed};
        for (const u64 expected : v.next_gaussian_bits) {
            const u64 got = std::bit_cast<u64>(r.next_gaussian());
            ++total;
            if (got == expected) {
                ++exact;
                continue;
            }
            // Same sign and exponent, differing only in the low significand
            // bits: a rounding difference, not a different value.
            const u64 difference = got > expected ? got - expected : expected - got;
            REQUIRE(difference <= 4);
        }
    }

    REQUIRE(total == 56);
    REQUIRE(exact >= 44);
}

// ── Xoroshiro128++ ──────────────────────────────────────────────────────────

// Reference vectors from the JDK's own Xoroshiro128PlusPlus, via
// scripts/gen_xoroshiro_vectors.java.
//
// The JDK ships this generator as a standard algorithm, which makes it an
// oracle for the whole of it: the transition function, the Stafford-13 seed
// mix, and the seed upgrade — the JDK XORs by the silver ratio before mixing,
// which is precisely what the game does. So a plain seed goes in and the entire
// path is checked end to end. A wrong rotation, a wrong mix constant or a
// swapped state half and nothing lines up.
//
// I first assumed the JDK used the golden ratio here and the vectors said
// otherwise; the constant is the silver ratio, and that is what made this an
// oracle for the seed upgrade rather than only for the core.
namespace {

struct XoroVector {
    i64 seed;
    i64 next_long[8];
};

// clang-format off
const XoroVector kXoroVectors[] = {
{0LL, {3038984756725240190LL,-3694039286755638414LL,4633751808701151732LL,2160572957309072155LL,1839370574944072389LL,-4488466507718817201LL,-4199796579929588030LL,-1069045159880208415LL,}},
    {1LL, {-1033667707219518978LL,6451672561743293322LL,-1821890263888393630LL,890086654470169703LL,8094835630745194324LL,2779418831538184155LL,-2153570570747265786LL,2631759950516672506LL,}},
    {-1LL, {-8676505878415342125LL,-868585888688873692LL,-6331679347063163302LL,-2068491455652362927LL,-5626054917968568837LL,350347487066691045LL,5757290794395940LL,-3423761802310783585LL,}},
    {42LL, {-4695948378737616609LL,7341713790291473579LL,-7542733514721318211LL,4888889476139319686LL,8419651034331256779LL,-6491934549477179079LL,8279452174680803839LL,6246239634032559210LL,}},
    {1234567890123LL, {-2624490476143626894LL,-4120571446321590704LL,-8577917762645600131LL,-9031676666403446600LL,3319664545208770767LL,-5757480376081603304LL,-6806829506973170798LL,1311108021591173775LL,}},
    {-998877665544332211LL, {7519316578565276097LL,573110358808193159LL,-3819507108672464273LL,-3391829786671280193LL,-6676585584767917494LL,4624326125635116349LL,-5186098402426869151LL,8096676595958135085LL,}},
};
// clang-format on

}  // namespace

TEST_CASE("xoroshiro reproduces the JDK stream draw for draw", "[math][random]") {
    for (const XoroVector& v : kXoroVectors) {
        XoroshiroRandomSource r{v.seed};
        for (const i64 expected : v.next_long) {
            REQUIRE(r.next_long() == expected);
        }
    }
}

TEST_CASE("the game's own seed upgrade spreads adjacent seeds apart", "[math][random]") {
    // Seeds 1 and 2 are one bit apart. Without the mix their states would be
    // too, and two worlds a player types seconds apart would look related.
    const XoroshiroRandomSource a{1};
    const XoroshiroRandomSource b{2};

    REQUIRE(a.state_lo() != b.state_lo());
    REQUIRE(a.state_hi() != b.state_hi());

    // Not merely different: unrelated. Half the bits should differ.
    const int distance = std::popcount(a.state_lo() ^ b.state_lo());
    REQUIRE(distance > 16);
    REQUIRE(distance < 48);

    // And the two halves of one state must not be related either.
    REQUIRE(a.state_lo() != a.state_hi());
}

TEST_CASE("a zero seed still produces a live generator", "[math][random]") {
    // Xoroshiro has one forbidden state: all zeroes, which it never leaves.
    // The seed upgrade is what keeps a seed of 0 away from it, and a world
    // seeded 0 is common enough that this is not a theoretical concern.
    XoroshiroRandomSource r{0};
    REQUIRE((r.state_lo() != 0 || r.state_hi() != 0));

    bool any_nonzero = false;
    for (int i = 0; i < 16; ++i) {
        any_nonzero = any_nonzero || r.next_long() != 0;
    }
    REQUIRE(any_nonzero);
}

TEST_CASE("xoroshiro's bounded draw is uniform and in range", "[math][random]") {
    // Lemire's method, not the legacy modulo rejection. The two consume
    // different amounts of state, so they are not interchangeable even though
    // both are correct in isolation.
    XoroshiroRandomSource r{12345};

    std::vector<int> counts(7, 0);
    for (int i = 0; i < 70000; ++i) {
        const i32 value = r.next_int(7);
        REQUIRE(value >= 0);
        REQUIRE(value < 7);
        ++counts[static_cast<usize>(value)];
    }
    for (const int count : counts) {
        REQUIRE(count > 9000);
        REQUIRE(count < 11000);
    }
}

TEST_CASE("xoroshiro's float and double draws stay in range", "[math][random]") {
    XoroshiroRandomSource r{99};
    for (int i = 0; i < 10000; ++i) {
        const f32 f = r.next_float();
        REQUIRE(f >= 0.0F);
        REQUIRE(f < 1.0F);
        const f64 d = r.next_double();
        REQUIRE(d >= 0.0);
        REQUIRE(d < 1.0);
    }
}

TEST_CASE("a bound of zero or less yields zero for xoroshiro too", "[math][random][malformed]") {
    XoroshiroRandomSource r{1};
    REQUIRE(r.next_int(0) == 0);
    REQUIRE(r.next_int(-3) == 0);
}
