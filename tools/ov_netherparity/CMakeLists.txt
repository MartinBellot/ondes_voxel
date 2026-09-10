# The Nether harness: biomes, carving masks and blocks against a Nether the real
# server generated (scripts/reference_nether.sh). The block comparison goes
# through ChunkGenerator::generate(), the code path the server runs.
ov_add_executable(ov_netherparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_gameplay
        ov_protocol
        ov_io
        ov_registry
        ov_nbt
        fmt::fmt
)
