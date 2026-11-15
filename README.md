# igwlegacy-launcher

A C++11 project that cross-compiles from Linux to Linux and Windows, using
**zig** as the cross toolchain and **CMake** as the build system. Release
builds are **bit-for-bit reproducible**: the same sources give the same
bytes on any machine, from any checkout path, at any time.

Nothing beyond [pixi](https://pixi.sh) has to be installed: it provides the
exact cmake, ninja, python and zig versions recorded in `pixi.lock`.

## Quick start

```sh
./dev.py build              # debug build for the host platform
./dev.py build -r -a        # release build for every supported platform
./dev.py build -t win-64    # debug build for 64 bit Windows only
./dev.py build -t           # list the platforms -t accepts
./dev.py run -r             # build for the host and run the binary
./dev.py run -t win-64      # build for Windows and run it through wine
./dev.py dist               # release build of everything into ./dist
./dev.py repro              # dist, then check an independent build matches
./dev.py clean              # remove build/ and dist/
```

`dev.py` always re-executes itself through `pixi run --locked`, even if a
cmake, ninja or zig is installed system-wide. A different zig release
produces different bytes, so only the pinned toolchain gives reproducible
output. Build trees end up in `build/<platform>-<config>/` and each one
installs into `build/<platform>-<config>/install/`.

## Platforms

| Platform   | zig triple              | Runs on                                    |
| ---------- | ----------------------- | ------------------------------------------ |
| `linux-64` | `x86_64-linux-gnu.2.17` | x86_64 Linux with glibc 2.17 or newer      |
| `linux-32` | `x86-linux-gnu.2.17`    | i686 Linux with glibc 2.17 or newer        |
| `win-64`   | `x86_64-windows-gnu`    | 64 bit Windows 10+, or 7/8 with the UCRT   |
| `win-32`   | `x86-windows-gnu`       | 32 bit Windows 10+, or 7/8 with the UCRT   |

They are defined in `cmake/targets.json`, which both the toolchain file and
`dev.py` read, so the two cannot drift apart. Adding a platform is a matter
of adding an entry there.

* **glibc 2.17** is pinned in the Linux triples. Without the suffix zig would
  target the glibc of the build machine, and the result would refuse to start
  on anything older.
* **Windows uses the mingw-w64 (`gnu`) ABI**, which keeps the project at
  C++11 with zig's own libc++ linked statically and needs no SDK download.
  The C runtime is Microsoft's UCRT (the `api-ms-win-crt-*` imports): built
  into Windows 10 and later, and available for Windows 7/8 through the
  Universal C Runtime update (KB2999226).
* **`"cpu": "baseline"`** pins the instruction set. For a native target zig
  would otherwise default to the build machine's CPU, which makes the output
  depend on the hardware that built it.

libc++ and libc++abi are linked statically on every target, so the binaries
depend on nothing but the platform's C library.

## Reproducible builds

The goal is that `./dev.py dist` writes the same `dist/SHA256SUMS` on every
machine. What makes that hold:

| Input that could leak        | How it is kept out                                        |
| ---------------------------- | --------------------------------------------------------- |
| Compiler version             | zig pinned in `pixi.lock`, checked against `targets.json` |
| Build machine CPU            | `-mcpu=baseline`                                          |
| Checkout and build paths     | `-ffile-prefix-map` for the source and build directories  |
| zig install path in libc++   | Release builds are stripped (`-s`)                        |
| `__DATE__` / `__TIME__`      | `-Werror=date-time`                                       |
| PE link timestamp            | zig's linker writes a content hash there instead          |
| Stale install artifacts      | install and dist trees are recreated on every run         |

The zig version check lives in `cmake/ZigToolchain.cmake`: configuring with
any zig other than the one in `targets.json` fails, unless
`-DIGWL_ALLOW_ANY_ZIG=ON` is passed (e.g. while trying out an upgrade). To
move to a new zig, update `pixi.toml`, `pixi.lock` and `targets.json`
together.

Stripping matters more than it looks: zig compiles its libc++ with debug
info even in optimised builds, and that debug info holds the absolute path of
the zig installation, which is the pixi environment inside this checkout.
Debug and RelWithDebInfo builds keep their symbols and are therefore only
reproducible from the same path.

### Checking it

```sh
./dev.py repro
```

runs `dist`, then builds everything a second time from a copy of the sources
at a different path, with a pixi environment of its own and empty zig caches,
so zig's libc++, libc++abi and libc startup code are compiled from scratch
rather than reused. It fails if any artifact differs and keeps both trees
around for inspection (e.g. with `diffoscope`). The copy includes
uncommitted changes, so it can be run before committing.

## Using CMake directly

`dev.py` is only a convenience layer; the plain CMake invocation, from
inside `pixi shell`, is:

```sh
cmake -S . -B build/win-64-release -G Ninja \
      --toolchain cmake/ZigToolchain.cmake \
      -DIGWL_TARGET=win-64 \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/win-64-release
```

## How the cross compilation works

CMake expects one executable per tool, while zig exposes everything as
sub-commands of a single binary. The toolchain file therefore generates small
shim scripts (`zig-cc`, `zig-cxx`, `zig-ar`, `zig-ranlib`, `zig-rc`) into the
build directory and pins the target triple and CPU inside them, so they apply
to compiling and linking alike. Beyond that it only tells CMake which system
it builds for and keeps `find_package` away from host libraries.

Because the triple lives in those shims rather than on the compile command
lines, ninja cannot see it change. `dev.py` records the inputs from
`targets.json` per build directory and discards the directory when they
change, instead of reporting "no work to do" against stale artifacts.

One flag the toolchain adds everywhere is `-fsanitize-trap=undefined`. zig
enables UBSan in debug builds and links its own runtime for it, and that
runtime needs a newer glibc than the one pinned. Trapping keeps every check
without the runtime: a violation becomes an illegal instruction instead of a
printed diagnostic. Release builds are unaffected.

## Layout

| Path                       | What it is                                               |
| -------------------------- | -------------------------------------------------------- |
| `src/`                     | The sources                                              |
| `CMakeLists.txt`           | The project, including the reproducibility flags         |
| `cmake/ZigToolchain.cmake` | Toolchain file that turns `zig cc` / `zig c++` into a cross compiler |
| `cmake/targets.json`       | Platforms, zig triples, CPU baseline and the pinned zig version |
| `cmake/zig-wrapper.sh.in`  | Template of the generated compiler shims                 |
| `dev.py`                   | Thin wrapper around the CMake commands                   |
| `pixi.toml`, `pixi.lock`   | The pinned toolchain                                     |
