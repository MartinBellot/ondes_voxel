# The playable client. The counterpart of ov_dedicated, which must never reach
# across the rendering frontier; this one is the only binary that may.
ov_add_executable(ov_voxel
    SOURCES
        src/breaking.cpp  # ── breaking ──
        src/chat.cpp
        src/entities.cpp
        src/hud_script.cpp  # ── hud ──
        src/interface.cpp
        src/main.cpp
        src/menus.cpp
        src/session.cpp
        src/world_source.cpp
    DEPS
        ov_client
        ov_audio
        ov_gameplay
        ov_netclient
        ov_server
        ov_nbt
        ov_registry
        ov_world
        ov_render
        ov_rhi
        fmt::fmt
)

# The shaders have to exist before the binary is useful, and a missing .spv is
# a runtime failure rather than a link error — so the dependency is declared
# rather than left to whoever runs the build.
add_dependencies(ov_voxel ov_client_shaders)
