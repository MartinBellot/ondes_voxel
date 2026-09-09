# Chunk storage, lighting and the abstract level interfaces.
#
# Knows nothing about the tick, the network or EnTT. That ignorance is what lets
# ov_sim (the authoritative world) and ov_netclient (the client's replica) share
# the same chunk storage instead of maintaining two of them — see the forbidden
# edges in cmake/OvModule.cmake.
ov_add_library(ov_world
    LAYER 6
    SOURCES
        src/block_ticks.cpp
        src/chunk.cpp
        src/chunk_map.cpp
        src/chunk_section.cpp
        src/chunk_storage.cpp
        src/heightmap.cpp
        src/level_dat.cpp
        src/light_array.cpp
        src/paletted_container.cpp
    DEPS
        ov_nbt
        ov_registry
        ov_math
)

ov_add_test(test_ov_world
    SOURCES
        tests/test_block_ticks.cpp
        tests/test_chunk.cpp
        tests/test_chunk_map.cpp
        tests/test_chunk_section.cpp
        tests/test_chunk_storage.cpp
        tests/test_heightmap.cpp
        tests/test_paletted_container.cpp
    DEPS ov_world
    LABELS unit
)
