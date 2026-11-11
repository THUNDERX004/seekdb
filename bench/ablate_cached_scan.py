#!/usr/bin/env python3
"""Build a diagnostic parser-only source from two explicitly supplied sources.

Copies only JsonRequestParser and Lexer from the candidate into the baseline.
This script is used by the paired CI benchmark, never by the submission build.
"""
from pathlib import Path
import argparse
import hashlib


def region(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("ambiguous source boundaries")
    first, last = text.index(start), text.index(end)
    if last <= first:
        raise ValueError("invalid source boundary order")
    return first, last


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    base = args.baseline.read_text(encoding="utf-8")
    candidate = args.candidate.read_text(encoding="utf-8")
    for start, end in (("class JsonRequestParser {", "enum class TokenKind"),
                       ("class Lexer {", "enum class CompareOp")):
        a, b = region(base, start, end)
        c, d = region(candidate, start, end)
        base = base[:a] + candidate[c:d] + base[b:]
    args.output.write_text(base, encoding="utf-8", newline="\n")
    print(args.output, hashlib.sha256(args.output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
