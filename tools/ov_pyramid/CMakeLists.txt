# ── great pyramid ── Measurements of the Great Pyramid, an original Ondes VOXEL
# structure: seeds with one near the origin, density, generation cost, and the
# same area generated in several chunk orders. docs/provenance/grande-pyramide.md.
ov_add_executable(ov_pyramid
    SOURCES
        src/main.cpp
    DEPS
        ov_worldgen
        ov_registry
        ov_world
        fmt::fmt
)
