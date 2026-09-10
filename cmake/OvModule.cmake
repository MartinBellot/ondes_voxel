# ─────────────────────────────────────────────────────────────────────────────
# OvModule.cmake — layering enforcement (lock #1 of 3)
#
# Ondes VOXEL is layered. A module may depend ONLY on modules whose layer is
# STRICTLY LOWER, and never on a module listed in its own forbidden set.
#
# The layer number alone is not enough: ov_netclient and ov_sim are siblings by
# intent, but a plain "<" comparison would happily let ov_netclient reach into
# ov_server's world. The forbidden sets encode the rules that ordering cannot.
#
# This is lock #1. The other two live elsewhere and are what actually make the
# rule survive contact with a tired afternoon:
#   #2  scripts/check_layers.py   — parses real #include edges, not declarations
#   #3  CI job linux-server-only  — builds with no Vulkan SDK present at all,
#                                   so a forbidden include cannot even compile
# ─────────────────────────────────────────────────────────────────────────────

# ── Layer assignment ────────────────────────────────────────────────────────
# Siblings may share a number: they never depend on each other, and "strictly
# lower" is exactly what forbids them from doing so.
set(OV_LAYER_ov_base       0)
set(OV_LAYER_ov_math       1)
set(OV_LAYER_ov_io         2)
set(OV_LAYER_ov_nbt        3)
set(OV_LAYER_ov_data       4)
set(OV_LAYER_ov_registry   5)
set(OV_LAYER_ov_world      6)
set(OV_LAYER_ov_protocol   7)
set(OV_LAYER_ov_entity     8)
set(OV_LAYER_ov_gameplay   9)
set(OV_LAYER_ov_worldgen  10)
set(OV_LAYER_ov_sim       11)
set(OV_LAYER_ov_server    12)
set(OV_LAYER_ov_netclient 12)
# ───────────────────────── rendering frontier ───────────────────────────────
set(OV_LAYER_ov_rhi       13)
set(OV_LAYER_ov_render    14)
set(OV_LAYER_ov_audio     14)
set(OV_LAYER_ov_client    15)

set(OV_ALL_MODULES
    ov_base ov_math ov_io ov_nbt ov_data ov_registry ov_world ov_protocol
    ov_entity ov_gameplay ov_worldgen ov_sim ov_server ov_netclient
    ov_rhi ov_render ov_audio ov_client)

# Everything at or above this layer is rendering. ov_dedicated must not reach it.
set(OV_RENDER_FRONTIER 13)

# ── Forbidden edges the layer order cannot express ──────────────────────────

# The replicated client world must never touch the authoritative one. Without
# this, "just for now, the renderer reads serverLevel->getChunk()" happens, and
# six months later the dedicated server is a fork. See principle 2 in CLAUDE.md.
set(OV_FORBID_ov_netclient ov_sim ov_server)

# Block behaviour takes a LevelWriter&, never a ServerLevel&. The moment
# ov_gameplay knows about the server, client-side prediction becomes impossible.
set(OV_FORBID_ov_gameplay  ov_sim ov_server ov_netclient)
set(OV_FORBID_ov_entity    ov_sim ov_server ov_netclient)

# ov_world is the shared chunk storage of BOTH worlds. It must stay ignorant of
# ticking, networking and EnTT so that ov_sim and ov_netclient can share it.
set(OV_FORBID_ov_world     ov_protocol ov_entity ov_gameplay)

# ov_rhi knows nothing about the game; ov_render knows nothing about windowing.
#
# "Nothing about the game" was first written as ${OV_ALL_MODULES}, which also
# forbade ov_base — and ov_base is not the game. It is fixed-width integers,
# OV_ASSERT, and the log the Vulkan validation layers have to report through.
# Forbidding it bought nothing and cost either a second set of integer aliases
# or a renderer that cannot say why it failed to start.
#
# So ov_base is the single exception, and the list is written out rather than
# computed: scripts/check_layers.py reads this line literally, and a
# list(REMOVE_ITEM) after it would leave the two locks disagreeing about what
# is allowed — which is worse than either rule on its own.
set(OV_FORBID_ov_rhi
    ov_math ov_io ov_nbt ov_data ov_registry ov_world ov_protocol
    ov_entity ov_gameplay ov_worldgen ov_sim ov_server ov_netclient
    ov_render ov_audio ov_client)
set(OV_FORBID_ov_render    ov_client ov_server ov_sim ov_netclient)
# ov_audio plays what it is told to, at a position it is given. It knows sounds,
# not packets, blocks or windows: the network half and the game half stay above
# it in ov_client, which is what lets the null backend be tested without a
# server and without a sound card.
set(OV_FORBID_ov_audio     ov_client ov_server ov_sim ov_netclient ov_render ov_rhi)

# ── ov_check_deps(<module> <deps...>) ───────────────────────────────────────
function(ov_check_deps NAME)
    if(NOT DEFINED OV_LAYER_${NAME})
        message(FATAL_ERROR
            "LAYERING: module '${NAME}' has no layer. Declare OV_LAYER_${NAME} in "
            "cmake/OvModule.cmake and document the choice in docs/ARCHITECTURE.md.")
    endif()

    foreach(dep IN LISTS ARGN)
        # Third-party targets are not layered; only ov_* edges are checked here.
        if(NOT dep MATCHES "^ov_")
            continue()
        endif()

        if(NOT DEFINED OV_LAYER_${dep})
            message(FATAL_ERROR "LAYERING: '${NAME}' depends on unknown module '${dep}'.")
        endif()

        if(NOT OV_LAYER_${dep} LESS OV_LAYER_${NAME})
            message(FATAL_ERROR
                "LAYERING VIOLATION: ${NAME}(L${OV_LAYER_${NAME}}) -> ${dep}(L${OV_LAYER_${dep}})\n"
                "  A module may only depend on strictly lower layers.\n"
                "  If this edge is genuinely needed, the layering is wrong: fix the design or "
                "write an ADR in docs/adr/ before touching this file.")
        endif()

        if(dep IN_LIST OV_FORBID_${NAME})
            message(FATAL_ERROR
                "LAYERING VIOLATION: ${NAME} -> ${dep} is explicitly forbidden.\n"
                "  See the rationale next to OV_FORBID_${NAME} in cmake/OvModule.cmake.")
        endif()
    endforeach()
endfunction()

# ── ov_add_library(<name> LAYER <n> SOURCES ... [DEPS ...] [PRIVATE_DEPS ...]) ─
#
# The ONLY way to declare a module. Calling target_link_libraries directly under
# src/ is a CI failure (scripts/check_layers.py greps for it).
#
# Headers live in <module>/include/<module>/ and are PUBLIC.
# Everything under <module>/src/ is PRIVATE, so a private header is physically
# unreachable from another module.
function(ov_add_library NAME)
    cmake_parse_arguments(A "" "LAYER" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})

    if(NOT DEFINED A_LAYER)
        message(FATAL_ERROR "ov_add_library(${NAME}): LAYER is required.")
    endif()
    if(NOT A_LAYER EQUAL OV_LAYER_${NAME})
        message(FATAL_ERROR
            "ov_add_library(${NAME}): declared LAYER ${A_LAYER} but OvModule.cmake says "
            "${OV_LAYER_${NAME}}. The registry in OvModule.cmake is the source of truth.")
    endif()
    if(NOT A_SOURCES)
        message(FATAL_ERROR "ov_add_library(${NAME}): SOURCES is required.")
    endif()

    ov_check_deps(${NAME} ${A_DEPS} ${A_PRIVATE_DEPS})

    add_library(${NAME} STATIC ${A_SOURCES})
    # ov_world -> ov::world. Not bash: a '#' inside ${} would open a CMake
    # comment and swallow the rest of the function.
    string(REGEX REPLACE "^ov_" "" _ov_short "${NAME}")
    add_library(ov::${_ov_short} ALIAS ${NAME})

    target_include_directories(${NAME}
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(${NAME}
        PUBLIC  ${A_DEPS}
        PRIVATE ${A_PRIVATE_DEPS} ov_warnings)

    set_target_properties(${NAME} PROPERTIES
        FOLDER "modules"
        OV_LAYER ${A_LAYER}
        POSITION_INDEPENDENT_CODE ON)

    # Determinism: worldgen and anything feeding it must not be reassociated or
    # contracted by the compiler. One FMA contraction and the terrain diverges
    # from vanilla in a way that is invisible until you compare it by eye.
    # See risk R3 in docs/ARCHITECTURE.md.
    if(NOT MSVC)
        target_compile_options(${NAME} PRIVATE -ffp-contract=off)
    else()
        target_compile_options(${NAME} PRIVATE /fp:precise)
    endif()
endfunction()

# ── ov_add_executable(<name> SOURCES ... [DEPS ...]) ────────────────────────
function(ov_add_executable NAME)
    cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${NAME} ${A_SOURCES})
    target_link_libraries(${NAME} PRIVATE ${A_DEPS} ov_warnings)
    set_target_properties(${NAME} PROPERTIES FOLDER "apps")

    # ov_dedicated must never reach across the rendering frontier, transitively
    # or otherwise. This is a declared-graph check; the real guarantee is the
    # linux-server-only CI job, which has no Vulkan SDK installed at all.
    if(NAME STREQUAL "ov_dedicated")
        foreach(dep IN LISTS A_DEPS)
            if(dep MATCHES "^ov_" AND DEFINED OV_LAYER_${dep}
               AND NOT OV_LAYER_${dep} LESS OV_RENDER_FRONTIER)
                message(FATAL_ERROR
                    "HEADLESS VIOLATION: ov_dedicated -> ${dep}(L${OV_LAYER_${dep}}).\n"
                    "  The dedicated server must build with zero rendering dependencies.")
            endif()
        endforeach()
    endif()
endfunction()

# ── ov_add_test(<name> SOURCES ... [DEPS ...] [LABELS ...]) ─────────────────
function(ov_add_test NAME)
    if(NOT OV_BUILD_TESTS)
        return()
    endif()
    cmake_parse_arguments(A "" "" "SOURCES;DEPS;LABELS" ${ARGN})
    add_executable(${NAME} ${A_SOURCES})
    target_link_libraries(${NAME} PRIVATE ${A_DEPS} Catch2::Catch2WithMain ov_warnings)
    set_target_properties(${NAME} PROPERTIES FOLDER "tests")
    # Tests that read generated data need to find it regardless of where the
    # build directory is.
    target_compile_definitions(${NAME} PRIVATE OV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${NAME} COMMAND ${NAME})
    if(A_LABELS)
        set_tests_properties(${NAME} PROPERTIES LABELS "${A_LABELS}")
    endif()
endfunction()
