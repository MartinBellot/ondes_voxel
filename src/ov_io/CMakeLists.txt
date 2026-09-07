ov_add_library(ov_io
    LAYER 2
    SOURCES
        src/byte_reader.cpp
        src/compression.cpp
        src/file.cpp
        src/modified_utf8.cpp
        src/zip.cpp
    DEPS
        ov_base
    PRIVATE_DEPS
        # vcpkg exposes one or the other depending on the triplet: static on
        # Linux and macOS, shared on Windows. Picking one by name breaks the
        # other platform at configure time.
        $<IF:$<TARGET_EXISTS:libdeflate::libdeflate_shared>,libdeflate::libdeflate_shared,libdeflate::libdeflate_static>
)

ov_add_test(test_ov_io
    SOURCES
        tests/test_byte_reader.cpp
        tests/test_byte_writer.cpp
        tests/test_compression.cpp
        tests/test_modified_utf8.cpp
        tests/test_zip.cpp
    DEPS ov_io
    LABELS unit
)
