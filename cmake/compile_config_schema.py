"""Compile the offline engine schema subset into private C++ rules."""

import argparse
from decimal import Decimal
import hashlib
import json
import math
from pathlib import Path
import re


KEYWORDS = {
    "$schema", "$id", "$defs", "$ref", "type", "const", "properties", "required",
    "additionalProperties", "items", "oneOf", "enum", "minimum", "exclusiveMinimum", "maximum",
    "minItems", "maxItems", "minLength", "maxLength",
}
TYPES = {"array", "boolean", "integer", "number", "object", "string", "null"}
EXTENTS = {"minItems", "maxItems", "minLength", "maxLength"}
BOUNDS = {"minimum", "exclusiveMinimum", "maximum"}
DIALECT = "https://json-schema.org/draft/2020-12/schema"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate key {key!r}")
        result[key] = value
    return result


def load(data):
    def reject_constant(value):
        raise ValueError(f"Invalid number {value}")

    return json.loads(data.decode("utf-8"), object_pairs_hook=unique, parse_int=Decimal,
                      parse_float=Decimal, parse_constant=reject_constant)


def pointer_token(name):
    return name.replace("~", "~0").replace("/", "~1")


class Compiler:
    def __init__(self, root, production, version=1):
        require(isinstance(root, dict), "Expected an object schema")
        defs = root.get("$defs", {})
        require(isinstance(defs, dict), "Expected an object for $defs")
        self.production = production
        self.rules = [{} for _ in range(1 + len(defs))]
        self.definitions = {f"#/$defs/{pointer_token(name)}": i for i, name in enumerate(defs, 1)}
        self.visit(root, 0, True)
        for i, schema in enumerate(defs.values(), 1):
            self.visit(schema, i)
        self.check_expansions()
        if production:
            require(root.get("$schema") == DIALECT and root.get("$id") == f"urn:engine-sim:config:{version}",
                    "The registry entry must declare its pinned identity and dialect")
            require(root.get("properties", {}).get("format_version") == {"const": Decimal(version)}
                    and "format_version" in root.get("required", []), "Missing exact format version discriminator")
            self.check_variants()

    @staticmethod
    def decimal_is_admitted(value):
        if not isinstance(value, Decimal) or not value.is_finite() or len(str(value)) > 128:
            return False
        binary = float(value)
        return math.isfinite(binary) and (binary != 0 or value.is_zero())

    def child(self, schema):
        index = len(self.rules)
        self.rules.append({})
        self.visit(schema, index)
        return index

    def visit(self, schema, index, root=False):
        require(isinstance(schema, dict), "Expected an object schema")
        unknown = schema.keys() - KEYWORDS
        require(not unknown, f"Unsupported schema keyword {sorted(unknown)}")
        require(root or not schema.keys() & {"$schema", "$id", "$defs"}, "Registry metadata is root-only")
        rule = self.rules[index]
        if "$ref" in schema:
            ref = schema["$ref"]
            require(isinstance(ref, str) and ref.startswith("#/$defs/"), f"Only local $defs refs are allowed: {ref!r}")
            require(ref in self.definitions, f"Unresolved schema ref {ref!r}")
            rule["reference"] = self.definitions[ref]
        if "type" in schema:
            value = schema["type"]
            require(isinstance(value, str) and value in TYPES, f"Unsupported type form {value!r}")
            rule["type"] = value
        if "const" in schema:
            value = schema["const"]
            require(isinstance(value, (str, bool, Decimal)) or value is None, "Only scalar const rules are supported")
            if isinstance(value, Decimal):
                require(self.decimal_is_admitted(value), "Numeric const exceeds the admitted decimal range")
            rule["const"] = value
        if "enum" in schema:
            choices = schema["enum"]
            require(rule.get("type") == "string" and isinstance(choices, list) and
                    0 < len(choices) <= 16 and all(isinstance(choice, str) for choice in choices) and
                    len(set(choices)) == len(choices), "Expected unique string enum choices")
            rule["choices"] = choices
        required = schema.get("required", [])
        require(isinstance(required, list) and all(isinstance(k, str) for k in required)
                and len(required) == len(set(required)), "Invalid required list")
        properties = schema.get("properties", {})
        require(isinstance(properties, dict), "Expected properties object")
        if "additionalProperties" in schema:
            require(type(schema["additionalProperties"]) is bool, "Expected boolean additionalProperties")
            rule["closed"] = not schema["additionalProperties"]
        if self.production and ("properties" in schema or rule.get("type") == "object" or
                                "required" in schema or "additionalProperties" in schema):
            require(rule.get("type") == "object" and rule.get("closed") and
                    all(k in properties for k in required), "Shipped object rules must use declared closed properties")
        if properties or required:
            order = [(k, self.child(v), k in required) for k, v in properties.items()]
            order.extend((k, -1, True) for k in required if k not in properties)
            rule["properties"] = order
        if "items" in schema:
            rule["items"] = self.child(schema["items"])
        if "oneOf" in schema:
            arms = schema["oneOf"]
            require(isinstance(arms, list) and 1 <= len(arms) <= 16, "Invalid oneOf extent")
            rule["alternatives"] = [self.child(arm) for arm in arms]
        for keyword in EXTENTS | BOUNDS:
            if keyword not in schema:
                continue
            value = schema[keyword]
            require(self.decimal_is_admitted(value), f"Invalid {keyword} bound")
            if keyword in EXTENTS:
                require(0 <= value <= 2_097_152 and value == int(value), f"Invalid {keyword} extent")
                value = int(value)
            rule[keyword] = value
        if self.production:
            if "items" in schema or schema.keys() & {"minItems", "maxItems"}:
                require(rule.get("type") == "array" and "items" in schema, "Invalid shipped array subform")
            if schema.keys() & {"minLength", "maxLength"}:
                require(rule.get("type") == "string", "Invalid shipped string subform")
            if schema.keys() & BOUNDS:
                require(rule.get("type") in {"integer", "number"}, "Invalid shipped number subform")
        for minimum, maximum in (("minItems", "maxItems"), ("minLength", "maxLength"), ("minimum", "maximum")):
            require(minimum not in rule or maximum not in rule or rule[minimum] <= rule[maximum],
                    f"Reversed {minimum} / {maximum}")

    def check_expansions(self):
        depths = {}
        pending = set()

        def walk(index):
            require(index not in pending, "Schema expansion cycle")
            require(len(pending) < 64, "Schema expansion exceeds depth limit")
            if index in depths:
                return depths[index]
            pending.add(index)
            rule = self.rules[index]
            children = [rule.get("reference", -1), rule.get("items", -1), *rule.get("alternatives", [])]
            children.extend(child for _, child, _ in rule.get("properties", []))
            depth = 1 + max((walk(child) for child in children if child >= 0), default=0)
            require(depth <= 64, "Schema expansion exceeds depth limit")
            pending.remove(index)
            depths[index] = depth
            return depth

        for index in range(len(self.rules)):
            walk(index)

    def check_variants(self):
        for rule in self.rules:
            arms = rule.get("alternatives")
            if arms is None:
                continue
            require(len(arms) >= 2, "Shipped oneOf requires at least two variants")
            tags = set()
            for index in arms:
                arm = self.rules[index]
                require(arm.keys() == {"reference"}, "Shipped oneOf arms must reference a variant definition")
                target = self.rules[arm["reference"]]
                require(target.get("closed") and target.get("type") == "object",
                        "Shipped variant definitions must be closed objects")
                kind = next((p for p in target.get("properties", []) if p[0] == "kind" and p[2]), None)
                require(kind is not None, "Shipped variants must have a required kind discriminator")
                tag = self.rules[kind[1]].get("const")
                require(isinstance(tag, str) and tag and tag not in tags, "Overlapping or missing variant discriminator")
                tags.add(tag)
            rule["discriminated"] = True


def literal(string):
    # Octal escapes keep generated source ASCII and preserve exact UTF-8.
    text = []
    for byte in string.encode("utf-8"):
        if byte in (ord('"'), ord('\\')):
            text.append("\\" + chr(byte))
        elif 32 <= byte < 127:
            text.append(chr(byte))
        else:
            text.append(f"\\{byte:03o}")
    return '"' + "".join(text) + '"'


def encode(value):
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, list):
        return "[" + ",".join(encode(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(json.dumps(key) + ":" + encode(item) for key, item in value.items()) + "}"
    return json.dumps(value, allow_nan=False)


def generate(lines, factory, rules):
    lines += [f"inline Rules {factory}() {{", f"    Rules rules({len(rules)});"]
    for i, rule in enumerate(rules):
        for key, value in sorted(rule.items()):
            if key == "type":
                cpp = "Type::" + value
            elif key in {"closed", "discriminated"}:
                cpp = str(value).lower()
            elif key == "const":
                if isinstance(value, Decimal):
                    cpp = f"Constant{{Kind::number, {literal(str(value))}}}"
                elif isinstance(value, str):
                    cpp = f"Constant{{Kind::string, {literal(value)}}}"
                elif isinstance(value, bool):
                    cpp = f"Constant{{Kind::boolean, {literal(str(value).lower())}}}"
                else:
                    cpp = "Constant{Kind::null, {}}"
                key = "constant"
            elif key == "properties":
                definitions = [f"{{{literal(k)}, {child}, {str(required).lower()}}}" for k, child, required in value]
                cpp = "{" + ", ".join(definitions) + "}"
            elif key == "choices":
                cpp = "{" + ", ".join(map(literal, value)) + "}"
            elif key == "alternatives":
                cpp = "{" + ", ".join(map(str, value)) + "}"
            elif key in BOUNDS:
                cpp = literal(str(value))
            else:
                cpp = str(value)
            lines.append(f"    rules[{i}].{key} = {cpp};")
    lines += ["    return rules;", "}"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--version", type=int, choices=(1, 2), default=1)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--expected-sha256")
    modes.add_argument("--development-suite", action="store_true")
    args = parser.parse_args()
    data = args.input.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    lines = ["// Generated from the checked JSON inputs.", "namespace engine_sim::config_v1::json_detail {"]
    if args.development_suite:
        groups = load(data)
        lines.append("struct GeneratedCase { std::string_view name, input; bool expected; };")
        for i, group in enumerate(groups):
            generate(lines, f"case_rules_{i}", Compiler(group["schema"], False).rules)
            lines.append(f"inline constexpr GeneratedCase cases_{i}[] = {{")
            for case in group["tests"]:
                name = group["description"] + " / " + case["description"]
                value = literal('{"value":' + encode(case["data"]) + "}")
                lines.append(f"    {{{literal(name)}, {value}, {str(case['valid']).lower()}}},")
            lines.append("};")
        lines.append("template<class Runner> void run_generated_cases(Runner &&run) {")
        for i in range(len(groups)):
            lines.append(f"    run(case_rules_{i}(), cases_{i});")
        lines.append("}")
    else:
        require(re.fullmatch(r"[0-9a-f]{64}", args.expected_sha256) and digest == args.expected_sha256,
                "Schema SHA-256 differs from the pinned registry entry")
        generate(lines, f"make_v{args.version}_rules", Compiler(load(data), True, args.version).rules)
        suffix = "" if args.version == 1 else f"_v{args.version}"
        schema = data.decode("utf-8")
        chunks = schema.splitlines(keepends=True)
        lines += [f'inline constexpr std::string_view embedded_schema{suffix}_digest = "{digest}";',
                  f"inline constexpr std::string_view embedded_schema{suffix}_bytes ="]
        lines.extend(f"    {literal(chunk)}" for chunk in chunks)
        lines[-1] += ";"
    lines.append("}")
    output = "\n".join(lines) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="ascii") != output:
        args.output.write_bytes(output.encode("ascii"))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RecursionError) as exc:
        raise SystemExit(f"Schema preflight failed: {exc}") from None
