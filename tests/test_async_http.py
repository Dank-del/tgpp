#!/usr/bin/env python3
"""Local, token-free async HTTP integration test."""
import http.server
import json
import subprocess
import sys
import threading
import time
from unittest.mock import patch

from local_http_server import LocalHTTPServer


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, payload):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.send(200, b"d" * (1024 * 1024))

    def do_POST(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            size = 0
            while True:
                length = int(self.rfile.readline().split(b";", 1)[0], 16)
                if not length:
                    self.rfile.readline()
                    break
                size += len(self.rfile.read(length))
                self.rfile.read(2)
        else:
            size = len(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        method = self.path.rsplit("/", 1)[-1]
        if method == "slow":
            time.sleep(0.15)
        if method == "badjson":
            self.send(200, b"not json")
        elif method == "servererror":
            self.send(503, b"unavailable")
        else:
            result = (
                {"id": 123, "is_bot": True, "first_name": "test"} if method == "getMe" else size
            )
            self.send(200, json.dumps({"ok": True, "result": result}).encode())


if __name__ == "__main__":
    with patch("socket.getfqdn", side_effect=AssertionError("loopback fixture attempted DNS")):
        server = LocalHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        process = subprocess.Popen([sys.argv[1], f"http://127.0.0.1:{server.server_port}"])
        try:
            result = process.wait(timeout=20)
            if result:
                raise subprocess.CalledProcessError(result, process.args)
        except subprocess.TimeoutExpired:
            if sys.platform == "darwin":
                try:
                    subprocess.run(
                        [
                            "lldb",
                            "--batch",
                            "-p",
                            str(process.pid),
                            "-o",
                            "thread backtrace all",
                            "-o",
                            "process detach",
                        ],
                        timeout=5,
                        check=False,
                    )
                except (OSError, subprocess.TimeoutExpired):
                    pass
            process.kill()
            process.wait()
            raise
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
