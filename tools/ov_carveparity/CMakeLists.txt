# The carver harness: our carving masks against the game's own.
#
# Separate from ov_parity on purpose. ov_parity compares blocks, which mixes
# every generation stage together and can only ever say that the total got
# better. This compares the one artefact the carvers produce — the set of cells
# they cut — against the set the game recorded in the same chunk, cell by cell.
# A carver that cuts the right volume in the wrong place scores zero here and
# would have scored well there.
ov_add_executable(ov_carveparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_nbt
        fmt::fmt
)
