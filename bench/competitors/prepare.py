#!/usr/bin/env python3
"""Fetch exact competitor revisions and record reproducible build availability."""
import argparse
import json
import pathlib
import shutil
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent


def run(command, folder, log, timeout=300):
    with log.open("w") as output:
        try:
            return subprocess.run(
                command, cwd=folder, stdout=output, stderr=subprocess.STDOUT, timeout=timeout
            ).returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            output.write(str(error) + "\n")
            return 127


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=pathlib.Path, default=pathlib.Path("build-competitors"))
    parser.add_argument(
        "--cmake-arg",
        action="append",
        default=[],
        help="Additional CMake dependency locations, e.g. -DCMAKE_PREFIX_PATH=...",
    )
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    pins = json.loads((ROOT / "pins.json").read_text())
    report = []
    adapters = {}
    for name, pin in pins.items():
        checkout = directory / ("tgbot" if name == "egorpugin/tgbot" else name)
        if not (checkout / ".git").exists():
            subprocess.run(["git", "init", str(checkout)], check=True, capture_output=True)
            subprocess.run(
                ["git", "-C", str(checkout), "remote", "add", "origin", pin["repository"]],
                check=True,
            )
        subprocess.run(
            ["git", "-C", str(checkout), "fetch", "--depth", "1", "origin", pin["revision"]],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(checkout), "checkout", "--detach", pin["revision"]], check=True
        )
        begin = time.monotonic()
        build = directory / (name.replace("/", "-") + "-adapter")
        configure_log = directory / (name.replace("/", "-") + "-configure.log")
        build_log = directory / (name.replace("/", "-") + "-build.log")
        if name == "tgbot-cpp":
            result = run(
                [
                    "cmake",
                    "-S",
                    str(ROOT),
                    "-B",
                    str(build),
                    "-DCMAKE_BUILD_TYPE=Release",
                    f"-DTGBOT_CPP_SOURCE={checkout}",
                    *args.cmake_arg,
                ],
                directory,
                configure_log,
            )
            if result == 0:
                result = run(
                    ["cmake", "--build", str(build), "--parallel", "2"], directory, build_log, 1200
                )
            executable = build / (
                "tgbot_cpp_bench.exe" if __import__("os").name == "nt" else "tgbot_cpp_bench"
            )
            if result == 0:
                adapters[name] = {
                    "revision": pin["revision"],
                    "command": [str(executable), "{base}", "{requests}", "{concurrency}"],
                }
        elif name == "TGBM":
            # Respect the host's no-Boost-download policy; a full package can be
            # tested manually on a machine with the upstream dependencies.
            result = run(
                [
                    "cmake",
                    "-S",
                    str(checkout),
                    "-B",
                    str(build),
                    "-DTGBM_USE_CPM=OFF",
                    *args.cmake_arg,
                ],
                directory,
                configure_log,
            )
            if result == 0:
                result = run(
                    ["cmake", "--build", str(build), "--parallel", "2"], directory, build_log
                )
        else:
            result = run(["sw", "build"], checkout, build_log)
        report.append(
            {
                "library": name,
                **pin,
                "build_seconds": time.monotonic() - begin,
                "exit_code": result,
                "status": "adapter-ready" if name in adapters else "unsupported",
                "configure_log": str(configure_log),
                "build_log": str(build_log),
                "reason": (
                    "" if name in adapters else "See concrete build logs; not a performance result"
                ),
            }
        )
    for entry in report:
        if entry["library"] not in adapters:
            adapters[entry["library"]] = {
                key: entry[key]
                for key in ("revision", "status", "reason", "configure_log", "build_log")
            }
    (directory / "adapters.json").write_text(json.dumps(adapters, indent=2) + "\n")
    (directory / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(directory / "build-report.json")


if __name__ == "__main__":
    main()
