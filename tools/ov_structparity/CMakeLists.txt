# Structure parity: where the game started a structure, and where we would.
#
# Separate from the three that exist because it is the only one whose oracle is
# a boolean rather than a field. ov_parity asks whether the noise puts stone
# where the game does and answers about 98 %; this one asks a question that has
# no percentage in it — a chunk starts a mineshaft or it does not — and mixing
# the two would let a wrong salt hide behind the noise's height error.
ov_add_executable(ov_structparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_nbt
        fmt::fmt
)
