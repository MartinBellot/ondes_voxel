# The Vulkan wrapper. Knows nothing about the game — no block, no chunk, no
# registry — and OV_FORBID_ov_rhi lists every module to keep it that way.
#
# volk, the Vulkan headers, VMA and GLFW are all PRIVATE_DEPS, and that is the
# load-bearing part: not one of them appears in a public header, so nothing
# above this module can grow a dependency on a graphics API, and editing a
# renderer header does not rebuild Vulkan (risk R5).
#
# GLFW is here for exactly one call — glfwCreateWindowSurface. The reasoning is
# on DeviceDesc::native_window: the client owns the window, the RHI owns Vulkan,
# and a surface needs both at once. Passing a VkSurfaceKHR the other way would
# put Vulkan types in ov_client.
ov_add_library(ov_rhi
    LAYER 13
    SOURCES
        src/command_list.cpp
        src/device.cpp
        src/pipeline.cpp
        src/resources.cpp
        src/swapchain.cpp
        src/vma_impl.cpp
    DEPS
        ov_base
    PRIVATE_DEPS
        volk::volk_headers
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        glfw
)

ov_add_test(test_ov_rhi
    SOURCES
        tests/test_handle.cpp
    DEPS ov_rhi
    LABELS unit
)
