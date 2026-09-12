# Sound: the catalog of events, the mixer, and the device.
#
# Layer 14, a sibling of ov_render and ignorant of it. It is under the client
# feature of vcpkg.json and under OV_BUILD_CLIENT here, so the headless server
# never compiles a line of it — principle 1 in CLAUDE.md.
#
# miniaudio (the device) and stb_vorbis (the decoder) are single-file C
# libraries. Each is compiled exactly once, in a translation unit of its own
# that includes nothing of ours, with warnings off: they are third-party code,
# and -Werror on our side must not turn their style into our build failure.
# Neither header is reachable from include/ — the engine is behind a PIMPL.
ov_add_library(ov_audio
    LAYER 14
    SOURCES
        src/sound_catalog.cpp
        src/sound_engine.cpp
        src/music.cpp
        src/third_party_miniaudio.cpp
        src/third_party_stb_vorbis.cpp
    DEPS
        ov_base
        ov_math
    PRIVATE_DEPS
        simdjson::simdjson
        # Not "ov_…": ov_check_deps treats that prefix as a layered module.
        thirdparty_audio
)

set_source_files_properties(
    src/third_party_miniaudio.cpp
    src/third_party_stb_vorbis.cpp
    PROPERTIES COMPILE_OPTIONS "-w")

ov_add_test(test_ov_audio
    SOURCES
        tests/test_sound_catalog.cpp
        tests/test_sound_engine.cpp
        tests/test_music.cpp
        tests/test_music_situation.cpp
        tests/test_sound_space.cpp
    DEPS ov_audio
    LABELS unit
)

# What 200 sounds a frame cost, with the voice cap and without (ctest -L bench).
ov_add_test(bench_ov_audio
    SOURCES
        tests/bench_sound_engine.cpp
    DEPS ov_audio
    LABELS bench
)
