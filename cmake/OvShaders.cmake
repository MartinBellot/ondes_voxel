# ─────────────────────────────────────────────────────────────────────────────
# OvShaders.cmake — GLSL to SPIR-V, at build time.
#
# There is no shader compiler in the shipped binary. shaderc costs tens of
# megabytes and a slice of every start-up to solve a problem the build already
# solves, and it turns a shader syntax error into a crash on a user's machine
# rather than a red build here.
#
# glslc ships with the Vulkan SDK. If it is missing, configuring still works and
# only the shader target fails, so that a contributor without the SDK can still
# build and test everything below the rendering frontier.
# ─────────────────────────────────────────────────────────────────────────────

find_program(OV_GLSLC
    NAMES glslc
    HINTS
        $ENV{VULKAN_SDK}/bin
        /usr/local/bin
        /opt/homebrew/bin
    DOC "glslc, the GLSL to SPIR-V compiler from the Vulkan SDK")

if(OV_GLSLC)
    message(STATUS "glslc: ${OV_GLSLC}")
else()
    message(STATUS "glslc: not found — shaders will not be compiled. "
                   "Install the Vulkan SDK from https://vulkan.lunarg.com/sdk/home")
endif()

# ── ov_add_shaders(<target> SOURCES ...) ────────────────────────────────────
#
# Compiles each source to <runtime output dir>/shaders/<name>.spv and makes
# <target> depend on it. The renderer loads them by that name at run time.
function(ov_add_shaders NAME)
    cmake_parse_arguments(A "" "" "SOURCES" ${ARGN})

    if(NOT OV_GLSLC)
        add_custom_target(${NAME}
            COMMAND ${CMAKE_COMMAND} -E echo
                "ov_add_shaders(${NAME}): glslc not found; no SPIR-V was produced."
            COMMAND ${CMAKE_COMMAND} -E false)
        return()
    endif()

    set(_ov_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
    set(_ov_outputs "")

    foreach(source IN LISTS A_SOURCES)
        get_filename_component(_ov_name "${source}" NAME)
        set(_ov_spv "${_ov_output_dir}/${_ov_name}.spv")

        add_custom_command(
            OUTPUT "${_ov_spv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_ov_output_dir}"
            # -g keeps the source mapping, which is what makes a validation
            # message name a line instead of an instruction offset. Optimising
            # is left to the driver: it is the only party that knows the target.
            COMMAND ${OV_GLSLC} --target-env=vulkan1.3 -g -o "${_ov_spv}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            COMMENT "glslc ${_ov_name}"
            VERBATIM)

        list(APPEND _ov_outputs "${_ov_spv}")
    endforeach()

    add_custom_target(${NAME} ALL DEPENDS ${_ov_outputs})
    set_target_properties(${NAME} PROPERTIES FOLDER "shaders")
endfunction()
