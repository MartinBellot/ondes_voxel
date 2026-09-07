# The Minecraft Java Edition 1.20.1 wire protocol (763).
#
# Depends on ov_io only for now: VarInt and framing need nothing else. Registry
# and world dependencies arrive with the packets that carry them.
ov_add_library(ov_protocol
    LAYER 7
    SOURCES
        src/framing.cpp
        src/types.cpp
        src/varint.cpp
    DEPS
        ov_io
)

ov_add_test(test_ov_protocol
    SOURCES
        tests/test_framing.cpp
        tests/test_types.cpp
        tests/test_varint.cpp
    DEPS ov_protocol
    LABELS unit
)
