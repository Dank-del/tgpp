import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / "tools/generate.py"
module = importlib.util.spec_from_file_location("generate", path)
generate = importlib.util.module_from_spec(module)
module.loader.exec_module(generate)


class GeneratorTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
