# The End harness: biomes, blocks and the spikes against an End the real server
# generated (scripts/reference_end.sh). The block comparison goes through
# ChunkGenerator::generate(), and the finished chunks through the pipeline and
# the End's decorator — the code paths the server runs.
ov_add_executable(ov_endparity
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_io
        ov_registry
        ov_nbt
        fmt::fmt
)
