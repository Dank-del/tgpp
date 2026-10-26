#!/usr/bin/env python3
"""Format maintained C++, Python, and CMake files; --check performs no writes."""
import argparse
import pathlib
import shutil
import subprocess

from generate import ROOT, clang_format, format_cpp


def files():
    result = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    )
    return sorted(
        {
            pathlib.Path(path.decode())
            for path in result.split(b"\0")
            if path and (ROOT / path.decode()).is_file()
        }
    )


def require_tool(name, version):
    executable = shutil.which(name)
    if not executable:
        raise SystemExit(
            "Install tools with: python3 -m pip install -r tools/requirements-format.txt"
        )
    actual = subprocess.check_output([executable, "--version"], text=True)
    if version not in actual:
        raise SystemExit(f"{name} {version} is required")
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    paths = files()
    cpp = [str(path) for path in paths if path.suffix in {".cpp", ".hpp", ".h", ".cc"}]
    python = [str(path) for path in paths if path.suffix == ".py"]
    cmake = [
        str(path) for path in paths if path.name == "CMakeLists.txt" or path.suffix == ".cmake"
    ]
    cpp_formatter = clang_format()
    if args.check:
        subprocess.run(
            [
                cpp_formatter,
                f"--style=file:{ROOT / '.clang-format'}",
                "--dry-run",
                "--Werror",
                *cpp,
            ],
            cwd=ROOT,
            check=True,
        )
    else:
        for name in cpp:
            path = ROOT / name
            source = path.read_text()
            formatted = format_cpp(source, name, cpp_formatter)
            if formatted != source:
                path.write_text(formatted)

    commands = [
        [
            require_tool("black", "25.1.0"),
            "--config",
            str(ROOT / "pyproject.toml"),
            *(["--check"] if args.check else []),
            *python,
        ],
        [
            require_tool("cmake-format", "0.6.13"),
            "--config-files",
            str(ROOT / ".cmake-format.py"),
            *(["--check"] if args.check else ["-i"]),
            *cmake,
        ],
    ]
    for command in commands:
        subprocess.run(command, cwd=ROOT, check=True)
    print(
        f"{'Checked' if args.check else 'Formatted'} {len(cpp)} C++, "
        f"{len(python)} Python, and {len(cmake)} CMake files"
    )


if __name__ == "__main__":
    main()
