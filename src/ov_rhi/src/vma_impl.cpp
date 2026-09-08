// The single translation unit that instantiates volk and VMA.
//
// Both are header-only libraries with an implementation switch, and both must
// be compiled exactly once. Keeping them here, alone, means the rest of the
// module's files stay cheap to rebuild — which on an 8 GB machine is the
// difference between a five-second edit cycle and a minute (risk R5).
//
// VMA is told to load Vulkan's entry points dynamically, because volk has
// already replaced the static ones with its own; letting VMA link them
// statically would call into a loader that was never initialised.
#define VOLK_IMPLEMENTATION
#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
