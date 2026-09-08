# The headless dedicated server.
#
# ov_add_executable refuses any dependency at or above the rendering frontier
# for this target specifically. That check is a convenience: the real guarantee
# is the linux-server-only CI job, which builds this on an image with no Vulkan
# SDK installed at all, so a forbidden include cannot compile.
ov_add_executable(ov_dedicated
    SOURCES
        src/main.cpp
    DEPS
        ov_protocol  # brings ov_io, ov_nbt, ov_world and ov_base transitively
        ov_math
        ov_registry
        ov_world
)

# Where the locally generated vanilla data lives. Passed in rather than searched
# for at runtime: the server must fail loudly with a path in the message, not
# quietly with an empty world.
target_compile_definitions(ov_dedicated PRIVATE OV_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
