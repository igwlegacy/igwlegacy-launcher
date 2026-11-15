#!/usr/bin/env python3
"""Creates and signs the manifest the launcher syncs against.

Server layout, below the URL compiled into the launcher (IGWL_BASE_URL):

    manifest.json         every file of the game: path, size, SHA-256,
                          modification time, executable bit, and for split
                          files the list of their chunks
    manifest.json.sig     hex encoded Ed25519 signature of manifest.json
    chunks/<sha256>       the content, named after its SHA-256: a file of at
                          most --chunk-size bytes as a single chunk, larger
                          ones split into chunks of at most --chunk-size
                          bytes, which the launcher downloads and concatenates

The hosting provider limits files to 20 MB, hence the default --chunk-size.
Chunks are plain byte ranges of the files, not compressed (game data usually
is already). Only manifest.json knows which file a chunk belongs to. Being
named after their content, chunks never change once written: identical data
is stored once, a rebuild only adds the chunks that are new, and clients can
keep downloading while new chunks are uploaded.

Typical use:

    # once: create the signing key; keep it secret and off the web server
    ./tools/manifest.py keygen ~/igwlegacy-signing.key
    # -> prints the public key for IGWL_MANIFEST_PUBLIC_KEY in CMakeLists.txt

    # on every release: split the game into <publish> and sign the manifest
    ./tools/manifest.py build <game> <publish> --key ~/igwlegacy-signing.key \\
        --launch-windows game.exe --launch-linux game.sh
    # then upload <publish> to the web server: chunks/ first, then
    # manifest.json and manifest.json.sig

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

# Manifest format written by `build`; must match kManifestFormat in
# src/manifest.cpp.
MANIFEST_FORMAT = 2

# The hosting provider refuses files larger than 20 MB. Decimal megabytes, so
# the default stays below the limit whether it is meant as MB or MiB.
DEFAULT_CHUNK_SIZE = 20 * 1000 * 1000

# Latest modification time the launcher accepts, around the year 2242; must
# match kMaxMtime in src/manifest.cpp.
MAX_MTIME = 1 << 33

# Directory below the publish directory (and the base URL) holding the
# chunks; must match chunk_url_path() in src/manifest.cpp.
CHUNK_DIRECTORY = "chunks"

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


def chunk_relative_path(digest):
    """Where a chunk lives below the publish directory (and the base URL)."""
    return f"{CHUNK_DIRECTORY}/{digest}"


def _is_chunk_name(name):
    return len(name) == 64 and all(c in "0123456789abcdef" for c in name)


def _write_chunk(publish, data):
    """Stores `data` as a chunk unless it exists already; returns its entry."""
    digest = hashlib.sha256(data).hexdigest()
    target = publish / chunk_relative_path(digest)
    # Same name means same content, so an existing chunk is left alone (and
    # keeps its modification time, so upload tools comparing times skip it);
    # its size is still checked to repair a chunk truncated by a crash.
    if not target.is_file() or target.stat().st_size != len(data):
        temporary = target.with_name(target.name + ".tmp")
        temporary.write_bytes(data)
        os.replace(temporary, target)
    return {"sha256": digest, "size": len(data)}


def _split_file(path, publish, chunk_size):
    """Stores the content of `path` as chunks below `publish`.

    Returns the SHA-256 of the whole file, its size and the list of chunk
    entries in file order: a single one named after the whole file's hash
    if it fits into `chunk_size` bytes, none for an empty file.
    """
    whole = hashlib.sha256()
    chunks = []
    size = 0
    with open(path, "rb") as handle:
        for data in iter(lambda: handle.read(chunk_size), b""):
            whole.update(data)
            chunks.append(_write_chunk(publish, data))
            size += len(data)
    return whole.hexdigest(), size, chunks


def _collect_files(game_dir, publish, chunk_size):
    """Every regular file below `game_dir` as manifest entries, sorted by path.

    Writes the content of every file as chunks below `publish`. Entries of
    files that fit into a single chunk carry no chunk list: their content is
    the chunk named after their own SHA-256.
    """
    entries = []
    for directory, subdirectories, names in os.walk(game_dir):
        # Deterministic traversal; symlinked directories are not followed.
        subdirectories.sort()
        for name in sorted(names):
            full = Path(directory) / name
            relative = full.relative_to(game_dir).as_posix()
            info = full.stat()
            if not stat.S_ISREG(info.st_mode):
                sys.exit(f"manifest.py: {full} is not a regular file")
            if relative.split("/", 1)[0].lower() == STATE_DIRECTORY:
                sys.exit(f"manifest.py: {relative} is inside the launcher's state directory")
            digest, size, chunks = _split_file(full, publish, chunk_size)
            # Files are read in pieces, so a file growing or shrinking
            # meanwhile would otherwise go unnoticed.
            if size != info.st_size:
                sys.exit(f"manifest.py: {full} changed while it was being read")
            mtime = int(info.st_mtime)
            if not 0 <= mtime <= MAX_MTIME:
                sys.exit(f"manifest.py: {full} has a modification time before 1970 or after "
                         "2242, which the launcher refuses; touch it")
            entry = {
                "path": relative,
                "size": size,
                "sha256": digest,
                # Whole seconds since the Unix epoch, which every file system
                # the launcher runs on can represent.
                "mtime": mtime,
            }
            if len(chunks) > 1:
                entry["chunks"] = chunks
            if info.st_mode & stat.S_IXUSR:
                entry["executable"] = True
            entries.append(entry)
    entries.sort(key=lambda entry: entry["path"])
    return entries


def _entry_chunks(entry):
    """The chunks a manifest entry is assembled from, in order."""
    if "chunks" in entry:
        return entry["chunks"]
    if entry["size"] == 0:
        return []
    return [{"sha256": entry["sha256"], "size": entry["size"]}]


def _remove_unreferenced(publish, referenced):
    """Deletes chunks (and leftover temporary files) the manifest does not use.

    Only touches names in chunks/ that look like chunks, so the publish
    directory mirrors exactly what the server needs.
    """
    removed = 0
    for chunk in sorted((publish / CHUNK_DIRECTORY).iterdir()):
        name = chunk.name[:-len(".tmp")] if chunk.name.endswith(".tmp") else chunk.name
        if not chunk.is_file() or not _is_chunk_name(name):
            continue
        if chunk.name != name or name not in referenced:
            chunk.unlink()
            removed += 1
    return removed


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
    game_dir = Path(args.game_dir)
    publish = Path(args.publish_dir)
    if not game_dir.is_dir():
        sys.exit(f"manifest.py: {game_dir} is not a directory")
    if args.chunk_size <= 0:
        sys.exit("manifest.py: --chunk-size must be positive")
    # Writing chunks into the tree being split would make them part of the
    # game (and the walk would pick up its own output).
    game_resolved, publish_resolved = game_dir.resolve(), publish.resolve()
    if (publish_resolved == game_resolved or game_resolved in publish_resolved.parents
            or publish_resolved in game_resolved.parents):
        sys.exit("manifest.py: the game and publish directories must not contain each other")
    secret = _read_secret(args.key)

    (publish / CHUNK_DIRECTORY).mkdir(parents=True, exist_ok=True)
    files = _collect_files(game_dir, publish, args.chunk_size)
    listed = {entry["path"] for entry in files}
    launch = {}
    for platform, program in (("windows", args.launch_windows), ("linux", args.launch_linux)):
        if program is None:
            continue
        if program not in listed:
            sys.exit(f"manifest.py: launch program {program} is not below {game_dir}")
        launch[platform] = program

    referenced = {chunk["sha256"] for entry in files for chunk in _entry_chunks(entry)}
    removed = 0 if args.keep_unreferenced else _remove_unreferenced(publish, referenced)

    manifest = {"format": MANIFEST_FORMAT, "files": files}
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
    split = sum("chunks" in entry for entry in files)
    print(f"{publish / 'manifest.json'}: {len(files)} files ({split} split), {total} bytes in "
          f"{len(referenced)} chunks of at most {args.chunk_size} bytes"
          + (f", {removed} unreferenced chunks removed" if removed else "")
          + f", signed with public key {ed25519_public_key(secret).hex()}")


def command_verify(args):
    publish = Path(args.publish_dir)
    data = (publish / "manifest.json").read_bytes()
    signature = bytes.fromhex((publish / "manifest.json.sig").read_text().strip())
    if not ed25519_verify(bytes.fromhex(args.public_key), data, signature):
        sys.exit("manifest.py: signature INVALID for this public key")
    manifest = json.loads(data)
    if manifest.get("format") != MANIFEST_FORMAT:
        sys.exit(f"manifest.py: manifest format {manifest.get('format')} is not "
                 f"{MANIFEST_FORMAT}")
    problems = 0
    for entry in manifest["files"]:
        sources = [(publish / chunk_relative_path(chunk["sha256"]), chunk["size"],
                    chunk["sha256"]) for chunk in _entry_chunks(entry)]
        # Reassembles the file exactly like the launcher does, checking every
        # piece and the result.
        whole = hashlib.sha256()
        size = 0
        broken = False
        for path, expected_size, expected_digest in sources:
            if args.max_size and expected_size > args.max_size:
                print(f"too large for the server: {path} ({expected_size} bytes)")
                broken = True
            if not path.is_file():
                print(f"missing: {path} of {entry['path']}")
                broken = True
                continue
            content = path.read_bytes()
            if len(content) != expected_size or hashlib.sha256(content).hexdigest() != expected_digest:
                print(f"corrupt: {path} of {entry['path']}")
                broken = True
            whole.update(content)
            size += len(content)
        if not broken and (size != entry["size"] or whole.hexdigest() != entry["sha256"]):
            print(f"mismatch: {entry['path']} does not reassemble to its size and hash")
            broken = True
        problems += broken
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

    build = subparsers.add_parser(
        "build", help="split the game into chunks, write and sign manifest.json")
    build.add_argument("game_dir", help="directory holding the game as it is to be installed")
    build.add_argument("publish_dir",
                       help="where manifest.json, manifest.json.sig and chunks/ are written")
    build.add_argument("--key", required=True, help="private key file from `keygen`")
    build.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE, metavar="BYTES",
                       help="files larger than this are split into chunks of at most this "
                            f"many bytes (default {DEFAULT_CHUNK_SIZE})")
    build.add_argument("--keep-unreferenced", action="store_true",
                       help="keep chunks below publish_dir that the new manifest does not use")
    build.add_argument("--launch-windows", metavar="PATH",
                       help="program (below game_dir) the Windows launcher starts")
    build.add_argument("--launch-linux", metavar="PATH",
                       help="program (below game_dir) the Linux launcher starts")
    build.set_defaults(func=command_build)

    verify = subparsers.add_parser("verify", help="check signature and files of a publish dir")
    verify.add_argument("publish_dir")
    verify.add_argument("--public-key", required=True, help="hex encoded public key")
    verify.add_argument("--max-size", type=int, default=DEFAULT_CHUNK_SIZE, metavar="BYTES",
                        help="report chunks larger than this "
                             f"(default {DEFAULT_CHUNK_SIZE}, 0 to disable)")
    verify.set_defaults(func=command_verify)

    args = parser.parse_args()
    _self_test()
    args.func(args)


if __name__ == "__main__":
    main()
