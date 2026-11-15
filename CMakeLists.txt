cmake_minimum_required(VERSION 3.25)

# C is needed for the vendored libraries (curl, mbedTLS, Monocypher).
project(igwlegacy_launcher VERSION 0.1.0 LANGUAGES C CXX)

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

# --- Launcher configuration --------------------------------------------------
#
# Compiled into the binary. Change the defaults here (dev.py does not pass
# any -D options) or override them on a plain cmake command line.

set(IGWL_BASE_URL "https://example.invalid/igwlegacy/" CACHE STRING
    "Base URL the game files are synced from; manifest.json, manifest.json.sig \
and files/ are expected below it")

# Hex encoded Ed25519 public key that manifest.json.sig must verify against.
# `tools/manifest.py keygen` creates a key pair and prints this value.
set(IGWL_MANIFEST_PUBLIC_KEY "" CACHE STRING
    "Hex encoded Ed25519 public key for verifying manifest.json")

string(LENGTH "${IGWL_MANIFEST_PUBLIC_KEY}" _igwl_key_length)
if(NOT IGWL_MANIFEST_PUBLIC_KEY MATCHES "^[0-9a-fA-F]*$"
   OR NOT (_igwl_key_length EQUAL 0 OR _igwl_key_length EQUAL 64))
    message(FATAL_ERROR "IGWL_MANIFEST_PUBLIC_KEY must be 64 hex digits (32 bytes)")
endif()
if(IGWL_MANIFEST_PUBLIC_KEY STREQUAL "")
    # Builds still succeed so CI keeps working before a key exists, but such
    # a launcher refuses to sync (it cannot verify anything) and says why.
    message(WARNING "IGWL_MANIFEST_PUBLIC_KEY is not set; the launcher will refuse "
                    "to sync. Run `tools/manifest.py keygen` and set it in CMakeLists.txt.")
endif()

configure_file(src/config.h.in "${CMAKE_BINARY_DIR}/generated/igwl_config.h" @ONLY)

# -----------------------------------------------------------------------------

include(cmake/Vendor.cmake)

add_executable(igwlegacy_launcher
    src/crypto.cpp
    src/gui.cpp
    src/http.cpp
    src/main.cpp
    src/manifest.cpp
    src/platform.cpp
    src/sync.cpp
)

set_target_properties(igwlegacy_launcher PROPERTIES
    OUTPUT_NAME igwlegacy-launcher
    # Pin the standard rather than letting the compiler default decide;
    # CXX_STANDARD_REQUIRED turns a missing -std= into an error instead of a
    # silent fallback to a newer one, and no extensions keeps it -std=c++11.
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # A GUI application on Windows: no console window pops up next to it.
    WIN32_EXECUTABLE ON
    # On Linux the X11 libraries are linked through stubs in the build tree
    # (see cmake/LinuxSysroot.cmake). A build tree RPATH pointing there would
    # embed an absolute path of the build machine, and the system libraries
    # are what has to be loaded anyway.
    SKIP_BUILD_RPATH ON
)

# The version comes from project() above, so it is part of the sources and
# changes the output only when somebody changes it on purpose.
target_compile_definitions(igwlegacy_launcher PRIVATE
    IGWL_VERSION="${PROJECT_VERSION}"
)

target_include_directories(igwlegacy_launcher PRIVATE "${CMAKE_BINARY_DIR}/generated")

if(WIN32)
    # WIN32_EXECUTABLE above makes CMake pass -mwindows, which zig accepts
    # but ignores, producing a console program that opens a terminal window
    # next to the launcher. Telling the linker directly works.
    target_link_options(igwlegacy_launcher PRIVATE "LINKER:--subsystem,windows")
endif()

target_compile_options(igwlegacy_launcher PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(igwlegacy_launcher PRIVATE
    igwl::fltk
    igwl::curl
    igwl::mbedtls
    igwl::monocypher
    igwl::json
    igwl::filesystem
)

# The vendored libraries come with install rules of their own (headers,
# static libraries, CMake package files), none of which belong in a release.
# Only the `launcher` component is installed by dev.py.
install(TARGETS igwlegacy_launcher RUNTIME DESTINATION bin COMPONENT launcher)
