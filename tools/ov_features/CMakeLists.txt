# Feature parity: our decoration against the world the real game wrote.
#
# Separate from ov_parity because it asks a different question. ov_parity asks
# whether the *terrain* matches; this one takes the game's own terrain as given
# and asks whether the features we put into it land on the same blocks. Mixing
# the two would make a missing carver look like a misplaced ore.
ov_add_executable(ov_features
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_nbt
        fmt::fmt
)
