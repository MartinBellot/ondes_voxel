# Surface parity: our surface rules against a world the real game wrote.
#
# Separate from ov_parity on purpose. That one asks whether the *noise* puts
# stone where the game does, and it answers about 98 %. This one asks a
# different question — given the game's own terrain, do our rules put the same
# grass, dirt, sand, gravel and terracotta on it — and mixing the two would let
# the noise's two-block height error hide every surface bug behind it.
ov_add_executable(ov_surfparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_nbt
        fmt::fmt
)
