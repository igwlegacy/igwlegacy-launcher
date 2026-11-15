#!/usr/bin/env python3
"""Creates and signs the manifest the launcher syncs against.

Server layout, below the URL compiled into the launcher (IGWL_BASE_URL):

    manifest.json         list of files with size and SHA-256
    manifest.json.sig     hex encoded Ed25519 signature of manifest.json
    files/...             the game files themselves

Typical use:

    # once: create the signing key; keep it secret and off the web server
    ./tools/manifest.py keygen ~/igwlegacy-signing.key
    # -> prints the public key for IGWL_MANIFEST_PUBLIC_KEY in CMakeLists.txt

    # on every release: put the game files into <publish>/files/, then
    ./tools/manifest.py build <publish> --key ~/igwlegacy-signing.key \\
        --launch-windows game.exe --launch-linux game.sh
    # and upload <publish> to the web server

    # optionally check a publish directory against a public key
    ./tools/manifest.py verify <publish> --public-key <hex>

Pure Python 3 without dependencies, so it runs anywhere, including on the web
server itself. Signing uses the RFC 8032 reference algorithm below; it is not
constant time, which does not matter for signing on a machine you control.
"""

import argparse
import hashlib
import json
import os
import stat
import sys
from pathlib import Path

# Must match kStateDirectory in src/manifest.cpp; the launcher keeps its own
# data there and rejects manifests that place files inside it.
STATE_DIRECTORY = ".igwl-launcher"

# --- Ed25519, after the reference implementation in RFC 8032, section 6 -------

_P = 2**255 - 19
_D = -121665 * pow(121666, _P - 2, _P) % _P
_Q = 2**252 + 27742317777372353535851937790883648493
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _sha512(data):
    return hashlib.sha512(data).digest()


def _inverse(x):
    return pow(x, _P - 2, _P)


def _point_add(a, b):
    """Adds two points in extended homogeneous coordinates (X, Y, Z, T)."""
    first = (a[1] - a[0]) * (b[1] - b[0]) % _P
    second = (a[1] + a[0]) * (b[1] + b[0]) % _P
    third = 2 * a[3] * b[3] * _D % _P
    fourth = 2 * a[2] * b[2] % _P
    e, f, g, h = second - first, fourth - third, fourth + third, second + first
    return (e * f, g * h, f * g, e * h)


def _point_mul(scalar, point):
    result = (0, 1, 1, 0)  # the neutral element
    while scalar > 0:
        if scalar & 1:
            result = _point_add(result, point)
        point = _point_add(point, point)
        scalar >>= 1
    return result


def _point_equal(a, b):
    # x1 / z1 == x2 / z2  <=>  x1 * z2 == x2 * z1, and the same for y.
    if (a[0] * b[2] - b[0] * a[2]) % _P != 0:
        return False
    return (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * _inverse(_D * y * y + 1) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_G_Y = 4 * _inverse(5) % _P
_G_X = _recover_x(_G_Y, 0)
_G = (_G_X, _G_Y, 1, _G_X * _G_Y % _P)


def _compress(point):
    z_inverse = _inverse(point[2])
    x = point[0] * z_inverse % _P
    y = point[1] * z_inverse % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(data):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


def _expand_secret(secret):
    digest = _sha512(secret)
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, digest[32:]


def _hash_mod_q(data):
    return int.from_bytes(_sha512(data), "little") % _Q


def ed25519_public_key(secret):
    scalar, _ = _expand_secret(secret)
    return _compress(_point_mul(scalar, _G))


def ed25519_sign(secret, message):
    scalar, prefix = _expand_secret(secret)
    public = _compress(_point_mul(scalar, _G))
    nonce = _hash_mod_q(prefix + message)
    r = _compress(_point_mul(nonce, _G))
    s = (nonce + _hash_mod_q(r + public + message) * scalar) % _Q
    return r + int.to_bytes(s, 32, "little")


def ed25519_verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(public)
    r = _decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _Q:
        return False
    h = _hash_mod_q(signature[:32] + public + message)
    return _point_equal(_point_mul(s, _G), _point_add(r, _point_mul(h, a)))


def _self_test():
    """RFC 8032, section 7.1, test 2: refuse to sign with a broken implementation."""
    secret = bytes.fromhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")
    public = bytes.fromhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c")
    expected = bytes.fromhex(
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")
    if (ed25519_public_key(secret) != public or ed25519_sign(secret, b"\x72") != expected
            or not ed25519_verify(public, b"\x72", expected)
            or ed25519_verify(public, b"\x73", expected)):
        sys.exit("manifest.py: Ed25519 self test failed, refusing to continue")


# --- Manifest -----------------------------------------------------------------

def _read_secret(path):
    text = Path(path).read_text().strip()
    try:
        secret = bytes.fromhex(text)
    except ValueError:
        secret = b""
    if len(secret) != 32:
        sys.exit(f"manifest.py: {path} does not contain a 64 digit hex key")
    return secret


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _collect_files(files_dir):
    """Every regular file below `files_dir` as manifest entries, sorted by path."""
    entries = []
    for directory, subdirectories, names in os.walk(files_dir):
        # Deterministic traversal; symlinked directories are not followed.
        subdirectories.sort()
        for name in sorted(names):
            full = Path(directory) / name
            relative = full.relative_to(files_dir).as_posix()
            info = full.stat()
            if not stat.S_ISREG(info.st_mode):
                sys.exit(f"manifest.py: {full} is not a regular file")
            if relative.split("/", 1)[0].lower() == STATE_DIRECTORY:
                sys.exit(f"manifest.py: {relative} is inside the launcher's state directory")
            entry = {"path": relative, "size": info.st_size, "sha256": _sha256(full)}
            if info.st_mode & stat.S_IXUSR:
                entry["executable"] = True
            entries.append(entry)
    entries.sort(key=lambda entry: entry["path"])
    return entries


def command_keygen(args):
    target = Path(args.key_file)
    if target.exists():
        sys.exit(f"manifest.py: {target} exists already, not overwriting a signing key")
    secret = os.urandom(32)
    # Created with owner-only permissions from the start, not chmod'ed later.
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(secret.hex() + "\n")
    print(f"private key written to {target} - keep it secret and backed up")
    print("public key (set IGWL_MANIFEST_PUBLIC_KEY in CMakeLists.txt to this):")
    print(ed25519_public_key(secret).hex())


def command_build(args):
    publish = Path(args.publish_dir)
    files_dir = publish / "files"
    if not files_dir.is_dir():
        sys.exit(f"manifest.py: {files_dir} does not exist; put the game files there")
    secret = _read_secret(args.key)

    files = _collect_files(files_dir)
    listed = {entry["path"] for entry in files}
    launch = {}
    for platform, program in (("windows", args.launch_windows), ("linux", args.launch_linux)):
        if program is None:
            continue
        if program not in listed:
            sys.exit(f"manifest.py: launch program {program} is not below {files_dir}")
        launch[platform] = program

    manifest = {"format": 1, "files": files}
    if launch:
        manifest["launch"] = launch
    # Stable formatting, so rebuilding an unchanged tree yields identical
    # bytes (and an identical signature, Ed25519 being deterministic).
    data = (json.dumps(manifest, indent=1, sort_keys=True, ensure_ascii=False) + "\n").encode()
    signature = ed25519_sign(secret, data)

    # Written next to each other; upload both at once, so clients never see
    # a manifest with the signature of another one.
    (publish / "manifest.json").write_bytes(data)
    (publish / "manifest.json.sig").write_text(signature.hex() + "\n")
    total = sum(entry["size"] for entry in files)
    print(f"{publish / 'manifest.json'}: {len(files)} files, {total} bytes, signed with "
          f"public key {ed25519_public_key(secret).hex()}")


def command_verify(args):
    publish = Path(args.publish_dir)
    data = (publish / "manifest.json").read_bytes()
    signature = bytes.fromhex((publish / "manifest.json.sig").read_text().strip())
    if not ed25519_verify(bytes.fromhex(args.public_key), data, signature):
        sys.exit("manifest.py: signature INVALID for this public key")
    problems = 0
    for entry in json.loads(data)["files"]:
        path = publish / "files" / entry["path"]
        if not path.is_file() or path.stat().st_size != entry["size"] or _sha256(path) != entry["sha256"]:
            print(f"mismatch: {entry['path']}")
            problems += 1
    if problems:
        sys.exit(f"manifest.py: {problems} files do not match the manifest")
    print("signature valid, all files match")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    keygen = subparsers.add_parser("keygen", help="create a new signing key")
    keygen.add_argument("key_file", help="where to write the private key")
    keygen.set_defaults(func=command_keygen)

    build = subparsers.add_parser("build", help="write and sign manifest.json")
    build.add_argument("publish_dir", help="directory whose files/ subdirectory holds the game")
    build.add_argument("--key", required=True, help="private key file from `keygen`")
    build.add_argument("--launch-windows", metavar="PATH",
                       help="program (below files/) the Windows launcher starts")
    build.add_argument("--launch-linux", metavar="PATH",
                       help="program (below files/) the Linux launcher starts")
    build.set_defaults(func=command_build)

    verify = subparsers.add_parser("verify", help="check signature and files of a publish dir")
    verify.add_argument("publish_dir")
    verify.add_argument("--public-key", required=True, help="hex encoded public key")
    verify.set_defaults(func=command_verify)

    args = parser.parse_args()
    _self_test()
    args.func(args)


if __name__ == "__main__":
    main()
