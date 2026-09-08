ov_add_library(ov_nbt
    LAYER 3
    SOURCES
        src/binary.cpp
        src/region.cpp
        src/region_writer.cpp
        src/tag.cpp
    DEPS
        ov_io
        ov_math
)

ov_add_test(test_ov_nbt
    SOURCES
        tests/test_binary.cpp
        tests/test_region.cpp
        tests/test_region_writer.cpp
        tests/test_tag.cpp
    DEPS ov_nbt
    LABELS unit
)
