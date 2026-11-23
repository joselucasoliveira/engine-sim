"""Produce test-only copies with clocks at real preparation boundaries."""
import hashlib
import json
from pathlib import Path
import sys


def measured(phase, expression):
    return f"cost::observe(cost::Phase::{phase}, [&] {{ return {expression}; }})"


def scope(phase):
    return f"    const cost::Scope measure_{phase}(cost::Phase::{phase});"


def replace(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Expected one preparation boundary: {old!r}")
    return source.replace(old, new)


def parser(source):
    for entry, signature in (
        ("ModelResult read_normalized", "validate_v1(parsed)"),
        ("Result read_versioned", "validate_document(parsed, format, true)"),
    ):
        preceding, needle, body = source.partition(entry)
        if not needle:
            raise ValueError(f"JSON reader entry point missing: {entry}")
        start, end, following = body.partition("\n}\n")
        if not end:
            raise ValueError(f"JSON reader body missing: {entry}")
        start = replace(start, "parse(input)", measured("tokenize_parse", "parse(input)"))
        start = replace(start, signature, measured("schema", signature))
        source = preceding + needle + start + end + following
    source = replace(source, "normalize(decode<Document>({parsed, 0}))",
                     "normalize(" + measured("document_projection", "decode<Document>({parsed, 0})") + ")")
    return replace(source, "decode<Document>(root)", measured("document_projection", "decode<Document>(root)"))


def schema(source):
    for registry in ("make_v1_rules()", "make_v2_rules()"):
        source = replace(source, registry, measured("shared_schema", registry))
    return source


def normalization(source):
    source = replace(source, "detail::check_shape(input);",
                     "cost::observe(cost::Phase::schema, [&] { detail::check_shape(input); });")
    source = replace(source, "detail::normalize_values(std::move(input))",
                     measured("normalization", "detail::normalize_values(std::move(input))"))
    return replace(source, "detail::check_graph(values)", measured("semantic_graph", "detail::check_graph(values)"))


def assembly(source):
    source = replace(source, "    auto options = options_for", scope("assembly") + "\n    auto options = options_for")
    fields = (("definition", "build_definition(model, profile.seed, source)"),
              ("resource_io", "pin(definitions, absolute_root, expected, checkpoint)"),
              ("resource_decode", "decodeImpulseWave(file.bytes, file.path)"))
    for phase, call in fields:
        source = replace(source, call, measured(phase, call))
    return source


def runtime(source):
    entry = "Outcome read_document(JsonRecipe &recipe, const JsonRecipe *expected) {"
    source = replace(source, entry, entry + "\n" + scope("document_read"))
    source = replace(source, "if (size != 0) recipe.bytes.assign(bytes, static_cast<std::size_t>(size));",
                     """cost::observe(cost::Phase::document_copy, [&] {
            if (size != 0) recipe.bytes.assign(bytes, static_cast<std::size_t>(size));
        });""")
    source = replace(source, "if (next.entry.empty()) next.bytes = saved->bytes;",
                     """if (next.entry.empty()) {
            cost::observe(cost::Phase::document_copy, [&] { next.bytes = saved->bytes; });
        }""")
    source = replace(source, "        runtime.adoptSession",
                     "    " + scope("activation") + "\n        runtime.adoptSession")
    return replace(source, "if (checkpoint) checkpoint(json_runtime::Phase::snapshot);",
                   "cost::before_swap(); if (checkpoint) checkpoint(json_runtime::Phase::snapshot);")


def geometry(source):
    entry = "Simulator *Engine::createSimulator(Vehicle *vehicle, Transmission *transmission, int initialFrequency) {"
    return replace(source, entry, entry + "\n" + scope("geometry_solver"))


def filter_table(source):
    build = "Table() { filter.initialize(1.0, 3.0, 1024); }"
    return replace(source, build, """Table() {
""" + "    " + scope("shared_filter") + """
            filter.initialize(1.0, 3.0, 1024);
        }""")


def synthesis(source):
    entry = "void Synthesizer::initialize(const Parameters &p) {"
    source = replace(source, entry, entry + "\n" + scope("dsp"))
    entry = "    int index)\n{\n    std::lock_guard<std::mutex> lock(m_lock0);"
    return replace(source, entry, "    int index)\n{\n" + scope("convolution")
                   + "\n    std::lock_guard<std::mutex> lock(m_lock0);")


def main(source, out):
    out.mkdir(parents=True, exist_ok=True)
    transforms = {
        "modules/json/src/decode.cpp": parser,
        "modules/json/src/schema.cpp": schema,
        "modules/config/src/config.cpp": normalization,
        "modules/assembly/src/preparation.cpp": assembly,
        "src/runtime/json_runtime.cpp": runtime,
        "src/engine.cpp": geometry,
        "src/function.cpp": filter_table,
        "src/synthesizer.cpp": synthesis,
    }
    inputs = {}
    for path, transform in transforms.items():
        before = (source / path).read_bytes()
        after = ('#include "observe.hpp"\n' + transform(before.decode("utf-8").replace("\r\n", "\n"))).encode("utf-8")
        target = out / Path(path).name
        target.write_bytes(after)
        inputs[path] = {"original_sha256": hashlib.sha256(before).hexdigest(),
                        "instrumented_sha256": hashlib.sha256(after).hexdigest()}
    (out / "identities.json").write_text(json.dumps(inputs, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main(*map(Path, sys.argv[1:]))
