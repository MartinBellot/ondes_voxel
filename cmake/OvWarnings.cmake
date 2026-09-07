# Shared compile options. Linked PRIVATE by every ov_* target.
add_library(ov_warnings INTERFACE)

if(MSVC)
    target_compile_options(ov_warnings INTERFACE
        /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8 /EHsc
        /wd4324  # structure padded due to alignas — intentional, see false sharing
    )
    if(OV_WERROR)
        target_compile_options(ov_warnings INTERFACE /WX)
    endif()
    # /fp:fast would break determinism (risk R3).
    target_compile_options(ov_warnings INTERFACE /fp:precise)
else()
    target_compile_options(ov_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wold-style-cast
    )
    if(OV_WERROR)
        target_compile_options(ov_warnings INTERFACE -Werror)
    endif()

    # Determinism (risk R3): no reassociation, no FMA contraction, ever.
    # -ffast-math / -Ofast are forbidden project-wide, not just discouraged.
    target_compile_options(ov_warnings INTERFACE -ffp-contract=off)
endif()

# Guard against anyone reintroducing fast-math through CMAKE_CXX_FLAGS.
if(CMAKE_CXX_FLAGS MATCHES "ffast-math|Ofast|fp:fast")
    message(FATAL_ERROR
        "Determinism violation: -ffast-math / -Ofast / /fp:fast are forbidden.\n"
        "  Worldgen is a graph of double-precision density functions; a single "
        "reassociation makes terrain diverge from vanilla undetectably. See risk R3.")
endif()
