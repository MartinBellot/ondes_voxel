# The server: the tick loop, the world it owns, and the protocol it speaks.
#
# A library rather than an application, so that the client can host one on a
# thread. Nothing above the rendering frontier appears here, and the CI job
# that builds without a Vulkan SDK is what keeps it that way.
ov_add_library(ov_server
    LAYER 12
    SOURCES
        src/server.cpp
    DEPS
        ov_gameplay
        ov_io
        ov_math
        ov_nbt
        ov_protocol
        ov_registry
        ov_world
    PRIVATE_DEPS
        fmt::fmt
)

# Where the locally generated vanilla data lives. Passed in rather than searched
# for at runtime: the server must fail loudly with a path in the message, not
# quietly with an empty world.
target_compile_definitions(ov_server PRIVATE OV_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
