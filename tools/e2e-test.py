#!/usr/bin/env python3
"""End-to-end test of a launcher binary against a local HTTP server.

    ./tools/e2e-test.py dist/igwlegacy-launcher-linux-64

Publishes a small game with tools/manifest.py, serves it on 127.0.0.1 and runs
the launcher headless against it: initial download, an up-to-date rerun,
repair of a locally modified file, removal of a file the manifest dropped,
the launch step, the server URL saved in igwlegacy-launcher-config.json
next to the binary, and refusal of a tampered manifest, a wrong key and a
server file that does not match its hash.
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
        files = publish / "files"
        game = work / "game"
        key = work / "signing.key"
        public_key = manifest_tool("keygen", str(key)).strip().splitlines()[-1]

        # A small game: nested directories, a non-ASCII name with spaces, a
        # file large enough for several progress callbacks, and a start script.
        (files / "data" / "sub").mkdir(parents=True)
        (files / "readme.txt").write_text("hello\n")
        (files / "data" / "big.bin").write_bytes(os.urandom(3 * 1024 * 1024))
        (files / "data" / "sub" / "näme with space.txt").write_text("ä\n")
        start = files / "start.sh"
        start.write_text('#!/bin/sh\necho started > "$(dirname "$0")/started.txt"\n')
        start.chmod(0o755)

        def publish_manifest():
            manifest_tool("build", str(publish), "--key", str(key), "--launch-linux", "start.sh")

        publish_manifest()

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
                  ["readme.txt", "data/big.bin", "data/sub/näme with space.txt", "start.sh"]),
              "all files downloaded intact")
        check(os.access(game / "start.sh", os.X_OK), "executable bit set")

        code, output = run("--no-launch")
        check(code == 0 and "All files are up to date" in output, "rerun finds nothing to do")

        (game / "readme.txt").write_text("modified\n")
        code, output = run("--no-launch")
        check(code == 0 and same("readme.txt") and "1 file needs" in output,
              "locally modified file is repaired")

        shutil.rmtree(files / "data" / "sub")
        publish_manifest()
        (game / "own-file.txt").write_text("not from the manifest\n")
        code, output = run("--no-launch")
        check(code == 0 and not (game / "data" / "sub").exists(),
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

        (files / "readme.txt").write_text("HELLO\n")
        (game / "readme.txt").unlink()
        code, output = run("--no-launch")
        check(code != 0 and not (game / "readme.txt").exists(),
              "server file not matching its hash is refused and not installed")

        server.shutdown()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        sys.exit(f"{len(failures)} check(s) failed")
    print("all checks passed")


if __name__ == "__main__":
    main()
