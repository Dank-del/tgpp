#!/usr/bin/env python3
"""Run isolated local HTTP benchmarks; optional adapters follow the same CLI contract."""
import argparse
import http.server
import json
import pathlib
import platform
import subprocess
import socket
import threading
import time


class Simulator(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        time.sleep(self.server.delay)
        body = json.dumps(
            {
                "ok": True,
                "result": {
                    "id": 1,
                    "is_bot": True,
                    "first_name": "Benchmark",
                    "username": "benchmark_bot",
                },
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = do_POST

    def log_message(self, *args):
        pass


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 256
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default="build-async/tgpp_http_bench")
    parser.add_argument("--requests", type=int, default=512)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--delay-ms", type=float, default=50)
    parser.add_argument(
        "--adapters",
        type=pathlib.Path,
        help="JSON mapping library names to {revision, command}; command is argv with {base}, {requests}, {concurrency} placeholders",
    )
    parser.add_argument(
        "--output", type=pathlib.Path, default=pathlib.Path("build-async/http-results.json")
    )
    args = parser.parse_args()
    server = Server(("127.0.0.1", 0), Simulator)
    server.delay = args.delay_ms / 1000
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    adapters = json.loads(args.adapters.read_text()) if args.adapters else {}
    results = []
    try:
        for mode in ("baseline-default", "sync", "async"):
            for concurrency in (1, 8, 32, 128):
                for repeat in range(args.repeats):
                    command = [args.driver, mode, base, str(args.requests), str(concurrency)]
                    completed = subprocess.run(command, text=True, capture_output=True, timeout=180)
                    if completed.returncode:
                        results.append(
                            {
                                "library": "tgpp",
                                "mode": mode,
                                "concurrency": concurrency,
                                "repeat": repeat,
                                "revision": revision,
                                "status": "failed",
                                "stderr": completed.stderr,
                                "stdout": completed.stdout,
                            }
                        )
                        continue
                    result = json.loads(completed.stdout)
                    result.update(revision=revision, repeat=repeat)
                    results.append(result)
        for library in ("tgpp-baseline", "tgbot-cpp", "TGBM", "egorpugin/tgbot"):
            adapter = adapters.get(library)
            if adapter and adapter.get("status") == "unsupported":
                results.append({"library": library, **adapter})
                continue
            if not adapter:
                results.append(
                    {
                        "library": library,
                        "status": "unsupported",
                        "reason": "No built adapter provided; no performance claim made",
                    }
                )
                continue
            if len(adapter["revision"]) != 40 or any(
                c not in "0123456789abcdef" for c in adapter["revision"]
            ):
                raise SystemExit(f"{library}: adapter must record an exact commit SHA")
            for concurrency in (
                (1, 4, 8, 32, 128) if library == "tgpp-baseline" else (1, 8, 32, 128)
            ):
                for repeat in range(args.repeats):
                    values = dict(base=base, requests=args.requests, concurrency=concurrency)
                    command = [part.format(**values) for part in adapter["command"]]
                    completed = subprocess.run(command, text=True, capture_output=True, timeout=180)
                    if completed.returncode:
                        results.append(
                            {
                                "library": library,
                                "revision": adapter["revision"],
                                "status": "failed",
                                "stderr": completed.stderr,
                                "concurrency": concurrency,
                            }
                        )
                        break
                    result = json.loads(completed.stdout)
                    result.update(library=library, revision=adapter["revision"], repeat=repeat)
                    results.append(result)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(
            {
                "platform": platform.platform(),
                "delay_ms": args.delay_ms,
                "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"])),
                "results": results,
            },
            indent=2,
        )
        + "\n"
    )
    print(args.output)


if __name__ == "__main__":
    main()
