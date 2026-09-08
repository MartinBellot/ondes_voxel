# The client half of the protocol, with no renderer in it.
#
# This module is the point of the layer graph and the easiest place to destroy
# it. It is a Minecraft client — it logs in, receives the world, walks around —
# and it must never see a graphics API. The CI job that builds without the
# Vulkan SDK is what proves it, and a single #include <vulkan/...> here would
# make the separation unenforceable. See risk R2.
#
# asio is PRIVATE for the same reason it is in ov_protocol: header-only and
# heavily templated, so its weight stays inside client.cpp.
ov_add_library(ov_netclient
    LAYER 12
    SOURCES
        src/client.cpp
    DEPS
        ov_protocol
        ov_registry
        ov_world
    PRIVATE_DEPS
        asio::asio
)
