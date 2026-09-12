# Structure blocks: our template layer against the blocks the game wrote.
#
# ov_structparity asks *where* a structure starts; this asks what it is made of.
# Two levels, kept apart because they fail in different places:
#
#   --level=a  the game's own pieces (template, origin, rotation, mirror, as it
#              stored them in structures.starts) placed by our code, block by
#              block against the finished reference chunks;
#   --level=b  our pieces, generated from the seed alone, against the game's
#              stored pieces — no blocks involved.
ov_add_executable(ov_structblocks
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_gameplay  # ── portals ── the climate noise, for the cold test
        ov_registry
        ov_world
        ov_nbt
        fmt::fmt
)
