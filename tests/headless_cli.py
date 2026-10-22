#!/usr/bin/env python3
"""Exercise the real CLI with no usable Qt/SDL video platform.

Run: python3 tests/headless_cli.py /absolute/path/to/pyrolight
The fixture uses a temporary pinned certificate; user settings are never read.
Requires Python 3 and openssl. Works with an AppImage via APPIMAGE_EXTRACT_AND_RUN=1.
"""
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import urlsplit

BINARY = str(Path(sys.argv.pop(1)).resolve())
HOST_ID = "00000000-0000-4000-8000-000000000001"
PNG = (Path(__file__).resolve().parents[1] / "app/res/no_app_image.png").read_bytes()


class HeadlessCliTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pyrolight-headless-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        self.env = {key: value for key, value in os.environ.items()
                    if key not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE")}
        self.env.update(XDG_CONFIG_HOME=str(self.root / "config"),
                        XDG_CACHE_HOME=str(self.root / "cache"),
                        XDG_DATA_HOME=str(self.root / "data"),
                        XDG_RUNTIME_DIR=str(runtime),
                        QT_QPA_PLATFORM="pyrodeck-invalid-platform",
                        SDL_VIDEODRIVER="pyrodeck-invalid-driver",
                        APPIMAGE_EXTRACT_AND_RUN="1")
        self.requests = []
        self.paired = True
        cert, key = self.root / "cert.pem", self.root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-subj", "/CN=localhost", "-days", "1", "-keyout", str(key),
                        "-out", str(cert)], check=True, capture_output=True)
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def handle(self):
                try:
                    super().handle()
                except (ConnectionError, ssl.SSLError):
                    pass  # Qt can abort a redundant polling request on CLI exit.

            def do_GET(self):
                path = urlsplit(self.path).path
                fixture.requests.append((isinstance(self.connection, ssl.SSLSocket), path))
                if path == "/serverinfo":
                    body = (f'<root status_code="200"><hostname>Fixture PC</hostname>'
                            f'<uniqueid>{HOST_ID}</uniqueid><HttpsPort>{fixture.https.server_port}</HttpsPort>'
                            f'<PairStatus>{int(fixture.paired)}</PairStatus><currentgame>0</currentgame>'
                            '<appversion>7.1.431.0</appversion><state>MOONSHINE_SERVER_FREE</state></root>').encode()
                elif path == "/applist":
                    body = (b'<root status_code="200"><App><AppTitle>Fixture game</AppTitle><ID>42</ID>'
                            b'<PyroshineSource>steam:620</PyroshineSource></App></root>')
                elif path == "/appasset":
                    body = PNG
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/png" if path == "/appasset" else "application/xml")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.https = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.https.socket = context.wrap_socket(self.https.socket, server_side=True)
        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        for server in (self.http, self.https):
            threading.Thread(target=server.serve_forever, daemon=True).start()
            self.addCleanup(server.server_close)
            self.addCleanup(server.shutdown)
        settings = self.root / "config/Moonlight Game Streaming Project/Moonlight.conf"
        settings.parent.mkdir(parents=True)
        pem = cert.read_text().replace("\n", "\\n")
        settings.write_text(f"[hosts]\nsize=1\n1\\hostname=Fixture PC\n1\\uuid={HOST_ID}\n"
                            f"1\\localaddress=127.0.0.1\n1\\localport={self.http.server_port}\n"
                            f"1\\srvcert=@ByteArray({pem})\n1\\nvidiasw=false\n")

    def run_cli(self, *args):
        return subprocess.run([BINARY, *args], env=self.env, cwd=self.root,
                              capture_output=True, text=True, timeout=45)

    def document(self, *args):
        result = self.run_cli("list", "--json", *args, HOST_ID)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout.strip().splitlines()[-1])

    def test_catalog_artwork_and_network_check_without_a_display(self):
        catalog = self.document()
        self.assertEqual(catalog["host"]["uuid"], HOST_ID)
        self.assertEqual(catalog["apps"][0]["id"], 42)
        self.assertEqual(catalog["apps"][0]["sources"], ["steam:620"])
        art = self.document("--artwork-id", "42")
        self.assertTrue(art["image"].startswith("data:image/png;base64,"))
        self.assertEqual(art["hostUuid"], HOST_ID)
        network = self.document("--network-action", "check")
        self.assertTrue(network["ok"])
        self.assertEqual(network["hostUuid"], HOST_ID)
        self.assertIn((True, "/applist"), self.requests)
        self.assertIn((True, "/appasset"), self.requests)
        self.assertNotIn((False, "/applist"), self.requests)
        self.assertNotIn((False, "/appasset"), self.requests)

    def test_unpaired_host_is_refused_without_a_display(self):
        self.paired = False
        result = self.run_cli("list", "--json", HOST_ID)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has not been paired", result.stderr)
        self.assertNotIn("could be initialized", result.stderr)
        self.assertNotIn((True, "/appasset"), self.requests)
        self.assertNotIn((True, "/applist"), self.requests)

    def test_help_and_capability_queries_without_a_display(self):
        for args in (["--help"], ["--version"], ["stream", "--help"],
                     ["--video-codec", "PyroWave", "stream", "--help"],
                     ["quit", "--help"], ["pair", "--help"], ["list", "--help"]):
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("platform plugin", result.stderr)

        # A help-looking token consumed as an option value must not dereference
        # the null preferences used by the early capability-query path.
        result = self.run_cli("stream", "--settings-revision", "--help")
        self.assertGreater(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
