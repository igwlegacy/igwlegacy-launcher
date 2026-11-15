#!/usr/bin/env python3
"""Build driver for igwlegacy-launcher.

Wraps the CMake/zig cross compilation setup so that a Linux host produces
binaries for every supported Linux and Windows target:

    ./dev.py build                 # debug, for the host platform
    ./dev.py build -r -a           # release, for every supported platform
    ./dev.py build -t win-64       # debug, for 64 bit Windows only
    ./dev.py build -t              # list the platforms -t accepts
    ./dev.py run -r                # build for the host and run the binary
    ./dev.py run -t win-64         # the same for Windows, through wine
    ./dev.py dist                  # release build of every platform into ./dist
    ./dev.py repro                 # check that ./dist is reproducible
    ./dev.py clean
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_ROOT = ROOT / "build"
DIST_DIR = ROOT / "dist"
TOOLCHAIN = ROOT / "cmake" / "ZigToolchain.cmake"
TARGETS = ROOT / "cmake" / "targets.json"

# Base name of the shipped executable, as set by OUTPUT_NAME in CMakeLists.txt.
PROGRAM = "igwlegacy-launcher"

# Written next to the artifacts by `dist` and compared by `repro`.
CHECKSUMS = "SHA256SUMS"

# The one place the platforms are spelled out is cmake/targets.json, which the
# toolchain file reads as well.
PLATFORMS = json.loads(TARGETS.read_text())["platforms"]

REQUIRED_TOOLS = ["cmake", "ninja", "zig"]

# The commands that shell out to the toolchain; the rest only touch files.
TOOLCHAIN_COMMANDS = {"build", "run", "dist", "repro"}

# What `-t` stands for when it is given no value, which asks for the list of
# platforms rather than naming one. argparse cannot offer `choices` and accept
# a valueless flag at the same time, so the names are checked by hand instead.
LIST_PLATFORMS = object()


def host_platform():
    """The target platform matching the machine this script is running on."""
    system = platform.system()
    machine = platform.machine().lower()
    if system != "Linux":
        sys.exit(f"dev.py: building is only supported on Linux, not on '{system}'")
    if machine in ("x86_64", "amd64"):
        return "linux-64"
    if machine in ("i386", "i486", "i586", "i686"):
        return "linux-32"
    # Any other Linux host can still cross compile every target, it just has
    # no native platform of its own in targets.json to default to.
    sys.exit(f"dev.py: no platform for host machine '{machine}', pass -t explicitly")


def reexec_in_pixi():
    """Re-run this script inside this project's pixi environment.

    Unlike a plain "are the tools on PATH" check, this insists on the pixi
    environment even when a cmake/ninja/zig happens to be installed system
    wide: reproducible output depends on the exact toolchain pinned in
    pixi.lock, and a system zig of any other version would silently produce
    different bytes. `--locked` makes pixi fail instead of quietly re-solving
    when pixi.toml and pixi.lock disagree.
    """
    inside = os.environ.get("PIXI_PROJECT_ROOT")
    if inside and Path(inside).resolve() == ROOT:
        missing = [tool for tool in REQUIRED_TOOLS if not shutil.which(tool)]
        if missing:
            sys.exit(f"dev.py: missing in the pixi environment: {', '.join(missing)}")
        return

    pixi = shutil.which("pixi")
    if pixi is None:
        sys.exit("dev.py: pixi was not found on PATH; it provides the pinned toolchain")

    command = [pixi, "run", "--locked", "--manifest-path", str(ROOT / "pixi.toml"),
               "python", str(ROOT / "dev.py"), *sys.argv[1:]]
    sys.exit(subprocess.call(command))


def run(command, env=None, cwd=None):
    text = [str(part) for part in command]
    print("+ " + " ".join(text), flush=True)
    subprocess.run(text, check=True, env=env, cwd=cwd)


def config_name(release):
    return "Release" if release else "Debug"


def build_dir(target, config):
    return BUILD_ROOT / f"{target}-{config.lower()}"


def install_dir(target, config):
    """Where a single platform build is installed, before it is collected."""
    return build_dir(target, config) / "install"


def executable_name(target):
    suffix = ".exe" if PLATFORMS[target]["system"] == "Windows" else ""
    return PROGRAM + suffix


def build_one(target, config, jobs=None, clean=False):
    """Configure, build and install a single target platform."""
    output = build_dir(target, config)
    stamp = output / ".igwl-build-inputs"
    data = json.loads(TARGETS.read_text())
    inputs = json.dumps({"zig": data["zig"], "cpu": data["cpu"], **PLATFORMS[target]},
                        sort_keys=True) + "\n"

    # The target triple is baked into the generated compiler shims rather than
    # into the compile command lines, so ninja cannot see it change and would
    # report "no work to do" against the old artifacts. Editing targets.json
    # therefore has to discard the build directory.
    if stamp.is_file() and stamp.read_text() != inputs:
        print(f"build inputs changed, discarding {output}", flush=True)
        clean = True

    if clean:
        shutil.rmtree(output, ignore_errors=True)

    output.mkdir(parents=True, exist_ok=True)
    stamp.write_text(inputs)

    # CMake is cheap to re-run and skips the work when nothing changed, so this
    # is called unconditionally instead of checking for an existing cache.
    run([
        "cmake",
        "-S", ROOT,
        "-B", output,
        "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}",
        f"-DIGWL_TARGET={target}",
        f"-DCMAKE_BUILD_TYPE={config}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ])

    build_command = ["cmake", "--build", output]
    if jobs:
        build_command += ["--parallel", str(jobs)]
    run(build_command)

    # A fresh install tree every time, so that nothing a previous build
    # installed can linger and end up in dist.
    shutil.rmtree(install_dir(target, config), ignore_errors=True)
    # Only the launcher itself: the vendored libraries bring install rules for
    # their headers and static libraries, which have no place in a release.
    run(["cmake", "--install", output, "--prefix", install_dir(target, config),
         "--component", "launcher"])


def resolve_targets(args):
    """The platforms a command was asked to build, defaulting to the host."""
    if getattr(args, "all", False):
        return sorted(PLATFORMS)
    if args.target:
        unknown = [name for name in args.target if name not in PLATFORMS]
        if unknown:
            sys.exit("dev.py: unknown platform {}; `./dev.py build -t` "
                     "lists the supported ones".format(", ".join(unknown)))
        return args.target
    return [host_platform()]


def print_platforms():
    """Prints every platform `-t` accepts, and what each one builds for."""
    try:
        host = host_platform()
    except SystemExit:
        host = None
    width = max(len(name) for name in PLATFORMS)
    print("supported platforms:")
    for name in sorted(PLATFORMS):
        note = "  host" if name == host else ""
        print(f"  {name:<{width}}  {PLATFORMS[name]['triple']}{note}")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_checksums(path):
    """Parses a SHA256SUMS file into {file name: digest}."""
    result = {}
    for line in path.read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        result[name.lstrip("*")] = digest
    return result


def command_build(args):
    config = config_name(args.release)
    for target in resolve_targets(args):
        print(f"\n== {target} ({config}) ==", flush=True)
        build_one(target, config, jobs=args.jobs, clean=args.clean)


def command_run(args):
    targets = resolve_targets(args)
    if len(targets) != 1:
        sys.exit("dev.py: run takes a single platform")
    target = targets[0]
    config = config_name(args.release)
    build_one(target, config, jobs=args.jobs)

    command = [install_dir(target, config) / "bin" / executable_name(target)]
    if PLATFORMS[target]["system"] == "Windows":
        wine = shutil.which("wine")
        if wine is None:
            sys.exit("dev.py: running a Windows build needs wine on PATH")
        command.insert(0, wine)

    # argparse.REMAINDER keeps the `--` that separates our flags from the
    # program's, which is not meant for the program itself.
    program_args = args.args[1:] if args.args[:1] == ["--"] else args.args
    run([*command, *program_args])


def command_dist(args):
    """Builds every platform in release mode and collects it into ./dist.

    The directory is rebuilt from scratch, so artifacts of a platform dropped
    in the meantime cannot linger, and SHA256SUMS is what `repro` compares.
    """
    targets = sorted(PLATFORMS)
    for target in targets:
        print(f"\n== {target} (Release) ==", flush=True)
        build_one(target, "Release", jobs=args.jobs, clean=args.clean)

    shutil.rmtree(DIST_DIR, ignore_errors=True)
    DIST_DIR.mkdir(parents=True)

    print(f"\ncollecting into {DIST_DIR}", flush=True)
    lines = []
    for target in targets:
        source = install_dir(target, "Release") / "bin" / executable_name(target)
        if not source.is_file():
            sys.exit(f"dev.py: {source} was not installed")
        stem, dot, suffix = source.name.partition(".")
        destination = DIST_DIR / f"{stem}-{target}{dot}{suffix}"
        shutil.copy2(source, destination)
        lines.append(f"{sha256(destination)}  {destination.name}")

    # Sorted by file name, so that the file itself is reproducible as well.
    lines.sort(key=lambda line: line.split(maxsplit=1)[1])
    (DIST_DIR / CHECKSUMS).write_text("\n".join(lines) + "\n")
    print("\n".join(f"  {line}" for line in lines))


def scratch_sources(destination):
    """Copies every source file of the checkout into `destination`.

    Tracked files plus untracked ones that are not ignored, so that the check
    covers uncommitted work too, while build trees, dist and the pixi
    environment stay behind.
    """
    listing = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT, check=True, capture_output=True).stdout.decode()
    for name in filter(None, listing.split("\0")):
        source = ROOT / name
        # Deleted but not yet committed files are still listed as cached.
        if not source.is_file():
            continue
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def command_repro(args):
    """Checks that ./dist is reproduced bit for bit by an independent build.

    The second build runs from a copy of the sources at a different path, in
    a pixi environment of its own (so the zig installation path differs too),
    and with empty zig caches, so that zig's libc++, libc++abi and libc
    startup code get recompiled from scratch instead of being reused. Anything
    that leaks a path, a timestamp or cached state shows up as a mismatch.
    """
    command_dist(args)

    scratch = Path(tempfile.mkdtemp(prefix="igwl-repro-"))
    try:
        # Deliberately a different directory name than the original checkout.
        checkout = scratch / "independent-checkout"
        scratch_sources(checkout)

        env = dict(os.environ)
        # Leave the current pixi activation behind, so that the copy activates
        # its own environment rather than inheriting this one.
        for key in [key for key in env if key.startswith(("PIXI_", "CONDA_"))]:
            del env[key]
        env["ZIG_GLOBAL_CACHE_DIR"] = str(scratch / "zig-global-cache")
        env["ZIG_LOCAL_CACHE_DIR"] = str(scratch / "zig-local-cache")

        print(f"\n== independent build in {checkout} ==", flush=True)
        dist_command = [sys.executable, checkout / "dev.py", "dist"]
        if args.jobs:
            dist_command += ["--jobs", str(args.jobs)]
        run(dist_command, env=env, cwd=scratch)

        ours = read_checksums(DIST_DIR / CHECKSUMS)
        theirs = read_checksums(checkout / "dist" / CHECKSUMS)

        problems = []
        for name in sorted(set(ours) | set(theirs)):
            if name not in theirs:
                problems.append(f"{name}: missing from the independent build")
            elif name not in ours:
                problems.append(f"{name}: only produced by the independent build")
            elif ours[name] != theirs[name]:
                problems.append(f"{name}: {ours[name]} != {theirs[name]}")

        if problems:
            print("\nrepro FAILED:", file=sys.stderr)
            for problem in problems:
                print(f"  {problem}", file=sys.stderr)
            print(f"\nboth builds are kept for inspection: {DIST_DIR} and "
                  f"{checkout / 'dist'}", file=sys.stderr)
            # Keep the scratch tree, it is what a diffoscope run needs.
            args.keep = True
            sys.exit(1)

        print(f"\nrepro ok: {len(ours)} files bit-identical across both builds")
    finally:
        if args.keep:
            print(f"scratch tree kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


def command_clean(_args):
    for directory in (BUILD_ROOT, DIST_DIR):
        print(f"removing {directory}", flush=True)
        shutil.rmtree(directory, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_jobs_flag(subparser):
        subparser.add_argument("-j", "--jobs", type=int,
                               help="number of parallel compile jobs")

    def add_target_flag(subparser, help_text):
        subparser.add_argument("-t", "--target", action="append", nargs="?",
                               const=LIST_PLATFORMS, metavar="PLATFORM",
                               help=help_text)

    build = subparsers.add_parser("build", help="cross compile the project")
    build.add_argument("-r", "--release", action="store_true",
                       help="use the release configuration (default: debug)")
    add_jobs_flag(build)
    build.add_argument("-a", "--all", action="store_true",
                       help="build for every supported platform")
    add_target_flag(build, "build for this platform (repeatable, default: the host); "
                           "with no value, list the platforms instead")
    build.add_argument("--clean", action="store_true",
                       help="discard the existing build directory first")
    build.set_defaults(func=command_build)

    run_parser = subparsers.add_parser("run", help="build one platform and run the binary")
    run_parser.add_argument("-r", "--release", action="store_true",
                            help="use the release configuration (default: debug)")
    add_jobs_flag(run_parser)
    add_target_flag(run_parser, "platform to build and run (default: the host); "
                                "Windows builds run through wine")
    run_parser.add_argument("args", nargs=argparse.REMAINDER,
                            help="arguments passed on to the binary")
    run_parser.set_defaults(func=command_run)

    dist = subparsers.add_parser(
        "dist", help="release build of every platform, collected into ./dist")
    add_jobs_flag(dist)
    dist.add_argument("--clean", action="store_true",
                      help="discard the existing build directories first")
    dist.set_defaults(func=command_dist)

    repro = subparsers.add_parser(
        "repro", help="run dist, then verify an independent build matches it")
    add_jobs_flag(repro)
    repro.add_argument("--clean", action="store_true",
                       help="discard the existing build directories first")
    repro.add_argument("--keep", action="store_true",
                       help="keep the scratch tree of the independent build")
    repro.set_defaults(func=command_repro)

    clean = subparsers.add_parser("clean", help="remove the build and dist directories")
    clean.set_defaults(func=command_clean)

    args = parser.parse_args()

    # Listing the platforms reads one JSON file and needs neither the toolchain
    # nor pixi, so it is answered before the re-exec rather than after it.
    if getattr(args, "target", None) and LIST_PLATFORMS in args.target:
        print_platforms()
        return

    if args.command in TOOLCHAIN_COMMANDS:
        reexec_in_pixi()

    try:
        args.func(args)
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)


if __name__ == "__main__":
    main()
