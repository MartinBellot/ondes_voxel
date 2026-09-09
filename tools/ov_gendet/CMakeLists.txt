# Does generating in parallel give the same world as generating in series?
#
# The one question the ChunkMap work had to answer before any speed number
# meant anything. A generator that is fast and slightly different on every run
# is not an improvement; it is a world that cannot be saved, compared or shipped.
#
# The check itself lives in ov_server, because what it checks — GeneratedWorld
# and AsyncChunkSource — is private to that module. This is the command line.
ov_add_executable(ov_gendet
    SOURCES
        src/main.cpp
    DEPS
        ov_server
        fmt::fmt
)
