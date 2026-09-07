ov_add_library(ov_base
    LAYER 0
    SOURCES
        src/assert.cpp
        src/log.cpp
        src/thread.cpp
        src/time.cpp
    DEPS
        fmt::fmt
        Threads::Threads
)

ov_add_test(test_ov_base
    SOURCES
        tests/test_time.cpp
        tests/test_types.cpp
        tests/test_thread.cpp
    DEPS ov_base
    LABELS unit
)
