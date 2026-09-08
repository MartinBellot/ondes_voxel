ov_add_library(ov_math
    LAYER 1
    SOURCES
        src/math.cpp
        src/random.cpp
    DEPS
        ov_base
)

ov_add_test(test_ov_math
    SOURCES
        tests/test_aabb.cpp
        tests/test_block_pos.cpp
        tests/test_random.cpp
        tests/test_raycast.cpp
        tests/test_vec.cpp
    DEPS ov_math
    LABELS unit
)
