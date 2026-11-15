# igwlegacy-launcher

A game launcher: a small FLTK window that brings a local game directory in
sync with the copy published on a web server (like rsync, but over plain
HTTPS and with a GUI) and then starts the game.

It is written in C++11 and cross-compiles from Linux to Linux and Windows,
using **zig** as the cross toolchain and **CMake** as the build system.
Release builds are **bit-for-bit reproducible**: the same sources give the
same bytes on any machine, from any checkout path, at any time.

Nothing beyond [pixi](https://pixi.sh) has to be installed: it provides the
exact cmake, ninja, python and zig versions recorded in `pixi.lock`. All
libraries are git submodules below `vendor/`, so after cloning run:

```sh
git submodule update --init
```

## How syncing works

The server holds three things below a base URL:

```
<base>/manifest.json       every file of the game with size and SHA-256
<base>/manifest.json.sig   Ed25519 signature of manifest.json (hex)
<base>/files/<path>        the files themselves
```

When Start is pressed (or with `--headless`), the launcher:

1. downloads `manifest.json` and its signature and verifies the signature
   against the public key compiled into it, before even parsing the JSON;
2. checks the local files: size first, then the SHA-256, which is cached in
   `<game>/.igwl-launcher/state.json` together with size and modification
   time so unchanged files are not hashed again on every start;
3. downloads what is missing or different into
   `.igwl-launcher/download.part`, verifies size and hash and only then
   renames it into place, so an interrupted or corrupted download never
   replaces a working file;
4. deletes files that an earlier manifest installed but the current one no
   longer lists. Files the launcher never installed (savegames, configs,
   mods) are never touched;
5. starts the program the manifest names for the platform (`launch.windows`
   or `launch.linux`) with the game directory as working directory, and
   closes.

The game directory defaults to the directory the launcher binary is in.
Every network or verification failure stops the sync with a message and
leaves the installed files as they were.

TLS is handled by Schannel and the Windows certificate store on Windows,
and by a vendored mbedTLS on Linux, which uses the distribution's CA bundle
(found at runtime, or set with `SSL_CERT_FILE`). The signature is what makes
the content trustworthy, though: a compromised web server can withhold
updates but cannot get the launcher to install anything that was not signed
with the private key.

### Command line

```
igwlegacy-launcher [--headless] [--no-launch] [--dir PATH]
                   [--base-url URL] [--public-key HEX] [--version] [--help]
```

`--headless` syncs without a window and prints progress, `--no-launch`
skips starting the game. `--base-url` and `--public-key` override the
compiled-in values, which is mostly useful for testing.

### Configuration

Two CMake cache variables in `CMakeLists.txt` are compiled into the binary:

| Variable                   | What it is                                           |
| -------------------------- | ---------------------------------------------------- |
| `IGWL_BASE_URL`            | Base URL of the published game, ending in `/`        |
| `IGWL_MANIFEST_PUBLIC_KEY` | 64 hex digits, the Ed25519 public key                |

A build without a public key still succeeds (with a configure warning), but
the launcher then refuses to sync.

### Publishing

`tools/manifest.py` creates keys and manifests. It is plain Python 3 without
dependencies (Ed25519 included), so it also runs on the web server.

```sh
# once: create the signing key and put the printed public key into
# IGWL_MANIFEST_PUBLIC_KEY; keep the private key secret, off the web server
./tools/manifest.py keygen ~/igwlegacy-signing.key

# per release: put the game into <publish>/files/, then
./tools/manifest.py build <publish> --key ~/igwlegacy-signing.key \
    --launch-windows game.exe --launch-linux game.sh
./tools/manifest.py verify <publish> --public-key <hex>   # optional
# upload <publish>/ to the base URL; manifest.json and manifest.json.sig last
```

Files with the executable bit set below `files/` are marked as executable
in the manifest, and the launcher sets the bit on Linux. Paths that cannot
exist on both Windows and Linux (reserved names like `CON`, characters such
as `:` or `\`, names differing only in case) are rejected by the launcher.

### Limitations

* No rollback protection: an attacker who controls the server can serve an
  older, validly signed manifest. Only content signed with the key installs.
* The launcher does not update itself and must not be listed in the
  manifest; the running binary is skipped if it is.
* Files are always downloaded whole; there is no delta transfer.

### Testing

```sh
./tools/e2e-test.py build/linux-64-debug/bin/igwlegacy-launcher
```

publishes a small signed test game, serves it on `127.0.0.1` and runs the
launcher headless against it: download, rerun, repair, removal of dropped
files, launch, and refusal of tampered manifests and files. CI runs it on
the linux-64 release binary.

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

libc++ and libc++abi are linked statically on every target, as are all
vendored libraries. On Windows the binaries depend on system DLLs only; on
Linux on the C library and on the X11 libraries every desktop has
(`libX11`, `libXrender`, `libXft`, `libfontconfig`), see below.

## Vendored libraries

| Submodule                    | Used for                                           |
| ---------------------------- | -------------------------------------------------- |
| `vendor/fltk`                | The GUI (X11 + Xft on Linux, GDI+ on Windows)      |
| `vendor/curl`                | HTTP(S), built static and HTTP-only                |
| `vendor/mbedtls`             | SHA-256 everywhere, TLS for curl on Linux          |
| `vendor/monocypher`          | Ed25519 signature verification (RFC 8032)          |
| `vendor/json`                | nlohmann/json, parsing the manifest                |
| `vendor/filesystem`          | ghc::filesystem, `std::filesystem` for C++11       |
| `vendor/xorgproto`, `vendor/libx11`, `vendor/libxrender`, `vendor/libxft`, `vendor/fontconfig`, `vendor/freetype` | Headers only, for linking against the system's X11 libraries |

`cmake/Vendor.cmake` builds them and pins every option they read, so the
build never depends on a library default or on something found on the build
machine. To update one, check out a newer release tag inside the submodule
and commit the new gitlink.

### Linking X11 without a sysroot

zig ships a glibc for every target but no X11. The launcher has to use the
X11 libraries of the system it runs on anyway, so it only needs something to
link against at build time. `cmake/LinuxSysroot.cmake` assembles that in the
build directory: the headers from the submodules above (plus the few that
upstream generates at configure time), and stub shared libraries with the
real sonames that export the right function names but contain nothing. The
binary ends up with the usual `DT_NEEDED` entries and the dynamic loader
picks up the real libraries at runtime. CMake's stock `FindX11`,
`FindFreetype` and `FindFontconfig` then work unchanged for FLTK.

The exported symbol lists live in `cmake/linux-stubs/*.symbols` and are
created by `tools/gen-linux-stub-symbols.py` from the libraries of a Linux
machine. Since these libraries only ever add functions, a list taken from a
recent system covers every function FLTK calls on older ones too.

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
| `src/`                     | The launcher: `gui`, `sync`, `manifest`, `http`, `crypto`, `platform` |
| `vendor/`                  | The libraries, as git submodules                         |
| `CMakeLists.txt`           | The project, including configuration and reproducibility flags |
| `cmake/Vendor.cmake`       | Builds the vendored libraries with pinned options        |
| `cmake/LinuxSysroot.cmake` | X11 headers and link stubs for the Linux targets         |
| `cmake/linux-stubs/`       | Symbol lists the link stubs are generated from           |
| `tools/manifest.py`        | Key generation, manifest building and signing            |
| `tools/e2e-test.py`        | End-to-end test against a local HTTP server              |
| `tools/gen-linux-stub-symbols.py` | Regenerates `cmake/linux-stubs/`                  |
| `cmake/ZigToolchain.cmake` | Toolchain file that turns `zig cc` / `zig c++` into a cross compiler |
| `cmake/targets.json`       | Platforms, zig triples, CPU baseline and the pinned zig version |
| `cmake/zig-wrapper.sh.in`  | Template of the generated compiler shims                 |
| `dev.py`                   | Thin wrapper around the CMake commands                   |
| `pixi.toml`, `pixi.lock`   | The pinned toolchain                                     |
