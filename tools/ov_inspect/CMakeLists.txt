# Debugging tool for Minecraft's binary formats. Not part of the game; links
# only the parsing modules, never the renderer.
ov_add_executable(ov_inspect
    SOURCES
        src/main.cpp
    DEPS
        ov_nbt
        ov_world
        ov_registry
        ov_gameplay
)
