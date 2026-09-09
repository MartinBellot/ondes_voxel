# The test world, written from code.
#
# A tool and not a test: its output is a world on disk that other measurements
# are taken in, and it needs the registry, which is generated locally and never
# committed.
ov_add_executable(ov_lab
    SOURCES
        src/canvas.cpp
        src/plots.cpp
        src/main.cpp
    DEPS
        ov_world
        ov_registry
        ov_nbt
        ov_io
        fmt::fmt
)
