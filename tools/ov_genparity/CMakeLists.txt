# Ore parity on a world we generated ourselves, end to end.
#
# Separate from ov_features because the two answer different questions.
# ov_features hands our decoration the *game's* terrain, so its number is the
# placement's alone. This one runs the whole pipeline — noise, biomes, surface,
# carvers, features — and asks whether the resulting world holds the game's
# ores. The gap between the two numbers is what our terrain is still worth, and
# collapsing them into one would hide it.
ov_add_executable(ov_genparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_nbt
        fmt::fmt
)
