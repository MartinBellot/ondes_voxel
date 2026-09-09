ov_add_library(ov_base
    LAYER 0
    SOURCES
        src/alloc_scope.cpp
        src/assert.cpp
        src/job_pool.cpp
        src/log.cpp
        src/md5.cpp
        src/resource_location.cpp
        src/thread.cpp
        src/time.cpp
    DEPS
        fmt::fmt
        Threads::Threads
)

ov_add_test(test_ov_base
    SOURCES
        tests/test_alloc_scope.cpp
        tests/test_job_pool.cpp
        tests/test_md5.cpp
        tests/test_resource_location.cpp
        tests/test_time.cpp
        tests/test_types.cpp
        tests/test_thread.cpp
    DEPS ov_base
    LABELS unit
)
