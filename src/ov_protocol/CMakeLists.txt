# The Minecraft Java Edition 1.20.1 wire protocol (763).
#
# Depends on ov_world and ov_nbt for the chunk packet, which carries paletted
# containers and an NBT heightmap compound — the two formats ov_world already
# owns. Encoding them a second time here is how the disk and the wire drift.
ov_add_library(ov_protocol
    LAYER 7
    SOURCES
        src/framing.cpp
        src/listener.cpp
        src/login.cpp
        src/play.cpp
        src/status.cpp
        src/types.cpp
        src/varint.cpp
    DEPS
        ov_io
        ov_nbt
        ov_world
    PRIVATE_DEPS
        # asio is header-only and heavily templated. Kept PRIVATE and used only
        # inside listener.cpp, so its weight never reaches a public header —
        # see risk R5 in docs/ARCHITECTURE.md.
        asio::asio
)

ov_add_test(test_ov_protocol
    SOURCES
        tests/test_framing.cpp
        tests/test_login.cpp
        tests/test_status.cpp
        tests/test_types.cpp
        tests/test_uuid_offline.cpp
        tests/test_varint.cpp
    DEPS ov_protocol
    LABELS unit
)
