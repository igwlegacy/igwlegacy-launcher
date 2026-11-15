# CMake toolchain file that drives `zig cc` / `zig c++` as a cross compiler.
#
#   cmake -S . -B build/win-64-release -G Ninja \
#         --toolchain cmake/ZigToolchain.cmake \
#         -DIGWL_TARGET=win-64 -DCMAKE_BUILD_TYPE=Release
#
# IGWL_TARGET is one of the platforms in targets.json. zig ships the headers,
# libc stubs and runtime libraries (libc++ included) for every target it
# supports, so one Linux host builds all of them without any extra SDK.

# CMake includes a toolchain file once per language and once more for every
# try_compile() project it spawns. Everything below is therefore idempotent,
# and the variables it depends on are forwarded to those projects explicitly.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
     IGWL_TARGET
     IGWL_ZIG_EXECUTABLE
     IGWL_ALLOW_ANY_ZIG)

if(NOT IGWL_TARGET)
    message(FATAL_ERROR
            "IGWL_TARGET is not set. Pass -DIGWL_TARGET=<one of the platforms "
            "in cmake/targets.json>.")
endif()

# The platform table lives in targets.json so that dev.py reads the very same
# values; one file parsed by both sides cannot drift apart.
file(READ "${CMAKE_CURRENT_LIST_DIR}/targets.json" _igwl_targets)

string(JSON _igwl_platform ERROR_VARIABLE _igwl_json_error
       GET "${_igwl_targets}" "platforms" "${IGWL_TARGET}")
if(NOT _igwl_json_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Unsupported IGWL_TARGET '${IGWL_TARGET}': ${_igwl_json_error}")
endif()

string(JSON CMAKE_SYSTEM_NAME      GET "${_igwl_platform}" "system")
string(JSON CMAKE_SYSTEM_PROCESSOR GET "${_igwl_platform}" "processor")
string(JSON _igwl_triple           GET "${_igwl_platform}" "triple")

# The instruction set the generated code may assume. zig already defaults to
# `baseline` when cross compiling, but for a native target it would default to
# the build machine's CPU, which makes the output depend on the hardware it
# was built on. Pinning it keeps the bytes identical on every machine.
string(JSON _igwl_cpu      GET "${_igwl_targets}" "cpu")
string(JSON _igwl_zig_want GET "${_igwl_targets}" "zig")

find_program(IGWL_ZIG_EXECUTABLE NAMES zig REQUIRED
             DOC "zig compiler used as the cross toolchain")

# A different zig release generates different code and ships a different
# libc++, so reproducible output is only possible with the exact version the
# project was locked to. pixi.lock provides it; anything else is refused
# unless explicitly allowed, e.g. while trying out a zig upgrade.
option(IGWL_ALLOW_ANY_ZIG "Accept a zig version other than the one in targets.json" OFF)
execute_process(
    COMMAND "${IGWL_ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE _igwl_zig_have
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT _igwl_zig_have STREQUAL _igwl_zig_want AND NOT IGWL_ALLOW_ANY_ZIG)
    message(FATAL_ERROR
            "${IGWL_ZIG_EXECUTABLE} is zig ${_igwl_zig_have}, but cmake/targets.json "
            "pins ${_igwl_zig_want}. Build through `./dev.py` (which uses the "
            "pixi environment) or pass -DIGWL_ALLOW_ANY_ZIG=ON to override.")
endif()

# CMake wants one executable per tool, but every zig tool is a sub-command of a
# single binary. Small generated shim scripts bridge that gap, and they are
# also where the target triple is pinned, so that it applies to compiling and
# linking alike.
set(_igwl_bin_dir "${CMAKE_BINARY_DIR}/zig-toolchain")

function(_igwl_wrapper out_var name)
    # Everything after `name` becomes the fixed argument list of the shim.
    list(JOIN ARGN " " IGWL_WRAPPER_ARGS)
    set(_output "${_igwl_bin_dir}/${name}")

    # configure_file() only rewrites the shim when its content changes, so
    # re-running CMake does not invalidate the whole build.
    configure_file("${CMAKE_CURRENT_LIST_DIR}/zig-wrapper.sh.in" "${_output}" @ONLY
                   FILE_PERMISSIONS
                   OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE)
    set(${out_var} "${_output}" PARENT_SCOPE)
endfunction()

_igwl_wrapper(_igwl_cc     zig-cc     cc  -target ${_igwl_triple} -mcpu=${_igwl_cpu})
_igwl_wrapper(_igwl_cxx    zig-cxx    c++ -target ${_igwl_triple} -mcpu=${_igwl_cpu})
_igwl_wrapper(_igwl_ar     zig-ar     ar)
_igwl_wrapper(_igwl_ranlib zig-ranlib ranlib)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # CMake enables the RC language for Windows targets and insists on a real
    # program even while there is not a single .rc file to compile.
    _igwl_wrapper(_igwl_rc zig-rc rc)
    set(CMAKE_RC_COMPILER "${_igwl_rc}" CACHE FILEPATH "zig rc" FORCE)
endif()

set(CMAKE_C_COMPILER "${_igwl_cc}")
set(CMAKE_CXX_COMPILER "${_igwl_cxx}")

# zig reports itself as clang, so CMake would otherwise go looking for a
# stand-alone llvm-ar / llvm-ranlib next to it and not find one.
set(CMAKE_AR                "${_igwl_ar}"     CACHE FILEPATH "zig ar" FORCE)
set(CMAKE_RANLIB            "${_igwl_ranlib}" CACHE FILEPATH "zig ranlib" FORCE)
set(CMAKE_C_COMPILER_AR     "${_igwl_ar}"     CACHE FILEPATH "zig ar" FORCE)
set(CMAKE_CXX_COMPILER_AR   "${_igwl_ar}"     CACHE FILEPATH "zig ar" FORCE)
set(CMAKE_C_COMPILER_RANLIB   "${_igwl_ranlib}" CACHE FILEPATH "zig ranlib" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${_igwl_ranlib}" CACHE FILEPATH "zig ranlib" FORCE)

# zig enables UBSan in debug builds and services it by linking its own runtime,
# which calls glibc functions newer than the glibc the Linux triples pin.
# Trapping keeps every check but needs no runtime, so a violation becomes an
# illegal instruction instead of a printed diagnostic. Release builds, where
# zig turns the checks off, are unaffected.
#
# Plain assignment, not append: this file is re-read once per language and
# again for every try_compile, and appending would stack the flags up.
set(CMAKE_C_FLAGS_INIT "-fsanitize-trap=undefined")
set(CMAKE_CXX_FLAGS_INIT "-fsanitize-trap=undefined")

# There is no target sysroot on disk - zig resolves the target headers and
# libraries internally. Pointing CMAKE_FIND_ROOT_PATH at an empty directory
# makes the ONLY modes below bite, so a cross build cannot pick up a host
# library from /usr, while host programs such as python still resolve.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/zig-sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_BINARY_DIR}/zig-sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
