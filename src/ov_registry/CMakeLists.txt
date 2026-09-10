# The block, item and entity registries.
#
# The numeric ids here are Mojang's, not ours: the vanilla client hard-codes
# them and never receives them, so a difference of one shows up as a client
# seeing the wrong blocks. They are derived from the official server jar's own
# reports by tools/ov_datagen, never maintained by hand.
ov_add_library(ov_registry
    LAYER 5
    SOURCES
        src/block_states.cpp
        src/registries.cpp
    DEPS
        ov_io
)

ov_add_test(test_ov_registry
    SOURCES
        tests/test_block_states.cpp
        tests/test_registries.cpp
        tests/test_sound_tables.cpp
    DEPS ov_registry
    LABELS unit
)
