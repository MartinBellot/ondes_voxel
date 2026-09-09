# Cave edges: the blocks lining a cave, ours against the game's.
#
# The one measurement the other three cannot make. ov_carveparity compares the
# carving mask, which does not depend on the stage order at all; ov_surfparity
# hands the rules the game's own terrain and never sees a carver; ov_parity
# asks solid-or-not, and a cave wall is solid whichever block it is made of.
# This one runs both stage orders in one process and asks which of them puts the
# game's block on the skin of a cave.
ov_add_executable(ov_caveedge
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_world
        ov_nbt
        fmt::fmt
)
