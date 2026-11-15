# Builds the third-party libraries from the git submodules in vendor/ and
# exposes them as targets:
#
#   igwl::fltk        FLTK 1.4 (X11 + Xft on Linux, GDI+ on Windows)
#   igwl::curl        static libcurl, HTTP(S) only
#                     (TLS: Schannel on Windows, mbedTLS on Linux)
#   igwl::mbedtls     mbedTLS (SHA-256 everywhere, TLS for curl on Linux)
#   igwl::monocypher  Monocypher incl. the optional Ed25519 (RFC 8032) module
#   igwl::json        nlohmann/json (header only)
#   igwl::filesystem  ghc::filesystem, a C++11 std::filesystem (header only)
#
# Every option a subproject reads is pinned here with a FORCE'd cache entry,
# so a configure never depends on a default that may change between library
# releases or on whatever a previous configure left in the cache.

include_guard(GLOBAL)

set(IGWL_VENDOR_DIR "${PROJECT_SOURCE_DIR}/vendor")

# Pins a subproject option. INTERNAL implies FORCE and hides it from the
# cache editors, as these are not meant to be tweaked by hand.
macro(igwl_pin name value)
    set(${name} "${value}" CACHE INTERNAL "pinned by cmake/Vendor.cmake")
endmacro()

# A missing submodule shows up as an empty directory; point at the fix
# instead of letting CMake fail somewhere deep inside a subproject.
foreach(_igwl_file
        fltk/CMakeLists.txt
        curl/CMakeLists.txt
        mbedtls/library/sha256.c
        json/single_include/nlohmann/json.hpp
        filesystem/include/ghc/filesystem.hpp
        monocypher/src/monocypher.c
        xorgproto/include/X11/X.h
        libx11/include/X11/Xlib.h
        libxrender/include/X11/extensions/Xrender.h
        libxft/include/X11/Xft/Xft.h.in
        fontconfig/fontconfig/fontconfig.h.in
        freetype/include/ft2build.h)
    if(NOT EXISTS "${IGWL_VENDOR_DIR}/${_igwl_file}")
        message(FATAL_ERROR "vendor/${_igwl_file} is missing. "
                "Run `git submodule update --init` to fetch the vendored libraries.")
    endif()
endforeach()

# Third-party code is compiled with its own warnings silenced; they are not
# ours to fix and would only bury the project's own diagnostics.
set(_igwl_quiet -w)

# Static libraries everywhere, so the launcher is a single executable.
igwl_pin(BUILD_SHARED_LIBS OFF)

# --- Linux: X11/Xft headers and link stubs -----------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include("${CMAKE_CURRENT_LIST_DIR}/LinuxSysroot.cmake")
endif()

# --- mbedTLS -----------------------------------------------------------------
#
# mbedTLS's own CMake project refuses to configure without its `framework`
# submodule, which is only needed for its tests and code generators (the
# generated sources are committed in release tags). Compiling library/*.c
# directly avoids vendoring that as well.

file(GLOB _igwl_mbedtls_sources CONFIGURE_DEPENDS "${IGWL_VENDOR_DIR}/mbedtls/library/*.c")
add_library(igwl_mbedtls STATIC ${_igwl_mbedtls_sources})
target_include_directories(igwl_mbedtls SYSTEM PUBLIC "${IGWL_VENDOR_DIR}/mbedtls/include")
target_include_directories(igwl_mbedtls PRIVATE "${IGWL_VENDOR_DIR}/mbedtls/library")
target_compile_options(igwl_mbedtls PRIVATE ${_igwl_quiet})
if(WIN32)
    # The entropy source (BCryptGenRandom) and net_sockets.c.
    target_link_libraries(igwl_mbedtls PUBLIC bcrypt ws2_32)
endif()
add_library(igwl::mbedtls ALIAS igwl_mbedtls)

# --- libcurl -----------------------------------------------------------------

igwl_pin(BUILD_STATIC_LIBS ON)
igwl_pin(BUILD_CURL_EXE OFF)
igwl_pin(BUILD_TESTING OFF)
igwl_pin(BUILD_EXAMPLES OFF)
igwl_pin(BUILD_LIBCURL_DOCS OFF)
igwl_pin(BUILD_MISC_DOCS OFF)
igwl_pin(ENABLE_CURL_MANUAL OFF)
igwl_pin(CURL_DISABLE_INSTALL ON)
igwl_pin(CURL_ENABLE_EXPORT_TARGET OFF)
igwl_pin(PICKY_COMPILER OFF)
# Only HTTP(S) is needed; every other protocol is dead weight and attack
# surface.
igwl_pin(HTTP_ONLY ON)
igwl_pin(CURL_DISABLE_WEBSOCKETS ON)
igwl_pin(CURL_DISABLE_ALTSVC ON)
igwl_pin(CURL_DISABLE_HSTS ON)
igwl_pin(CURL_DISABLE_NETRC ON)
igwl_pin(CURL_DISABLE_COOKIES ON)
# Never look for anything on the build machine.
igwl_pin(CURL_USE_PKGCONFIG OFF)
igwl_pin(CURL_USE_CMAKECONFIG OFF)
# Optional dependencies that are not vendored.
igwl_pin(CURL_USE_LIBPSL OFF)
igwl_pin(CURL_USE_LIBSSH2 OFF)
igwl_pin(CURL_USE_LIBSSH OFF)
igwl_pin(CURL_USE_GSSAPI OFF)
igwl_pin(CURL_ZLIB OFF)
igwl_pin(CURL_BROTLI OFF)
igwl_pin(CURL_ZSTD OFF)
igwl_pin(USE_NGHTTP2 OFF)
igwl_pin(USE_LIBIDN2 OFF)
igwl_pin(USE_WIN32_IDN OFF)
igwl_pin(ENABLE_ARES OFF)
igwl_pin(CURL_USE_OPENSSL OFF)
igwl_pin(CURL_USE_WOLFSSL OFF)
igwl_pin(CURL_USE_GNUTLS OFF)
igwl_pin(CURL_USE_RUSTLS OFF)

if(WIN32)
    # Schannel verifies against the Windows certificate store, so there is no
    # CA bundle to ship or keep up to date.
    igwl_pin(CURL_USE_SCHANNEL ON)
    igwl_pin(CURL_USE_MBEDTLS OFF)
    igwl_pin(ENABLE_UNICODE ON)
    # HTTP_ONLY disables the LDAP protocol, yet curl still links wldap32
    # unless told otherwise.
    igwl_pin(USE_WIN32_LDAP OFF)
else()
    igwl_pin(CURL_USE_MBEDTLS ON)
    # Handed over directly instead of being searched for. Target names work
    # as link items, and the header directory gives curl the version check.
    igwl_pin(MBEDTLS_INCLUDE_DIR "${IGWL_VENDOR_DIR}/mbedtls/include")
    igwl_pin(MBEDTLS_LIBRARY igwl_mbedtls)
    igwl_pin(MBEDX509_LIBRARY igwl_mbedtls)
    igwl_pin(MBEDCRYPTO_LIBRARY igwl_mbedtls)
    # curl would check this by linking a test program against the libraries
    # above, which do not exist yet at configure time. DES is only used for
    # NTLM, which is disabled anyway.
    igwl_pin(HAVE_MBEDTLS_DES_CRYPT_ECB 0)
    # The CA bundle location differs between distributions; the launcher
    # looks for it at runtime (see src/platform.cpp) instead of hard-coding
    # the build machine's path.
    igwl_pin(CURL_CA_BUNDLE none)
    igwl_pin(CURL_CA_PATH none)
endif()

add_subdirectory("${IGWL_VENDOR_DIR}/curl" "${CMAKE_BINARY_DIR}/vendor/curl" EXCLUDE_FROM_ALL SYSTEM)
target_compile_options(libcurl_static PRIVATE ${_igwl_quiet})
add_library(igwl::curl ALIAS libcurl_static)

# --- FLTK --------------------------------------------------------------------

igwl_pin(FLTK_BUILD_SHARED_LIBS OFF)
igwl_pin(FLTK_BUILD_TEST OFF)
igwl_pin(FLTK_BUILD_EXAMPLES OFF)
igwl_pin(FLTK_BUILD_FLUID OFF)
igwl_pin(FLTK_BUILD_FLTK_OPTIONS OFF)
igwl_pin(FLTK_BUILD_FORMS OFF)
igwl_pin(FLTK_BUILD_GL OFF)
igwl_pin(FLTK_BUILD_HTML_DOCS OFF)
igwl_pin(FLTK_BUILD_PDF_DOCS OFF)
igwl_pin(FLTK_OPTION_PRINT_SUPPORT OFF)
igwl_pin(FLTK_OPTION_SVG OFF)
igwl_pin(FLTK_OPTION_CAIRO_WINDOW OFF)
igwl_pin(FLTK_OPTION_CAIRO_EXT OFF)
# The image libraries are bundled with FLTK; only built if ever linked.
igwl_pin(FLTK_USE_SYSTEM_LIBJPEG OFF)
igwl_pin(FLTK_USE_SYSTEM_LIBPNG OFF)
igwl_pin(FLTK_USE_SYSTEM_ZLIB OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # X11 only: Wayland would need libwayland, libxkbcommon, libdecor, cairo
    # and pango, none of which are vendored. X11 works under XWayland too.
    igwl_pin(FLTK_BACKEND_WAYLAND OFF)
    igwl_pin(FLTK_BACKEND_X11 ON)
    igwl_pin(FLTK_USE_PANGO OFF)
    igwl_pin(FLTK_GRAPHICS_CAIRO OFF)
    # Antialiased text through Xft/fontconfig, with Xrender underneath.
    igwl_pin(FLTK_USE_XFT ON)
    igwl_pin(FLTK_USE_XRENDER ON)
    # Not vendored; FLTK works without them (single monitor geometry from
    # the core protocol, core cursors, no clipboard change notification).
    igwl_pin(FLTK_USE_XINERAMA OFF)
    igwl_pin(FLTK_USE_XFIXES OFF)
    igwl_pin(FLTK_USE_XCURSOR OFF)
endif()

# FLTK's install rules would export its include directories, among them the
# stub sysroot inside the build tree, which CMake rejects. Nothing of FLTK is
# installed anyway, so its rules are not generated at all; the variable is
# evaluated per directory, so the launcher's own install rule is unaffected.
set(CMAKE_SKIP_INSTALL_RULES ON)
add_subdirectory("${IGWL_VENDOR_DIR}/fltk" "${CMAKE_BINARY_DIR}/vendor/fltk" EXCLUDE_FROM_ALL SYSTEM)
unset(CMAKE_SKIP_INSTALL_RULES)
target_compile_options(fltk PRIVATE ${_igwl_quiet})
add_library(igwl::fltk ALIAS fltk)
# FLTK adds its own output directory (vendor/fltk/lib) as a link directory
# for everything linking it. The libraries end up in ${CMAKE_BINARY_DIR}/lib
# instead, so the directory would never exist and the linker warns about it
# on every link; an empty directory keeps the warning away.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/vendor/fltk/lib")

# --- Monocypher --------------------------------------------------------------

add_library(igwl_monocypher STATIC
    "${IGWL_VENDOR_DIR}/monocypher/src/monocypher.c"
    "${IGWL_VENDOR_DIR}/monocypher/src/optional/monocypher-ed25519.c"
)
target_include_directories(igwl_monocypher SYSTEM PUBLIC
    "${IGWL_VENDOR_DIR}/monocypher/src"
    "${IGWL_VENDOR_DIR}/monocypher/src/optional"
)
target_compile_options(igwl_monocypher PRIVATE ${_igwl_quiet})
add_library(igwl::monocypher ALIAS igwl_monocypher)

# --- Header-only libraries ---------------------------------------------------

add_library(igwl_json INTERFACE)
target_include_directories(igwl_json SYSTEM INTERFACE "${IGWL_VENDOR_DIR}/json/single_include")
add_library(igwl::json ALIAS igwl_json)

add_library(igwl_filesystem INTERFACE)
target_include_directories(igwl_filesystem SYSTEM INTERFACE "${IGWL_VENDOR_DIR}/filesystem/include")
add_library(igwl::filesystem ALIAS igwl_filesystem)
