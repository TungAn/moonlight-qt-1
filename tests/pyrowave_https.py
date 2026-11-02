#!/usr/bin/env python3
"""Runtime checks of the production NvHTTP HTTPS probe against a local TLS fixture."""
import argparse
import http.server
import ssl
import subprocess
import tempfile
import threading
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("harness", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pyrowave-https-") as temp:
        root = Path(temp)
        for name in ["host", "wrong"]:
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", str(root / f"{name}.key"), "-out", str(root / f"{name}.pem"),
                            "-subj", "/CN=127.0.0.1", "-days", "1"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        for mode, operation, expected in [
            ("valid", "discover", "supported"), ("unsupported", "discover", "unsupported"),
            ("valid", "probe", "success"), ("truncated", "probe", "reject"),
            ("oversized", "probe", "reject"), ("compressed", "probe", "reject"),
            ("redirect", "probe", "reject"), ("wrong-pin", "probe", "reject"),
            ("busy-stream", "probe", "reject"), ("busy-probe", "probe", "reject"),
            ("not-found", "probe", "reject"),
            ("timeout", "probe", "reject"),
            # A conditional quit reaches /cancel only on hosts that advertise
            # the check; older hosts would stop any running game.
            ("cancel-supported", "cancel", "success"),
            ("cancel-unsupported", "cancel", "reject"),
        ]:
            cancels = []

            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_):
                    pass

                def do_GET(self):
                    try:
                        if self.path.startswith("/serverinfo"):
                            xml = '<root status_code="200"><appversion>7.1.431.-1</appversion>'
                            if mode != "unsupported":
                                xml += '<PyroWaveBandwidthProbeBytes>33554432</PyroWaveBandwidthProbeBytes>'
                            if mode == "cancel-supported":
                                xml += '<PyroshineConditionalCancel>1</PyroshineConditionalCancel>'
                            body = (xml + '</root>').encode()
                            self.send_response(200)
                            self.send_header("Content-Length", str(len(body)))
                            self.end_headers()
                            self.wfile.write(body)
                            return
                        if self.path.startswith("/cancel"):
                            cancels.append(self.path)
                            body = b'<root status_code="200"><cancel>1</cancel></root>'
                            self.send_response(200)
                            self.send_header("Content-Length", str(len(body)))
                            self.end_headers()
                            self.wfile.write(body)
                            return
                        if mode == "timeout":
                            time.sleep(11)
                            return
                        if mode in ("busy-stream", "busy-probe", "not-found"):
                            self.send_response({"busy-stream": 409, "busy-probe": 429, "not-found": 404}[mode])
                            self.send_header("Content-Length", "0")
                            self.end_headers()
                            return
                        self.send_response(302 if mode == "redirect" else 200)
                        size = 33554432 + (1 if mode == "oversized" else 0)
                        self.send_header("Content-Length", str(size))
                        if mode == "compressed":
                            self.send_header("Content-Encoding", "gzip")
                        if mode == "redirect":
                            self.send_header("Location", "https://127.0.0.1:1/forbidden")
                        self.end_headers()
                        if mode == "redirect":
                            return
                        left = 65536 if mode == "truncated" else size
                        while left:
                            data = b'P' * min(65536, left)
                            self.wfile.write(data)
                            left -= len(data)
                    except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                        pass

            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.daemon_threads = True
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(root / "host.pem", root / "host.key")
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            certificate = root / ("wrong.pem" if mode == "wrong-pin" else "host.pem")
            try:
                subprocess.run([str(args.harness.resolve()), str(server.server_port), str(certificate),
                                operation, expected], check=True, timeout=20)
                if mode == "cancel-supported" and not any("expectedAppId=42" in path for path in cancels):
                    raise AssertionError("conditional cancel was not sent")
                if mode == "cancel-unsupported" and cancels:
                    raise AssertionError(f"cancel sent to a host without the check: {cancels}")
                print(f"PASS {mode}/{operation}", flush=True)
            finally:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
