#!/usr/bin/env python3
"""Run the separately built allocation-failure regression in an empty directory."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="mvd-allocation-", dir=args.tmp_root) as root:
        result = subprocess.run(
            [str(executable), root], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            encoding="utf-8", errors="strict", timeout=120, check=False,
        )
        print(result.stdout, end="", flush=True)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
