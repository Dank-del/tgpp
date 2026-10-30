import importlib.util
import copy
import json
import pathlib
import os
import subprocess
import sys
import tempfile
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / "tools/generate.py"
module = importlib.util.spec_from_file_location("generate", path)
generate = importlib.util.module_from_spec(module)
module.loader.exec_module(generate)


class GeneratorTests(unittest.TestCase):
    def test_optional_upstream_fields_preserve_fluent_bridge_and_raw_escape_hatch(self):
        spec = json.loads((path.parents[1] / "spec/api.json").read_text(encoding="utf-8"))
        updated = copy.deepcopy(spec)
        updated["methods"]["sendMessage"]["fields"].append(
            {
                "name": "future_optional_field",
                "types": ["String"],
                "required": False,
                "description": "A future optional Telegram field.",
            }
        )
        outputs = generate.Generator(updated, "test").generate()
        raw = outputs["include/tgpp/methods.hpp"]
        self.assertIn("std::optional<std::string> future_optional_field", raw)
        codecs = "".join(
            text for name, text in outputs.items() if name.startswith("src/generated/methods_")
        )
        self.assertIn('"future_optional_field"', codecs)
        async_header = outputs["include/tgpp/async_methods.hpp"]
        self.assertIn("ChatActions to(std::int64_t chat_id) const;", async_header)
        self.assertIn("ChatActions to(std::string chat_id) const;", async_header)
        self.assertIn('#include "fluent.hpp"', async_header)
        self.assertNotIn("class MessageBuilder", async_header)
        self.assertIn("make_request(options, std::move(attachments))", async_header)

    def test_new_methods_and_fields_automatically_receive_typed_builders(self):
        spec = json.loads((path.parents[1] / "spec/api.json").read_text(encoding="utf-8"))
        spec["methods"]["futureAction"] = {
            "href": "https://example.test/futureaction",
            "description": ["A future API method."],
            "returns": ["Boolean"],
            "fields": [
                {
                    "name": "chat_id",
                    "types": ["Integer", "String"],
                    "required": True,
                    "description": "Chat.",
                },
                {
                    "name": "send",
                    "types": ["String"],
                    "required": False,
                    "description": "A field sharing the terminal name.",
                },
                {
                    "name": "requires",
                    "types": ["Boolean"],
                    "required": False,
                    "description": "C++20 keyword field.",
                },
            ],
        }
        outputs = generate.Generator(spec, "test").generate()
        builders = outputs["include/tgpp/api_builders.hpp"]
        self.assertIn("class FutureActionBuilder final", builders)
        self.assertIn(
            "FutureActionBuilder& chat_id(std::variant<std::int64_t, std::string> value)", builders
        )
        self.assertIn("FutureActionBuilder& set_send(std::string value)", builders)
        self.assertIn("FutureActionBuilder& requires_(bool value)", builders)
        self.assertIn("this->builder_request_.send = std::move(value);", builders)
        self.assertIn("api::FutureActionBuilder future_action() const", builders)
        self.assertIn("this->mark_required(0);", builders)
        self.assertEqual(builders.count(" final : public ApiRequestBuilder<"), len(spec["methods"]))
        for name in spec["methods"]:
            builder = name[0].upper() + name[1:] + "Builder"
            self.assertIn(f"class {builder} final", builders)

    def test_ambiguous_fluent_names_fail_before_generating_invalid_cpp(self):
        spec = json.loads((path.parents[1] / "spec/api.json").read_text(encoding="utf-8"))
        source = copy.deepcopy(spec["methods"]["sendMessage"])
        spec["methods"]["api"] = source
        with self.assertRaisesRegex(ValueError, "entry point"):
            generate.Generator(spec, "test").generate()
        del spec["methods"]["api"]
        source["fields"].extend(
            [
                {
                    "name": "send",
                    "types": ["String"],
                    "required": False,
                    "description": "Collision one.",
                },
                {
                    "name": "set_send",
                    "types": ["String"],
                    "required": False,
                    "description": "Collision two.",
                },
            ]
        )
        spec["methods"]["futureAction"] = source
        with self.assertRaisesRegex(ValueError, "setter collision"):
            generate.Generator(spec, "test").generate()

    def test_generated_decode_uses_size_aware_transactional_scratch(self):
        spec = json.loads((path.parents[1] / "spec/api.json").read_text(encoding="utf-8"))
        outputs = generate.Generator(spec, "test").generate()
        codecs = "\n".join(
            text for name, text in outputs.items() if name.startswith("src/generated/")
        )
        self.assertIn("detail::DecodeStorage<Update> storage;", codecs)
        self.assertIn("detail::DecodeStorage<User> storage;", codecs)
        self.assertIn("auto& parsed = storage.value();", codecs)
        self.assertIn("value = std::move(parsed);", codecs)
        self.assertNotIn("Update parsed{};", codecs)

    def test_only_recursive_components_need_boxing(self):
        graph = {"Leaf": set(), "A": {"B", "Leaf"}, "B": {"A"}, "C": {"Leaf"}}
        groups = generate.components(graph)
        self.assertEqual(groups["A"], groups["B"])
        self.assertNotEqual(groups["A"], groups["Leaf"])
        self.assertNotEqual(groups["C"], groups["Leaf"])

    def test_discriminator_is_a_single_literal(self):
        def field(description):
            return {"types": ["String"], "description": description}

        self.assertEqual(generate.literal(field("Type of the media, must be photo")), "photo")
        self.assertEqual(generate.literal(field('Type, always "emoji"')), "emoji")
        self.assertIsNone(generate.literal(field('Can be "one" or "two"')))

    def test_unknown_types_fail_instead_of_becoming_untyped_json(self):
        generator = generate.Generator({"types": {}, "methods": {}}, "test")
        with self.assertRaisesRegex(ValueError, "unknown schema type"):
            generator.type(["NewUpstreamType"])

    def test_recursive_array_is_in_dependency_graph(self):
        node = {"subtypes": ["String", "Array of Node"]}
        generator = generate.Generator({"types": {"Node": node}, "methods": {}}, "test")
        self.assertEqual(generator.graph["Node"], {"Node"})
        self.assertEqual(generator.type(["Array of Node"], "Node"), "std::vector<Box<Node>>")

    def test_mixed_array_elements_are_representable(self):
        generator = generate.Generator({"types": {}, "methods": {}}, "test")
        self.assertEqual(
            generator.type(["Array of String", "Array of Integer"]),
            "std::vector<std::variant<std::string, std::int64_t>>",
        )


class PlatformReproducibilityTests(unittest.TestCase):
    def test_checkout_preserves_pinned_spec_bytes_with_autocrlf(self):
        root = path.parents[1]
        original = (root / "spec/api.json").read_bytes()
        with tempfile.TemporaryDirectory(prefix="tgpp-checkout-") as folder:
            repo = pathlib.Path(folder)
            subprocess.run(["git", "init", "--quiet", str(repo)], check=True, capture_output=True)
            (repo / "spec").mkdir()
            (repo / ".gitattributes").write_bytes((root / ".gitattributes").read_bytes())
            destination = repo / "checkout"
            destination.mkdir()
            for data in (original, original.replace(b"\n", b"\r\n")):
                with self.subTest(crlf=b"\r\n" in data):
                    (repo / "spec/api.json").write_bytes(data)
                    subprocess.run(
                        [
                            "git",
                            "-c",
                            "core.autocrlf=true",
                            "add",
                            ".gitattributes",
                            "spec/api.json",
                        ],
                        cwd=repo,
                        check=True,
                        capture_output=True,
                    )
                    subprocess.run(
                        [
                            "git",
                            "-c",
                            "core.autocrlf=true",
                            "checkout-index",
                            "--all",
                            "--force",
                            "--prefix=" + destination.as_posix() + "/",
                        ],
                        cwd=repo,
                        check=True,
                        capture_output=True,
                    )
                    self.assertEqual((destination / "spec/api.json").read_bytes(), data)

    def test_generator_is_independent_of_default_text_encoding(self):
        environment = os.environ.copy()
        environment.update(LC_ALL="C", PYTHONUTF8="0", PYTHONCOERCECLOCALE="0")
        subprocess.run(
            [sys.executable, str(path), "--check"],
            env=environment,
            cwd=path.parents[1],
            check=True,
            capture_output=True,
        )


if __name__ == "__main__":
    unittest.main()
