# Assembles run/assets/ from the player's own Minecraft installation.
# Reads archives only; links nothing from the rendering side.
ov_add_executable(ov_assetimport
    SOURCES
        src/launcher.cpp
        src/main.cpp
    DEPS
        ov_io
)
