#!/usr/bin/env python3
"""Generate the C++17 Bot API surface from a pinned gotgbot-compatible spec.

The default invocation is entirely offline. --update-spec deliberately refreshes
the upstream pin; --check verifies both its hash and all generated files.
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/PaulSonOfLars/telegram-bot-api-spec"
KEYWORDS = set("alignas alignof and and_eq asm auto bitand bitor bool break case catch char char16_t char32_t class compl const constexpr const_cast continue decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq".split())
PRIMITIVES = {"Integer": "std::int64_t", "Float": "double", "Boolean": "bool", "String": "std::string", "True": "bool"}


def identifier(name):
    return name + "_" if name in KEYWORDS else name


def snake(name):
    return identifier(re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower())


def base(name):
    while name.startswith("Array of "):
        name = name[9:]
    return name


def literal(field):
    if field["types"] != ["String"]:
        return None
    match = re.search(r'(?:always|must be)\s+["“]([^"”]+)["”]', field["description"], re.I)
    if not match:
        match = re.search(r"(?:always|must be)\s+([a-z][a-z0-9_]*)[.;]?$", field["description"], re.I)
    return match.group(1) if match else None


def cpp_string(value):
    return json.dumps(value, ensure_ascii=False)


def comment(text):
    return "\n".join("/// " + line for line in text.replace("*/", "* /").splitlines())


def components(graph):
    """Tarjan SCCs: only edges inside recursive components need boxing."""
    indices, low, stack, active, result = {}, {}, [], set(), {}

    def visit(node):
        indices[node] = low[node] = len(indices)
        stack.append(node)
        active.add(node)
        for child in sorted(graph[node]):
            if child not in indices:
                visit(child)
                low[node] = min(low[node], low[child])
            elif child in active:
                low[node] = min(low[node], indices[child])
        if low[node] == indices[node]:
            while True:
                child = stack.pop()
                active.remove(child)
                result[child] = node
                if child == node:
                    break
    for node in sorted(graph):
        if node not in indices:
            visit(node)
    return result


class Generator:
    def __init__(self, spec, digest):
        self.spec, self.digest = spec, digest
        self.types = {n: t for n, t in spec["types"].items() if n != "InputFile"}
        self.graph = {}
        for name, node in self.types.items():
            refs = {base(t) for t in node.get("subtypes", [])}
            refs.update(base(t) for f in node.get("fields", []) for t in f["types"])
            self.graph[name] = refs & self.types.keys()
        self.scc = components(self.graph)

    def boxed(self, owner, target):
        return owner in self.scc and self.scc[owner] == self.scc[target]

    def type(self, names, owner=None):
        # The scraper lists album element alternatives as repeated "Array of"
        # types. They describe one array whose elements can have different types.
        if len(names) > 1 and all(name.startswith("Array of ") for name in names):
            return f"std::vector<{self.type([name[9:] for name in names], owner)}>"
        def one(name):
            if name.startswith("Array of "):
                return f"std::vector<{one(name[9:])}>"
            if name in PRIMITIVES:
                return PRIMITIVES[name]
            if name == "InputFile":
                return name
            if name not in self.types:
                raise ValueError(f"unknown schema type: {name}")
            return f"Box<{name}>" if self.boxed(owner, name) else name
        mapped = list(dict.fromkeys(one(n) for n in names))
        return mapped[0] if len(mapped) == 1 else "std::variant<" + ", ".join(mapped) + ">"

    def fields(self, node, owner=None, required_first=False):
        fields = node.get("fields", [])
        if required_first:
            fields = sorted(fields, key=lambda f: not f["required"])
        output = []
        for field in fields:
            output.append(comment(field["description"]))
            kind = self.type(field["types"], owner)
            if not field["required"]:
                kind = f"std::optional<{kind}>"
            default = cpp_string(literal(field)) if literal(field) else ""
            output.append(f"    {kind} {identifier(field['name'])}{{{default}}};")
        return output

    def order(self):
        seen, ordered = set(), []
        def visit(name):
            if name in seen:
                return
            seen.add(name)
            for child in sorted(self.graph[name]):
                if not self.boxed(name, child):
                    visit(child)
            ordered.append(name)
        for name in sorted(self.types):
            visit(name)
        return ordered

    def banner(self):
        return f"// Generated by tools/generate.py. DO NOT EDIT.\n// {self.spec['version']}; spec SHA256: {self.digest}\n"

    def object_codec(self, name, node, owner=None):
        lines = [f"Json encode_object(const {name}& value, Request* request) {{", "    Json json = Json::object();"]
        if not node.get("fields"):
            lines.append("    (void)value; (void)request;")
        for field in node.get("fields", []):
            member = identifier(field["name"])
            tag = literal(field)
            if tag:
                lines.append(f"    if (value.{member} != {cpp_string(tag)}) throw std::invalid_argument(\"invalid {name}.{member} discriminator\");")
            lines.append(f"    detail::write_field(json, {cpp_string(field['name'])}, value.{member}, request);")
        lines.extend(["    return json;", "}", f"void decode_object(const Json& json, {name}& value) {{",
                      '    if (!json.is_object()) throw std::invalid_argument("expected Object");', f"    {name} parsed{{}};"])
        for field in node.get("fields", []):
            member = identifier(field["name"])
            required = "required" if field["required"] else "optional"
            lines.append(f"    detail::read_{required}(json, {cpp_string(field['name'])}, parsed.{member});")
            tag = literal(field)
            if tag:
                lines.append(f"    if (parsed.{member} != {cpp_string(tag)}) throw std::invalid_argument(\"invalid {name}.{member} discriminator\");")
        lines.extend(["    value = std::move(parsed);", "}"])
        return "\n".join(lines)

    def union_codec(self, name, node):
        subs = node["subtypes"]
        lines = [f"Json encode_object(const {name}& value, Request* request) {{",
                 "    return to_json(value.value, request);", "}",
                 f"void decode_object(const Json& json, {name}& value) {{"]
        if all(sub in self.types for sub in subs):
            lines.append('    if (!json.is_object()) throw std::invalid_argument("expected union Object");')
        # Telegram's accessible/inaccessible message union is selected by date.
        if name == "MaybeInaccessibleMessage":
            for index, sub in enumerate(subs):
                condition = 'json.at("date") == 0' if sub == "InaccessibleMessage" else 'json.at("date") != 0'
                kind = self.type([sub], name)
                lines.append(f"    if ({condition}) {{ value.value.emplace<{index}>(from_json<{kind}>(json)); return; }}")
        else:
            tags = [{f["name"]: literal(f) for f in self.types.get(sub, {}).get("fields", []) if literal(f)} for sub in subs]
            common = set(tags[0]).intersection(*(set(tag) for tag in tags[1:]))
            discriminator = next((key for key in sorted(common) if len({tag[key] for tag in tags}) == len(subs)), None)
            if discriminator:
                for index, sub in enumerate(subs):
                    kind = self.type([sub], name)
                    lines.append(f"    if (json.at({cpp_string(discriminator)}) == {cpp_string(tags[index][discriminator])}) {{ value.value.emplace<{index}>(from_json<{kind}>(json)); return; }}")
            else:
                # Structural unions have no common discriminator. Prefer the most
                # specific shape (e.g. venue over location), then schema order.
                def required_count(sub):
                    return sum(f["required"] for f in self.types.get(sub, {}).get("fields", []))
                for sub in sorted(subs, key=lambda s: -required_count(s)):
                    index, kind = subs.index(sub), self.type([sub], name)
                    lines.extend([f"    try {{ auto parsed = from_json<{kind}>(json); value.value.emplace<{index}>(std::move(parsed)); return; }}",
                                  "    catch (const std::invalid_argument&) {}",
                                  "    catch (const nlohmann::json::exception&) {}"])
        lines.extend([f'    throw std::invalid_argument("unknown or invalid {name} alternative");', "}"])
        return "\n".join(lines)

    def generate(self):
        header = [self.banner(), '#pragma once\n#include "codec.hpp"\n', "namespace tgpp {"]
        for name in sorted(self.types):
            header.append(f"struct {name};")
        for name in self.order():
            node = self.types[name]
            header.extend([comment("\n".join(node.get("description", []))), f"/// @see {node['href']}", f"struct {name} {{"])
            if node.get("subtypes"):
                header.append(f"    {self.type(node['subtypes'], name)} value;")
            else:
                header.extend(self.fields(node, name))
            header.append("};")
        for name in sorted(self.types):
            header.extend([f"Json encode_object(const {name}& value, Request* request);",
                           f"void decode_object(const Json& json, {name}& value);"])
        header.extend([f'inline constexpr const char* api_version = {cpp_string(self.spec["version"])};', "} // namespace tgpp\n"])
        outputs = {"include/tgpp/types.hpp": "\n".join(header)}
        codecs = []
        for name in sorted(self.types):
            node = self.types[name]
            codecs.append(self.union_codec(name, node) if node.get("subtypes") else self.object_codec(name, node, name))
        for index in range(0, len(codecs), 25):
            outputs[f"src/generated/types_{index // 25:02}.cpp"] = self.banner() + '#include <tgpp/types.hpp>\nnamespace tgpp {\n' + "\n".join(codecs[index:index + 25]) + "\n}\n"

        methods = [self.banner(), '#pragma once\n#include "types.hpp"\n', "namespace tgpp {"]
        declarations, implementations = [], []
        for name, method in sorted(self.spec["methods"].items()):
            options = name[0].upper() + name[1:] + "Request"
            result = self.type(method["returns"])
            methods.extend([comment("\n".join(method["description"])), f"/// @see {method['href']}", f"struct {options} {{"])
            methods.extend(self.fields(method, required_first=True))
            methods.extend(["};", f"Request make_request(const {options}& options, std::vector<Attachment> attachments = {{}});",
                            f"Json encode_object(const {options}& value, Request* request);",
                            f"void decode_object(const Json& json, {options}& value);"])
            default = " = {}" if not any(f["required"] for f in method.get("fields", [])) else ""
            declarations.append(f"    {result} {snake(name)}(const {options}& options{default}, std::vector<Attachment> attachments = {{}}) const;")
            implementations.append("\n".join([self.object_codec(options, method),
                                    f"Request make_request(const {options}& options, std::vector<Attachment> attachments) {{",
                                    f"    Request request; request.method = {cpp_string(name)}; request.files = std::move(attachments);",
                                    "    request.parameters = to_json(options, &request);", "    return request;", "}",
                                    f"{result} Bot::{snake(name)}(const {options}& options, std::vector<Attachment> attachments) const {{",
                                    f"    return from_json<{result}>(call(make_request(options, std::move(attachments))));", "}"]))
        methods.extend(["class Bot : public Client {", "public:", "    using Client::Client;", *declarations, "};", "} // namespace tgpp\n"])
        outputs["include/tgpp/methods.hpp"] = "\n".join(methods)
        # Method code is split by complete implementation blocks, not line count.
        block_size = 12
        for index in range(0, len(self.spec["methods"]), block_size):
            start, end = index, index + block_size
            outputs[f"src/generated/methods_{index // block_size:02}.cpp"] = self.banner() + '#include <tgpp/methods.hpp>\nnamespace tgpp {\n' + "\n".join(implementations[start:end]) + "\n}\n"
        return outputs


def update_spec():
    with urllib.request.urlopen("https://api.github.com/repos/PaulSonOfLars/telegram-bot-api-spec/commits/main", timeout=30) as response:
        commit = json.load(response)["sha"]
    prefix = f"https://raw.githubusercontent.com/PaulSonOfLars/telegram-bot-api-spec/{commit}"
    with urllib.request.urlopen(prefix + "/api.json", timeout=30) as response:
        data = response.read()
    spec = json.loads(data)
    lock = {"repository": REPOSITORY, "commit": commit, "sha256": hashlib.sha256(data).hexdigest(), "version": spec["version"]}
    with urllib.request.urlopen(prefix + "/LICENSE", timeout=30) as response:
        license_text = response.read()
    (ROOT / "spec/api.json").write_bytes(data)
    (ROOT / "spec/LICENSE").write_bytes(license_text)
    (ROOT / "spec/lock.json").write_text(json.dumps(lock, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true")
    group.add_argument("--update-spec", action="store_true")
    args = parser.parse_args()
    if args.update_spec:
        update_spec()
    data = (ROOT / "spec/api.json").read_bytes()
    lock = json.loads((ROOT / "spec/lock.json").read_text())
    digest = hashlib.sha256(data).hexdigest()
    if digest != lock["sha256"]:
        raise SystemExit("spec hash mismatch; restore the pinned spec or use --update-spec")
    spec = json.loads(data)
    outputs = Generator(spec, digest).generate()
    stale = set((ROOT / "src/generated").glob("*.cpp")) - {ROOT / name for name in outputs}
    changed = [name for name, text in outputs.items() if not (ROOT / name).exists() or (ROOT / name).read_text() != text]
    if args.check:
        if changed or stale:
            raise SystemExit("generated files are stale: " + ", ".join(changed + [str(p.relative_to(ROOT)) for p in sorted(stale)]))
    else:
        for path in stale:
            path.unlink()
        for name in changed:
            text = outputs[name]
            path = ROOT / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
    print(f"{'Verified' if args.check else 'Generated'} {len(spec['types'])} types (including InputFile), {len(spec['methods'])} methods; {spec['version']}")


if __name__ == "__main__":
    main()
