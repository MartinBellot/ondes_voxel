# The playable client. The counterpart of ov_dedicated, which must never reach
# across the rendering frontier; this one is the only binary that may.
ov_add_executable(ov_voxel
    SOURCES
        src/main.cpp
    DEPS
        ov_client
        ov_render
        ov_rhi
        fmt::fmt
)

# The shaders have to exist before the binary is useful, and a missing .spv is
# a runtime failure rather than a link error — so the dependency is declared
# rather than left to whoever runs the build.
add_dependencies(ov_voxel ov_client_shaders)
