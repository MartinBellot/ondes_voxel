# Verify the toolchain actually provides the C++23 library features used here,
# and say so clearly when it does not.
#
# Without this, a standard library that lacks <expected> produces a few hundred
# template errors starting at "no template named 'expected' in namespace 'std'"
# in whichever header happened to be compiled first — which points at our code
# rather than at the toolchain, and reads as a bug in the project.
#
# The combination that bites: clang paired with an older libstdc++. Each half is
# modern enough on its own; together they are not.

include(CheckCXXSourceCompiles)

function(ov_check_cxx23_support)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
    if(MSVC)
        set(CMAKE_REQUIRED_FLAGS "/std:c++latest")
    endif()

    check_cxx_source_compiles([[
        #include <expected>
        #include <bit>
        #include <cstdint>

        std::expected<int, const char*> probe() { return 42; }

        int main() {
            std::uint32_t value = 0x01020304u;
            return probe().has_value() ? static_cast<int>(std::byteswap(value) & 1) : 1;
        }
    ]] OV_HAVE_CXX23_LIBRARY)

    if(OV_HAVE_CXX23_LIBRARY)
        return()
    endif()

    message(FATAL_ERROR
        "\n"
        "This toolchain does not provide the C++23 library features Ondes VOXEL uses.\n"
        "\n"
        "  compiler ......... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
        "  path ............. ${CMAKE_CXX_COMPILER}\n"
        "\n"
        "Missing: std::expected and/or std::byteswap. Every parser here returns\n"
        "std::expected because the decode path is hot and must not throw, and the\n"
        "big-endian readers use std::byteswap.\n"
        "\n"
        "Known-good toolchains:\n"
        "  GCC 13 or newer\n"
        "  Clang 16 or newer with libc++       (add -stdlib=libc++)\n"
        "  Clang 16 or newer with libstdc++ 13 or newer\n"
        "  Apple Clang 15 or newer\n"
        "  MSVC 19.34 or newer\n"
        "\n"
        "The combination that usually fails is a recent clang paired with an older\n"
        "libstdc++: each half is modern enough alone, together they are not.\n"
        "On Debian or Ubuntu, either install a newer libstdc++-dev, or build with\n"
        "libc++:\n"
        "\n"
        "    sudo apt-get install -y libc++-dev libc++abi-dev\n"
        "    cmake --preset linux-debug -DCMAKE_CXX_FLAGS=-stdlib=libc++\n")
endfunction()
