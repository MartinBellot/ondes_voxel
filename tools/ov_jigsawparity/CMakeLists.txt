# Jigsaw parity: the pieces we grow from the seed against the pieces the game
# stored in `structures.starts` — element, position, rotation, box, ground
# delta, junctions — for every jigsaw start of a reference world, finished or
# not (the pieces are decided long before the blocks).
ov_add_executable(ov_jigsawparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_world
        ov_nbt
        fmt::fmt
)
