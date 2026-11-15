#!/usr/bin/env python3
"""Regenerates the symbol lists behind the Linux link stubs.

The Linux builds have no sysroot: FLTK's X11/Xft dependencies are compiled
against the vendored headers and linked against small stub shared objects
that cmake/LinuxSysroot.cmake generates from the lists in cmake/linux-stubs/.
At runtime the real system libraries with the same SONAME are loaded instead.

The lists are committed, so that the build never depends on what happens to
be installed on the build machine. Rerun this script only when the vendored
headers move to a release that adds functions FLTK starts to use:

    ./tools/gen-linux-stub-symbols.py /usr/lib/libX11.so.6 /usr/lib/libXft.so.2 ...

Only exported *functions* are recorded. Data symbols are left out on purpose,
because their sizes differ between 32 and 64 bit and a copy relocation of the
wrong size would corrupt memory at runtime; should FLTK ever reference one,
the link fails loudly instead.
"""

import subprocess
import sys
from pathlib import Path

OUTPUT_DIR = Path(__file__).resolve().parent.parent / "cmake" / "linux-stubs"


def soname(library):
    """The SONAME recorded in the dynamic section of `library`."""
    output = subprocess.run(["readelf", "-d", library], check=True,
                            capture_output=True, text=True).stdout
    for line in output.splitlines():
        if "(SONAME)" in line:
            return line.split("[", 1)[1].rstrip("]")
    sys.exit(f"{library}: no SONAME found")


def exported_functions(library):
    """Names of the global function symbols `library` defines."""
    output = subprocess.run(["nm", "-D", "--defined-only", library], check=True,
                            capture_output=True, text=True).stdout
    names = set()
    for line in output.splitlines():
        fields = line.split()
        # "<address> <type> <name>"; T is a global symbol in the text section.
        if len(fields) == 3 and fields[1] == "T":
            # Drop any symbol version suffix, the stubs are unversioned.
            names.add(fields[2].split("@", 1)[0])
    return sorted(names)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for library in sys.argv[1:]:
        name = soname(library)
        functions = exported_functions(library)
        target = OUTPUT_DIR / f"{name}.symbols"
        target.write_text("".join(f"{function}\n" for function in functions))
        print(f"{target}: {len(functions)} functions")


if __name__ == "__main__":
    main()
