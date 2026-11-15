#!/usr/bin/env python3
"""End-to-end test of a launcher binary against a local HTTP server.

    ./tools/e2e-test.py dist/igwlegacy-launcher

Publishes a small game with tools/manifest.py, using a small chunk size so
that some files are published as a single chunk and others split into
several, serves it on 127.0.0.1 and runs the launcher headless against it:
initial download and reassembly with modification times and executable bit,
an up-to-date rerun, repair of a locally modified file and of a changed
modification time, a file growing beyond the chunk size between releases,
removal of a file the manifest dropped, the launch step, the server URL
saved in igwlegacy-launcher-config.json next to the binary, and refusal of a
tampered manifest, a wrong key, and a chunk on the server that does not
match its hash.
"""

import functools
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent

# Small enough that the test game needs splitting without being large.
CHUNK_SIZE = 1000 * 1000


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def manifest_tool(*args):
    return subprocess.run([sys.executable, "-I", str(TOOLS / "manifest.py"), *args],
                          check=True, capture_output=True, text=True).stdout


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    launcher = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="igwl-e2e-"))
    failures = []

    def check(condition, what):
        print(("ok    " if condition else "FAIL  ") + what)
        if not condition:
            failures.append(what)

    try:
        publish = work / "publish"
        # The game as published, and the directory the launcher installs it to.
        files = work / "source"
        game = work / "game"
        key = work / "signing.key"
        public_key = manifest_tool("keygen", str(key)).strip().splitlines()[-1]

        # A small game: nested directories, a non-ASCII name with spaces, an
        # empty file, a file of exactly the chunk size (published whole),
        # files split into several chunks with and without a short last one,
        # an identical copy of a split file, and a start script.
        (files / "data" / "sub").mkdir(parents=True)
        (files / "readme.txt").write_text("hello\n")
        (files / "empty").write_bytes(b"")
        (files / "data" / "exact.bin").write_bytes(os.urandom(CHUNK_SIZE))
        (files / "data" / "big.bin").write_bytes(os.urandom(3 * 1024 * 1024))
        (files / "data" / "even.bin").write_bytes(os.urandom(2 * CHUNK_SIZE))
        shutil.copy(files / "data" / "big.bin", files / "data" / "big copy.bin")
        (files / "data" / "sub" / "näme with space.txt").write_text("ä\n")
        start = files / "start.sh"
        start.write_text('#!/bin/sh\necho started > "$(dirname "$0")/started.txt"\n')
        start.chmod(0o755)
        # A distinct, known modification time to look for after the sync.
        os.utime(files / "readme.txt", (1500000000, 1500000000))

        def publish_manifest():
            manifest_tool("build", str(files), str(publish), "--key", str(key),
                          "--chunk-size", str(CHUNK_SIZE), "--launch-linux", "start.sh")

        def published_manifest():
            return json.loads((publish / "manifest.json").read_text())

        def entry(relative):
            return next(e for e in published_manifest()["files"] if e["path"] == relative)

        def chunk_file(digest):
            return publish / "chunks" / digest

        publish_manifest()
        manifest_tool("verify", str(publish), "--public-key", public_key,
                      "--max-size", str(CHUNK_SIZE))
        published = sorted(path.relative_to(publish).as_posix()
                           for path in publish.rglob("*"))
        check(all(path.stat().st_size <= CHUNK_SIZE
                  for path in publish.rglob("*") if path.is_file()),
              "no published file exceeds the chunk size")
        check(all(name in ("chunks", "manifest.json", "manifest.json.sig")
                  or (name.startswith("chunks/") and len(name) == len("chunks/") + 64)
                  for name in published),
              "only chunks named after their hash are published, no file names")
        check(published_manifest()["format"] == 2
              and all("chunks" not in entry(name) for name in ["readme.txt", "data/exact.bin"])
              and chunk_file(entry("readme.txt")["sha256"]).read_bytes() == b"hello\n"
              and entry("readme.txt")["mtime"] == 1500000000,
              "files up to the chunk size are published as a single chunk, with mtime")
        check("chunks" not in entry("empty") and chunk_file(entry("empty")["sha256"]) not in
              [publish / name for name in published],
              "an empty file needs no chunk")
        check([len(entry(name)["chunks"]) for name in ["data/big.bin", "data/even.bin"]] == [4, 2]
              and not chunk_file(entry("data/big.bin")["sha256"]).exists(),
              "larger files are split into chunks and not published whole")
        # Chunks: readme, exact, start.sh, 4 of big (shared with its copy),
        # 2 of even, plus the one of the non-ASCII file.
        check(entry("data/big.bin")["chunks"] == entry("data/big copy.bin")["chunks"]
              and len(list((publish / "chunks").iterdir())) == 10,
              "identical chunks are stored once")

        server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(QuietHandler, directory=str(publish)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base_url = f"http://127.0.0.1:{server.server_address[1]}/"

        def run(*extra, key_hex=public_key):
            result = subprocess.run(
                [str(launcher), "--headless", "--dir", str(game), "--base-url", base_url,
                 "--public-key", key_hex, *extra],
                capture_output=True, text=True, timeout=120)
            return result.returncode, result.stdout + result.stderr

        def same(relative):
            local = game / relative
            return (local.is_file() and hashlib.sha256(local.read_bytes()).digest()
                    == hashlib.sha256((files / relative).read_bytes()).digest())

        code, output = run("--no-launch")
        check(code == 0, "initial sync succeeds")
        check(all(same(name) for name in
                  ["readme.txt", "empty", "data/exact.bin", "data/big.bin", "data/big copy.bin",
                   "data/even.bin", "data/sub/näme with space.txt", "start.sh"]),
              "all files downloaded intact, split ones reassembled")
        check(os.access(game / "start.sh", os.X_OK), "executable bit set")
        check(all(int((game / e["path"]).stat().st_mtime) == e["mtime"]
                  for e in published_manifest()["files"])
              and int((game / "readme.txt").stat().st_mtime) == 1500000000,
              "modification times are taken from the manifest")

        code, output = run("--no-launch")
        check(code == 0 and "All files are up to date" in output, "rerun finds nothing to do")

        (game / "readme.txt").write_text("modified\n")
        code, output = run("--no-launch")
        check(code == 0 and same("readme.txt") and "1 file needs" in output,
              "locally modified file is repaired")

        os.utime(game / "data" / "even.bin", (1600000000, 1600000000))
        code, output = run("--no-launch")
        check(code == 0 and "All files are up to date" in output
              and int((game / "data" / "even.bin").stat().st_mtime) == entry("data/even.bin")["mtime"],
              "a changed modification time is restored without downloading")

        (game / "data" / "big.bin").write_bytes(b"x" * (3 * 1024 * 1024))
        code, output = run("--no-launch")
        check(code == 0 and same("data/big.bin") and "1 file needs" in output,
              "locally modified split file is repaired")

        # A release in which a whole file grows beyond the chunk size.
        (files / "data" / "exact.bin").write_bytes(os.urandom(CHUNK_SIZE + 1))
        publish_manifest()
        code, output = run("--no-launch")
        check(code == 0 and same("data/exact.bin") and len(entry("data/exact.bin")["chunks"]) == 2
              and not chunk_file(entry("data/exact.bin")["sha256"]).exists(),
              "file growing beyond the chunk size is split and updated")

        shutil.rmtree(files / "data" / "sub")
        publish_manifest()
        (game / "own-file.txt").write_text("not from the manifest\n")
        code, output = run("--no-launch")
        check(code == 0 and not (game / "data" / "sub").exists()
              and not chunk_file(hashlib.sha256("ä\n".encode()).hexdigest()).exists(),
              "file dropped from the manifest is removed, with its empty directory")
        check((game / "own-file.txt").is_file(), "files never installed by the launcher stay")

        code, output = run()
        # The game is started asynchronously.
        for _ in range(50):
            if (game / "started.txt").is_file():
                break
            time.sleep(0.1)
        check(code == 0 and (game / "started.txt").is_file(), "game is launched after the sync")

        # Installed like a user would: the launcher inside the game directory,
        # no --dir and no --base-url, the server coming from its config file.
        # The manifest also tries to replace that config file, which the
        # launcher must refuse.
        installed = work / "installed"
        installed.mkdir()
        installed_launcher = installed / launcher.name
        shutil.copy2(launcher, installed_launcher)
        config = installed / "igwlegacy-launcher-config.json"
        config_text = json.dumps({"base_url": base_url, "unrelated": True})
        config.write_text(config_text)
        (files / "igwlegacy-launcher-config.json").write_text('{"base_url": "https://evil.invalid/"}')
        publish_manifest()
        result = subprocess.run(
            [str(installed_launcher), "--headless", "--no-launch", "--public-key", public_key],
            capture_output=True, text=True, timeout=120)
        check(result.returncode == 0 and (installed / "readme.txt").is_file(),
              "server URL is taken from igwlegacy-launcher-config.json")
        check(config.read_text() == config_text and "launcher's own file" in result.stdout,
              "manifest cannot overwrite the config file")
        (files / "igwlegacy-launcher-config.json").unlink()
        publish_manifest()

        manifest = publish / "manifest.json"
        original = manifest.read_bytes()
        manifest.write_bytes(original.replace(b'"size": 6', b'"size": 7'))
        code, output = run("--no-launch")
        check(code != 0 and "signature" in output, "tampered manifest is refused")
        manifest.write_bytes(original)

        code, output = run("--no-launch", key_hex="ab" * 32)
        check(code != 0 and "signature" in output, "manifest signed with another key is refused")

        # Tampering below the publish directory, after the manifest is built.
        chunk_file(entry("readme.txt")["sha256"]).write_text("HELLO\n")
        (game / "readme.txt").unlink()
        code, output = run("--no-launch")
        check(code != 0 and not (game / "readme.txt").exists(),
              "server file not matching its hash is refused and not installed")

        chunk = entry("data/even.bin")["chunks"][1]["sha256"]
        chunk_path = chunk_file(chunk)
        chunk_path.write_bytes(b"\0" + chunk_path.read_bytes()[1:])
        (game / "data" / "even.bin").unlink()
        code, output = run("--no-launch")
        check(code != 0 and "chunk 2/2 of data/even.bin" in output
              and not (game / "data" / "even.bin").exists(),
              "chunk not matching its hash is refused and the file not installed")
        check(subprocess.run([sys.executable, "-I", str(TOOLS / "manifest.py"), "verify",
                              str(publish), "--public-key", public_key],
                             capture_output=True).returncode != 0,
              "manifest.py verify notices the corrupt files")

        server.shutdown()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        sys.exit(f"{len(failures)} check(s) failed")
    print("all checks passed")


if __name__ == "__main__":
    main()
