# The window, the input and the game loop. The top of the graph: nothing
# depends on this, so it is the only place a window may be created.
#
# GLFW is a PRIVATE_DEP, so no header above or beside this module ever sees it.
ov_add_library(ov_client
    LAYER 15
    SOURCES
        src/container_screen.cpp
        src/creative_screen.cpp
        src/entity_renderer.cpp
        src/gui.cpp
        src/hud.cpp
        src/item_view.cpp
        src/overlay.cpp
        src/sound_director.cpp
        src/terrain_renderer.cpp
        src/window.cpp
    DEPS
        ov_rhi
        ov_render
        ov_audio
        ov_netclient
        ov_registry
    PRIVATE_DEPS
        glfw
)

# The parts that decide geometry rather than draw it: the GUI scale, the slot
# layouts, the click modes. None of them needs a device, which is exactly why
# they are constants and functions rather than something worked out while
# drawing.
ov_add_test(test_ov_client
    SOURCES
        tests/test_creative_screen.cpp
        tests/test_interface.cpp
        tests/test_sound_director.cpp
    DEPS ov_client
    LABELS unit
)

# SPIR-V, compiled by glslc at build time and written next to the executable.
# There is no shader compiler in the shipped binary; see cmake/OvShaders.cmake.
ov_add_shaders(ov_client_shaders
    SOURCES
        shaders/debug_triangle.vert
        shaders/debug_triangle.frag
        shaders/entity.vert
        shaders/entity.frag
        shaders/gui.vert
        shaders/gui.frag
        shaders/overlay.vert
        shaders/overlay.frag
        shaders/terrain.vert
        shaders/terrain.frag
)
