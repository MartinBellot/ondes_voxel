# The Minecraft Java Edition 1.20.1 wire protocol (763).
#
# Depends on ov_world and ov_nbt for the chunk packet, which carries paletted
# containers and an NBT heightmap compound — the two formats ov_world already
# owns. Encoding them a second time here is how the disk and the wire drift.
ov_add_library(ov_protocol
    LAYER 7
    SOURCES
        src/biome_music.cpp
        src/blast.cpp
        src/breaking.cpp
        src/chat.cpp
        src/chat_types.cpp
        src/client_play.cpp
        src/effect_packets.cpp
        src/entity.cpp
        src/entity_metadata.cpp
        src/framing.cpp
        src/hud.cpp
        src/interaction.cpp
        src/listener.cpp
        src/login.cpp
        src/play.cpp
        src/recipe_packets.cpp
        src/scoreboard_packets.cpp  # ── scoreboard ──
        src/sound.cpp
        src/status.cpp
        src/survival.cpp
        src/types.cpp
        src/varint.cpp
    DEPS
        ov_io
        ov_nbt
        ov_world
    PRIVATE_DEPS
        # asio is header-only and heavily templated. Kept PRIVATE and used only
        # inside listener.cpp, so its weight never reaches a public header —
        # see risk R5 in docs/ARCHITECTURE.md.
        asio::asio
)

ov_add_test(test_ov_protocol
    SOURCES
        tests/test_biome_music.cpp
        tests/test_blast_packets.cpp
        tests/test_breaking_packets.cpp
        tests/test_chat.cpp
        tests/test_chunk_roundtrip.cpp
        tests/test_client_play.cpp
        tests/test_sound_packets.cpp
        tests/test_effect_packets.cpp
        tests/test_entities.cpp
        tests/test_entity_metadata.cpp
        tests/test_entity_packets.cpp
        tests/test_framing.cpp
        tests/test_hud_packets.cpp
        tests/test_interaction.cpp
        tests/test_login.cpp
        tests/test_scoreboard_packets.cpp  # ── scoreboard ──
        tests/test_status.cpp
        tests/test_survival.cpp
        tests/test_types.cpp
        tests/test_uuid_offline.cpp
        tests/test_varint.cpp
    DEPS ov_protocol
    LABELS unit
)

# Fixed-seed fuzzing of every decoder that reads socket bytes. A separate
# executable so the sanitizer build can compile it alone (--target), and a
# separate label so a long run (OV_FUZZ_ITERATIONS) can be selected with -L fuzz.
ov_add_test(fuzz_ov_protocol
    SOURCES
        tests/fuzz_decoders.cpp
    DEPS ov_protocol
    LABELS unit fuzz
)
