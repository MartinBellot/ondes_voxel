# Runs the block model pipeline over a real resource pack and counts what came
# out. Not part of the game; the assets it reads are never committed.
ov_add_executable(ov_modelbake
    SOURCES
        src/main.cpp
    DEPS
        ov_render
        fmt::fmt
)
