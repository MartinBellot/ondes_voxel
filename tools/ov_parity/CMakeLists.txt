# The parity harness: our generator against a world the real game wrote.
#
# A tool rather than a test, because it needs a reference world that is
# generated locally and never committed — and because its useful output is a
# number and a list of disagreements, not a pass or a fail.
ov_add_executable(ov_parity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_nbt
        fmt::fmt
)
