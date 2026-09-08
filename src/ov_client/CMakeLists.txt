# The window, the input and the game loop. The top of the graph: nothing
# depends on this, so it is the only place a window may be created.
#
# GLFW is a PRIVATE_DEP, so no header above or beside this module ever sees it.
ov_add_library(ov_client
    LAYER 15
    SOURCES
        src/window.cpp
    DEPS
        ov_rhi
        ov_render
    PRIVATE_DEPS
        glfw
)

# SPIR-V, compiled by glslc at build time and written next to the executable.
# There is no shader compiler in the shipped binary; see cmake/OvShaders.cmake.
ov_add_shaders(ov_client_shaders
    SOURCES
        shaders/debug_triangle.vert
        shaders/debug_triangle.frag
        shaders/terrain.vert
        shaders/terrain.frag
)
