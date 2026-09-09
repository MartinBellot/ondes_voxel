# Terrain generation, bit-for-bit with the game for a given seed.
#
# Everything here is sensitive to the last bit of a double: the point of seed
# parity is that the same seed produces the same world, and a rounding
# difference in the noise moves every hill. The build already forbids
# -ffast-math and sets -ffp-contract=off; this module is the reason those rules
# exist.
ov_add_library(ov_worldgen
    LAYER 10
    SOURCES
        src/biome_source.cpp
        src/carver.cpp
        src/carving_mask.cpp
        src/chunk_generator.cpp
        src/decoration.cpp
        src/density.cpp
        src/feature.cpp
        src/noise.cpp
        src/ore_feature.cpp
        src/placement.cpp
        src/surface_rules.cpp
        src/surface_system.cpp
    DEPS
        ov_math
        ov_registry
        ov_world
    PRIVATE_DEPS
        # Same rule as everywhere else: simdjson is a template-heavy header, so
        # it appears in the .cpp files that read the datapack and in no
        # installed one.
        simdjson::simdjson
)

ov_add_test(test_ov_worldgen
    SOURCES
        tests/test_biome_source.cpp
        tests/test_carver.cpp
        tests/test_density.cpp
        tests/test_feature.cpp
        tests/test_noise.cpp
        tests/test_surface_rules.cpp
    DEPS ov_worldgen
    LABELS unit
)
