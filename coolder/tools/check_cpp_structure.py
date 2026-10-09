#!/usr/bin/env python3
"""Check the shared libai/coolder C++ structure limits (including headers)."""

import argparse
from pathlib import Path
import sys

try:
    from tree_sitter import Language, Parser
    import tree_sitter_cpp
except ImportError:
    sys.exit("Install tooling: python3 -m pip install -r coolder/tools/style-requirements.txt")

ROOT = Path(__file__).resolve().parents[2]
EXTENSIONS = {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp"}
EXCLUDED = {"build", "obj", "lib", "CMakeFiles"}


def walk(node):
    yield node
    for child in node.named_children:
        yield from walk(child)


def check(path, parser):
    data = path.read_bytes()
    lines = data.splitlines()
    errors = []
    label = path.relative_to(ROOT)
    if len(lines) > 1000:
        errors.append(f"{label}: {len(lines)} lines (limit 1000)")
    tree = parser.parse(data)
    functions = 0
    longest = 0
    literal_lines = set()
    for node in walk(tree.root_node):
        # Literal contents belong to the generated text, not C++ indentation.
        if node.type == "raw_string_literal":
            literal_lines.update(range(node.start_point.row + 1, node.end_point.row + 1))
        if node.type not in {"function_definition", "lambda_expression"}:
            continue
        functions += 1
        length = node.end_point.row - node.start_point.row + 1
        longest = max(longest, length)
        if length > 200:
            errors.append(f"{label}:{node.start_point.row + 1}: function/lambda has {length} lines (limit 200)")
    maximum_indent = 0
    for index, line in enumerate(lines):
        if index in literal_lines or not line.strip():
            continue
        # clang-format uses tabs for nesting and spaces for continuation alignment.
        depth = len(line) - len(line.lstrip(b"\t"))
        maximum_indent = max(maximum_indent, depth)
        if depth > 4:
            errors.append(f"{label}:{index + 1}: {depth} indentation levels (limit 4)")
    return errors, functions, longest, len(lines), maximum_indent


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("paths", nargs="*", type=Path, help="Optional source files; default: both source trees")
    options = args.parse_args()
    files = options.paths or sorted(
        path for folder in (ROOT / "libai", ROOT / "coolder")
        for path in folder.rglob("*")
        if path.suffix in EXTENSIONS and not EXCLUDED.intersection(path.relative_to(ROOT).parts)
    )
    parser = Parser(Language(tree_sitter_cpp.language()))
    failures = []
    count = longest = largest = indentation = 0
    for path in files:
        errors, functions, length, lines, depth = check(path.resolve(), parser)
        failures.extend(errors)
        count += functions
        longest, largest, indentation = max(longest, length), max(largest, lines), max(indentation, depth)
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"{len(files)} files, {count} functions/lambdas; maxima: {largest} lines/file, "
          f"{longest} lines/function, {indentation} indentation levels")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
