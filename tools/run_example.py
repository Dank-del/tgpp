#!/usr/bin/env python3
"""Run a built example with the ignored, local .env file (no third-party deps)."""
import argparse
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = {
    "echo_bot": "build/echo_bot",
    "modular_bot": "build/examples/modular_bot/modular_bot",
    "send_message": "build/send_message",
    "showcase_bot": "build/examples/showcase_bot/showcase_bot",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", choices=EXAMPLES)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    environment = os.environ.copy()
    local = ROOT / ".env"
    if local.exists():
        for number, line in enumerate(local.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, separator, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
                parser.error(f"invalid .env assignment on line {number}")
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            # Parse values as data, never shell code; .env is the explicit local
            # configuration and takes precedence over the inherited environment.
            environment[key] = value
    executable = ROOT / EXAMPLES[args.example]
    if not executable.is_file():
        parser.error("build the examples first with TGPP_BUILD_EXAMPLES=ON")
    os.chdir(ROOT)
    os.execve(str(executable), [str(executable), *args.arguments], environment)


if __name__ == "__main__":
    main()
