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
        ov_protocol  # brings ov_io, ov_math and ov_base transitively
        ov_math
)
