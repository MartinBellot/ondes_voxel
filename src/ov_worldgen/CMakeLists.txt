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
        src/aquifer.cpp
        src/biome_source.cpp
        src/carver.cpp
        src/carving_mask.cpp
        src/chunk_generator.cpp
        src/decoration.cpp
        src/density.cpp
        src/end_features.cpp
        src/end_islands.cpp
        src/feature.cpp
        src/nether_feature.cpp  # ── nether-2 ──
        src/noise.cpp
        src/ore_feature.cpp
        # ── overworld features (features-2) ──
        src/bamboo_feature.cpp
        src/biome_info_noise.cpp
        src/cave_feature.cpp
        src/dripstone_feature.cpp
        src/huge_mushroom_feature.cpp
        src/ice_feature.cpp
        src/lush_feature.cpp
        src/noise_state_provider.cpp
        src/ocean_feature.cpp
        src/overworld_feature.cpp
        src/root_system_feature.cpp
        src/surface_feature.cpp
        src/terrain_feature.cpp
        # ── end overworld features ──
        src/pipeline.cpp
        src/placement.cpp
        src/random_factory.cpp
        src/structure.cpp
        src/structure_pieces.cpp
        src/structure_set.cpp
        src/structure_stage.cpp
        src/structure_template.cpp
        src/surface_rules.cpp
        src/surface_system.cpp
        src/tree_feature.cpp
        src/vegetation_feature.cpp
    DEPS
        ov_io
        ov_math
        ov_nbt
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
        tests/test_aquifer.cpp
        tests/test_biome_source.cpp
        tests/test_carver.cpp
        tests/test_chunk_generator.cpp
        tests/test_density.cpp
        tests/test_end.cpp
        tests/test_feature.cpp
        tests/test_nether.cpp
        tests/test_nether_feature.cpp  # ── nether-2 ──
        tests/test_noise.cpp
        tests/test_overworld_feature.cpp
        tests/test_pipeline.cpp
        tests/test_structure_set.cpp
        tests/test_structure_template.cpp
        tests/test_surface_rules.cpp
        tests/test_tree_feature.cpp
    DEPS ov_worldgen
    LABELS unit
)
