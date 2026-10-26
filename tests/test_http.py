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

PAYLOAD = b"binary\x00payload\xff\r\n"
MESSAGE = {"message_id": 1, "date": 1700000000, "chat": {"id": 42, "type": "private"}}


class Handler(http.server.BaseHTTPRequestHandler):
    errors = []
    seen = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            assert self.path.startswith("/bot123:test_token/")
            method = self.path.rsplit("/", 1)[1]
            self.seen.append(method)
            raw = self.rfile.read(int(self.headers["Content-Length"]))
            content_type = self.headers["Content-Type"]
            if content_type.startswith("multipart/form-data"):
                message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                    b"Content-Type: " + content_type.encode() + b"\r\nMIME-Version: 1.0\r\n\r\n" + raw)
                parts = {part.get_param("name", header="content-disposition"): part for part in message.iter_parts()}
                assert parts["chat_id"].get_payload(decode=True) == b"42"
                if method == "sendDocument":
                    assert parts["document"].get_payload(decode=True) == b"attach://upload_0"
                    assert parts["upload_0"].get_filename() == "report.bin"
                    assert parts["upload_0"].get_payload(decode=True) == PAYLOAD
                elif method == "sendMediaGroup":
                    media = json.loads(parts["media"].get_payload(decode=True))
                    assert media == [{"type": "photo", "media": "attach://photo"},
                                     {"type": "video", "media": "attach://video"}]
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
                    assert parameters == {"chat_id": 42, "text": "Hello, 世界", "disable_notification": False}
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
                response = {"ok": False, "error_code": 429, "description": "retry later", "parameters": {"retry_after": 7}}
            body = b"not JSON" if method == "badJson" else json.dumps(response).encode()
        except Exception as error:
            self.errors.append(repr(error))
            status, body = 500, b"{}"
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    with tempfile.TemporaryDirectory() as directory:
        upload = pathlib.Path(directory) / "upload.bin"
        upload.write_bytes(PAYLOAD)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}", str(upload)], timeout=30)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        assert not Handler.errors, Handler.errors
        assert result.returncode == 0, result.returncode
        assert Handler.seen == ["getMe", "sendMessage", "sendDocument", "sendMediaGroup", "getChat", "badJson"], Handler.seen


if __name__ == "__main__":
    main()
