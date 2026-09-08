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
        ov_server  # everything it does; this binary is three lines
)

# Where the locally generated vanilla data lives. Passed in rather than searched
# for at runtime: the server must fail loudly with a path in the message, not
# quietly with an empty world.
# The data directory is baked into ov_server, which is where the paths are used.
