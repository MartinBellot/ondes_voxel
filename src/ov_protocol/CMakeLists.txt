# The Minecraft Java Edition 1.20.1 wire protocol (763).
#
# Depends on ov_io only for now: VarInt and framing need nothing else. Registry
# and world dependencies arrive with the packets that carry them.
ov_add_library(ov_protocol
    LAYER 7
    SOURCES
        src/framing.cpp
        src/listener.cpp
        src/status.cpp
        src/types.cpp
        src/varint.cpp
    DEPS
        ov_io
    PRIVATE_DEPS
        # asio is header-only and heavily templated. Kept PRIVATE and used only
        # inside listener.cpp, so its weight never reaches a public header —
        # see risk R5 in docs/ARCHITECTURE.md.
        asio::asio
)

ov_add_test(test_ov_protocol
    SOURCES
        tests/test_framing.cpp
        tests/test_status.cpp
        tests/test_types.cpp
        tests/test_varint.cpp
    DEPS ov_protocol
    LABELS unit
)
