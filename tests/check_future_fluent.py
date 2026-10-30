#!/usr/bin/env python3
"""Compile stable fluent usage against a synthetic future API in an isolated tree."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from generate import Generator, format_generated  # noqa: E402


FUTURE_CONTRACT = r"""
#include <tgpp/api_builders.hpp>
#include <concepts>
#include <utility>

static_assert(std::same_as<
    decltype(std::declval<tgpp::AsyncBot>().api()
                 .future_action().chat_id(42).label("future")
                 .set_send("ok").requires_(false).set_mark_required(7).send()),
    tgpp::Task<bool>>);
static_assert(std::same_as<
    decltype(std::declval<tgpp::AsyncBot>().api()
                 .send_message().chat_id(42).text("hello").future_flag(false).send()),
    tgpp::Task<tgpp::Message>>);

tgpp::Task<void> future_api_source_compatibility(tgpp::AsyncBot bot) {
    // This method and these setters exist only in the synthetic specification.
    const auto accepted = co_await bot.api().future_action().chat_id(42).label("x")
        .set_send("ok").requires_(false).set_mark_required(7).send();
    // Request member escaping and fluent setter escaping are independent: the
    // JSON field `send` must not steal the terminal send() operation.
    tgpp::FutureActionRequest raw{};
    raw.chat_id = std::int64_t(42);
    raw.send = "ok";
    raw.requires_ = false;
    raw.mark_required = 7;
    co_await bot.api().future_action().request(std::move(raw)).send();
    const auto message = co_await bot.api().send_message()
        .chat_id(42).text("hello").future_flag(false).send();
    co_await bot.api().send_message()
        .chat_id(std::string("@channel")).text("hello").future_flag(std::nullopt).send();
    static_cast<void>(accepted);
    static_cast<void>(message);
}
"""


def future_spec():
    spec = copy.deepcopy(json.loads((ROOT / "spec/api.json").read_text(encoding="utf-8")))
    if "futureAction" in spec["methods"] or any(
        field["name"] == "future_flag" for field in spec["methods"]["sendMessage"]["fields"]
    ):
        raise SystemExit("Rename the synthetic future fixture: it now collides with the real API")
    spec["methods"]["sendMessage"]["fields"].append(
        {
            "name": "future_flag",
            "types": ["Boolean"],
            "required": False,
            "description": "Synthetic optional field used only by the forward-compatibility test.",
        }
    )
    spec["methods"]["futureAction"] = {
        "name": "futureAction",
        "href": "https://example.invalid/futureAction",
        "description": ["Synthetic method used only by the forward-compatibility test."],
        "returns": ["Boolean"],
        "fields": [
            {
                "name": "chat_id",
                "types": ["Integer", "String"],
                "required": True,
                "description": "Target chat.",
            },
            {
                "name": "label",
                "types": ["String"],
                "required": False,
                "description": "An optional label.",
            },
            {
                "name": "send",
                "types": ["String"],
                "required": False,
                "description": "Synthetic field colliding with the builder terminal.",
            },
            {
                "name": "requires",
                "types": ["Boolean"],
                "required": False,
                "description": "Synthetic C++20 keyword field.",
            },
            {
                "name": "mark_required",
                "types": ["Integer"],
                "required": False,
                "description": "Synthetic field colliding with a builder internal.",
            },
        ],
    }
    return spec


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--compiler-style", choices=["unix", "msvc"], default="unix")
    parser.add_argument("--include", nargs="+", default=[])
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build-artifacts")
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    spec = future_spec()
    digest = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    # Only headers are needed by the syntax-only compile. Keep all code generation
    # in memory and never refresh the real schema, lock, or checked-in SDK files.
    outputs = Generator(spec, digest).generate()
    outputs = format_generated(
        {name: source for name, source in outputs.items() if name.startswith("include/")}
    )
    if "include/tgpp/api_builders.hpp" not in outputs:
        raise SystemExit("Generator did not produce schema-derived fluent builders")
    with tempfile.TemporaryDirectory(prefix="tgpp-future-api-", dir=args.work_dir) as folder:
        work = Path(folder)
        shutil.copytree(ROOT / "include/tgpp", work / "include/tgpp")
        for name, source in outputs.items():
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.encode("utf-8"))
        fixture = work / "future_fluent_contract.cpp"
        # The existing user-style fixture is deliberately kept unchanged: it must
        # compile in the very same translation unit as the new API usage.
        fixture.write_bytes(
            (
                (ROOT / "tests/fluent_contract.cpp").read_text(encoding="utf-8") + FUTURE_CONTRACT
            ).encode("utf-8")
        )
        includes = [work / "include", *(Path(value) for value in args.include if value)]
        if args.compiler_style == "msvc":
            command = [
                args.compiler,
                "/nologo",
                "/std:c++20",
                "/utf-8",
                "/DNOMINMAX",
                "/DWIN32_LEAN_AND_MEAN",
                "/EHsc",
                "/permissive-",
                "/Zc:__cplusplus",
                "/Zs",
                "/DASIO_STANDALONE",
                "/DASIO_NO_DEPRECATED",
                *[f"/I{path}" for path in includes],
                str(fixture),
            ]
        else:
            command = [
                args.compiler,
                "-std=c++20",
                "-fsyntax-only",
                "-pthread",
                "-DASIO_STANDALONE",
                "-DASIO_NO_DEPRECATED",
                *[f"-I{path}" for path in includes],
                str(fixture),
            ]
        subprocess.run(command, cwd=work, check=True, timeout=150)
    print("Stable fluent contract and synthetic future method/field compile together")


if __name__ == "__main__":
    main()
