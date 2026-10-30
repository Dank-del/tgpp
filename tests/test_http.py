"""Exercise the real curl transport without Telegram credentials or internet."""

import email.parser
import email.policy
import http.server
import json
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import patch

from local_http_server import LocalHTTPServer

PAYLOAD = b"binary\x00payload\xff\r\n"
MESSAGE = {"message_id": 1, "date": 1700000000, "chat": {"id": 42, "type": "private"}}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    errors = []
    seen = []
    ports = set()
    peer_ports = []
    guard = threading.Lock()
    active = 0
    max_active = 0

    def log_message(self, *_):
        pass

    def do_POST(self):
        with self.guard:
            type(self).active += 1
            type(self).max_active = max(self.max_active, self.active)
        try:
            assert self.path.startswith("/bot123:test_token/")
            method = self.path.rsplit("/", 1)[1]
            self.seen.append(method)
            self.ports.add(self.client_address[1])
            self.peer_ports.append(self.client_address[1])
            if method == "getMe":
                time.sleep(0.05)  # Keep concurrent requests overlapping.
            raw = self.rfile.read(int(self.headers["Content-Length"]))
            content_type = self.headers["Content-Type"]
            if content_type.startswith("multipart/form-data"):
                message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                    b"Content-Type: "
                    + content_type.encode()
                    + b"\r\nMIME-Version: 1.0\r\n\r\n"
                    + raw
                )
                parts = {
                    part.get_param("name", header="content-disposition"): part
                    for part in message.iter_parts()
                }
                assert parts["chat_id"].get_payload(decode=True) == b"42"
                if method == "sendDocument":
                    assert parts["document"].get_payload(decode=True) == b"attach://upload_0"
                    assert parts["upload_0"].get_filename() == "report.bin"
                    assert parts["upload_0"].get_payload(decode=True) == PAYLOAD
                elif method == "sendMediaGroup":
                    media = json.loads(parts["media"].get_payload(decode=True))
                    assert media == [
                        {"type": "photo", "media": "attach://photo"},
                        {"type": "video", "media": "attach://video"},
                    ]
                    assert parts["photo"].get_payload(decode=True) == PAYLOAD
                    assert parts["video"].get_payload(decode=True) == PAYLOAD
                else:
                    raise AssertionError("unexpected multipart method")
            else:
                assert content_type == "application/json"
                parameters = json.loads(raw)
                if method == "getMe":
                    assert parameters == {}
                elif method == "sendMessage":
                    assert parameters == {
                        "chat_id": 42,
                        "text": "Hello, 世界",
                        "disable_notification": False,
                    }
            status = 200
            if method == "getMe":
                result = {"id": 123, "is_bot": True, "first_name": "test"}
            elif method == "sendMediaGroup":
                result = [MESSAGE, MESSAGE]
            else:
                result = MESSAGE
            response = {"ok": True, "result": result}
            if method == "getChat":
                status = 429
                response = {
                    "ok": False,
                    "error_code": 429,
                    "description": "retry later",
                    "parameters": {"retry_after": 7},
                }
            body = b"not JSON" if method == "badJson" else json.dumps(response).encode()
        except Exception as error:
            self.errors.append(repr(error))
            status, body = 500, b"{}"
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        with self.guard:
            type(self).active -= 1


def main():
    with tempfile.TemporaryDirectory() as directory:
        upload = pathlib.Path(directory) / "upload.bin"
        upload.write_bytes(PAYLOAD)
        with patch("socket.getfqdn", side_effect=AssertionError("loopback fixture attempted DNS")):
            server = LocalHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run(
                [sys.argv[1], f"http://127.0.0.1:{server.server_port}", str(upload)], timeout=30
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        assert not Handler.errors, Handler.errors
        assert result.returncode == 0, result.returncode
        assert Handler.seen[:6] == [
            "getMe",
            "sendMessage",
            "sendDocument",
            "sendMediaGroup",
            "getChat",
            "badJson",
        ], Handler.seen
        assert Handler.seen[6:] == ["getMe"] * 8, Handler.seen
        assert (
            len(set(Handler.peer_ports[:6])) == 1
        ), "sequential calls should reuse their HTTP connection"
        assert (
            Handler.max_active == 2
        ), "parallel HTTP calls must overlap and respect the configured pool bound"
        assert len(Handler.ports) == 2, "the two pooled handles should reuse their connections"


if __name__ == "__main__":
    main()
