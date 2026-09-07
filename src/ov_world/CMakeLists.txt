# Chunk storage, lighting and the abstract level interfaces.
#
# Knows nothing about the tick, the network or EnTT. That ignorance is what lets
# ov_sim (the authoritative world) and ov_netclient (the client's replica) share
# the same chunk storage instead of maintaining two of them — see the forbidden
# edges in cmake/OvModule.cmake.
ov_add_library(ov_world
    LAYER 6
    SOURCES
        src/paletted_container.cpp
    DEPS
        ov_registry
        ov_math
)

ov_add_test(test_ov_world
    SOURCES
        tests/test_paletted_container.cpp
    DEPS ov_world
    LABELS unit
)
