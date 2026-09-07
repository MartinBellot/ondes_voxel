# Sanitizer wiring. Enabled through presets, never by default.
#   -DOV_SANITIZER=address   → ASan + UBSan (the default CI sanitizer job)
#   -DOV_SANITIZER=thread    → TSan (single-writer invariant, principle 3)
#   -DOV_SANITIZER=undefined → UBSan alone

set(OV_SANITIZER "" CACHE STRING "address | thread | undefined | (empty)")
set_property(CACHE OV_SANITIZER PROPERTY STRINGS "" address thread undefined)

if(NOT OV_SANITIZER)
    return()
endif()

if(MSVC)
    if(NOT OV_SANITIZER STREQUAL "address")
        message(FATAL_ERROR "MSVC only supports OV_SANITIZER=address.")
    endif()
    add_compile_options(/fsanitize=address)
    return()
endif()

if(OV_SANITIZER STREQUAL "address")
    set(_ov_san "address,undefined")
elseif(OV_SANITIZER STREQUAL "thread")
    set(_ov_san "thread")
elseif(OV_SANITIZER STREQUAL "undefined")
    set(_ov_san "undefined")
else()
    message(FATAL_ERROR "Unknown OV_SANITIZER='${OV_SANITIZER}'.")
endif()

add_compile_options(-fsanitize=${_ov_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
add_link_options(-fsanitize=${_ov_san})
message(STATUS "Sanitizer: ${_ov_san}")
