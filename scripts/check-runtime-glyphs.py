#!/usr/bin/env python3
"""Reject non-ASCII glyphs in C/C++ runtime string literals.

The compiled Montserrat subsets used by Nightglass guarantee the printable
ASCII range. Documentation and source comments are intentionally ignored.
"""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (ROOT / "main", ROOT / "components")
SUFFIXES = {".c", ".cpp", ".h", ".hpp"}


def unsupported_literals(path: Path) -> list[tuple[int, str]]:
    text = path.read_text(encoding="utf-8")
    findings: list[tuple[int, str]] = []
    state = "code"
    line = 1
    start_line = 1
    escaped = False
    index = 0
    literal: list[str] = []

    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""

        if state == "code":
            if char == "/" and following == "/":
                state = "line_comment"
                index += 1
            elif char == "/" and following == "*":
                state = "block_comment"
                index += 1
            elif char == '"':
                state = "string"
                start_line = line
                literal = []
                escaped = False
            elif char == "'":
                state = "char"
                escaped = False
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state == "string":
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                unsupported = "".join(value for value in literal if ord(value) > 127)
                if unsupported:
                    findings.append((start_line, unsupported))
                state = "code"
            else:
                literal.append(char)
        elif state == "char":
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == "'":
                state = "code"

        if char == "\n":
            line += 1
        index += 1

    return findings


def main() -> int:
    failed = False
    for source_root in SOURCE_ROOTS:
        for path in sorted(source_root.rglob("*")):
            if path.suffix not in SUFFIXES:
                continue
            for line, glyphs in unsupported_literals(path):
                failed = True
                relative = path.relative_to(ROOT)
                codepoints = " ".join(f"U+{ord(glyph):04X}" for glyph in glyphs)
                print(f"{relative}:{line}: unsupported runtime glyphs: {codepoints}")
    if failed:
        print("Runtime UI strings must use the compiled ASCII-safe glyph set.", file=sys.stderr)
        return 1
    print("Nightglass runtime UI glyph check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
