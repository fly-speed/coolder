#!/usr/bin/env python3
"""Require English comments on C++ header types, fields and functions."""

import re
import sys

from check_cpp_structure import ROOT, EXCLUDED, Language, Parser, tree_sitter_cpp, walk

SCOPES = {
    "field_declaration_list", "declaration_list", "translation_unit",
    "preproc_ifdef", "preproc_if", "preproc_else", "preproc_elif",
}


def declarations(tree):
    """Exclude local variables and references to externally defined struct types."""
    for node in walk(tree.root_node):
        if node.type in {"class_specifier", "struct_specifier"}:
            if node.child_by_field_name("body"):
                yield node
            elif node.parent.type in SCOPES | {"declaration", "field_declaration"}:
                if not node.parent.children_by_field_name("declarator"):
                    yield node
        elif node.type == "function_definition":
            yield node
        elif node.type in {"field_declaration", "declaration"}:
            if node.parent.type not in SCOPES:
                continue
            if not node.children_by_field_name("declarator"):
                continue
            if any(child.type in {"class_specifier", "struct_specifier"}
                   and child.child_by_field_name("body") for child in node.named_children):
                continue
            parent = node.parent
            while parent and parent.type != "function_definition":
                parent = parent.parent
            if parent is None:
                yield node


def preceding_comment(lines, row):
    """Read an adjacent line-comment or block-comment group, allowing blank lines."""
    index = row - 1
    while index >= 0 and not lines[index].strip():
        index -= 1
    comments = []
    if index >= 0 and lines[index].strip().endswith("*/"):
        while index >= 0:
            comments.append(lines[index])
            if "/*" in lines[index]:
                break
            index -= 1
    else:
        while index >= 0 and lines[index].lstrip().startswith("//"):
            comments.append(lines[index])
            index -= 1
    return "\n".join(comments)


def main():
    parser = Parser(Language(tree_sitter_cpp.language()))
    failures = []
    count = files = 0
    for directory in (ROOT / "libai", ROOT / "coolder"):
        for path in sorted(directory.rglob("*")):
            if path.suffix not in {".h", ".hh", ".hpp"}:
                continue
            if EXCLUDED.intersection(path.relative_to(ROOT).parts):
                continue
            files += 1
            data = path.read_bytes()
            lines = data.decode("utf-8").splitlines()
            for node in declarations(parser.parse(data)):
                count += 1
                row = node.start_point.row
                comment = preceding_comment(lines, row)
                if not re.search(r"[A-Za-z]{2,}", comment):
                    failures.append(f"{path.relative_to(ROOT)}:{row + 1}: "
                                    f"missing English comment for {node.type}")
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"Checked {count} declarations in {files} headers; "
          f"{len(failures)} missing English comments")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
