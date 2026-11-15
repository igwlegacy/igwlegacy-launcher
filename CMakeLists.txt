cmake_minimum_required(VERSION 3.25)

project(igwlegacy_launcher VERSION 0.1.0 LANGUAGES CXX)

# Single-config generators default to an unoptimised, undefined build type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Collect everything in one place per build directory so that dev.py and
# `cmake --install` have predictable paths to work with.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Reproducibility ---------------------------------------------------------
#
# The release artifacts are meant to be bit-identical no matter who builds
# them, where the checkout lives or when the build runs. The settings below
# remove the inputs that would otherwise leak into the output.

# Absolute paths reach the binary through __FILE__ (assert, logging macros)
# and through debug info. Rewriting both the source and the build directory
# prefixes makes them relative to the tree instead of to the machine.
add_compile_options(
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/="
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}/="
)

# __DATE__ and __TIME__ differ on every build. Turning their use into an error
# keeps them from ever sneaking in; a version or date to embed has to come
# from the sources (see IGWL_VERSION below) instead.
add_compile_options(-Werror=date-time)

# zig links its libc++ and libc++abi with debug info even in optimised builds,
# and that debug info carries the absolute path of the zig installation, i.e.
# of the pixi environment inside this checkout. Stripping release binaries
# removes it, along with the symbol table nobody ships anyway. Debug and
# RelWithDebInfo keep their symbols and are only reproducible on the same path.
add_link_options("$<$<CONFIG:Release,MinSizeRel>:-s>")

# -----------------------------------------------------------------------------

add_executable(igwlegacy_launcher
    src/main.cpp
)

set_target_properties(igwlegacy_launcher PROPERTIES
    OUTPUT_NAME igwlegacy-launcher
    # Pin the standard rather than letting the compiler default decide;
    # CXX_STANDARD_REQUIRED turns a missing -std= into an error instead of a
    # silent fallback to a newer one, and no extensions keeps it -std=c++11.
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# The version comes from project() above, so it is part of the sources and
# changes the output only when somebody changes it on purpose.
target_compile_definitions(igwlegacy_launcher PRIVATE
    IGWL_VERSION="${PROJECT_VERSION}"
)

target_compile_options(igwlegacy_launcher PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS igwlegacy_launcher RUNTIME DESTINATION bin)
