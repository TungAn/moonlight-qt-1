#!/usr/bin/env python3
"""Run only against isolated Xvfb servers, never the user's desktop roots."""
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile


def server(binary, auth, directory):
    read_fd, write_fd = os.pipe()
    log = open(Path(directory) / f"xvfb-{read_fd}.log", "w+")
    process = subprocess.Popen(
        [binary, "-displayfd", str(write_fd), "-screen", "0", "64x64x24", "-nolisten", "tcp", "-noreset", "-auth", auth],
        pass_fds=[write_fd], stdout=log, stderr=log,
    )
    os.close(write_fd)
    try:
        if not select.select([read_fd], [], [], 10)[0]:
            raise RuntimeError("Xvfb did not publish a display")
        number = os.read(read_fd, 64).decode().strip()
        if not number.isdigit():
            log.seek(max(0, log.seek(0, os.SEEK_END) - 4096))
            raise RuntimeError(log.read())
        return process, ":" + number
    except BaseException:
        process.terminate()
        process.wait()
        raise
    finally:
        os.close(read_fd)
        log.close()


def main():
    xvfb, fixture = sys.argv[1:]
    processes = []
    with tempfile.TemporaryDirectory(prefix="pyrolight-gamescope-") as directory:
        auth = Path(directory) / "Xauthority"
        # Wildcard local authority is restricted to these disposable servers.
        fields = [b"", b"", b"MIT-MAGIC-COOKIE-1", os.urandom(16)]
        auth.write_bytes(struct.pack(">H", 65535) + b"".join(struct.pack(">H", len(value)) + value for value in fields))
        try:
            root, root_display = server(xvfb, str(auth), directory)
            processes.append(root)
            game, game_display = server(xvfb, str(auth), directory)
            processes.append(game)
            environment = dict(os.environ, DISPLAY=game_display, XAUTHORITY=str(auth), SDL_VIDEODRIVER="dummy")
            subprocess.run([fixture, root_display, game_display, str(root.pid)], env=environment, check=True, timeout=30)
        finally:
            for process in processes:
                process.terminate()
            for process in processes:
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
