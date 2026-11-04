#!/usr/bin/env python3
"""Extract actual seekdb functions and generate/run a standalone C++ leaf test.

No seekdb build, server, SQL connection, or checkout mutation is performed.
Default action only generates C++; --run explicitly compiles and runs the leaf.
The framework stubs are intentionally small; SQL full fixtures remain required.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


HERE = Path(__file__).resolve().parent
DEFAULT_SOURCE_ROOT = Path(__file__).resolve().parents[3]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def block(text: str, marker: str) -> str:
    """Copy a complete definition verbatim, balancing braces outside comments."""
    if text.count(marker) != 1:
        raise ValueError("Expected one unambiguous definition: " + marker)
    start = text.index(marker)
    opening = text.index("{", start)
    depth, state, pos = 0, "code", opening
    while pos < len(text):
        char = text[pos]
        pair = text[pos:pos + 2]
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "comment":
            if pair == "*/":
                state = "code"
                pos += 1
        elif state in ('"', "'"):
            if char == "\\":
                pos += 1
            elif char == state:
                state = "code"
        elif pair == "//":
            state = "line"
            pos += 1
        elif pair == "/*":
            state = "comment"
            pos += 1
        elif char in ('"', "'"):
            state = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
        pos += 1
    raise ValueError("Unclosed definition: " + marker)


def generate(source: Path, output: Path) -> tuple[Path, dict]:
    manifest_bytes = (HERE / "utf8_prepare_leaf_manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["schema_version"] != 2:
        raise ValueError("Unsupported leaf manifest schema")
    original_entries = {item["path"]: item for item in manifest["source_files"]}
    expected_sources = {
        "src/storage/fts/ik/ob_ik_processor.cpp",
        "src/storage/fts/ik/ob_ik_processor.h",
        "src/storage/fts/ik/ob_ik_char_util.h",
    }
    if len(manifest["source_files"]) != 3 or set(original_entries) != expected_sources:
        raise ValueError("Manifest must pin all three candidate and baseline source files")
    input_entries = {item["path"]: item for item in manifest["leaf_input_files"]}
    receipts: list[dict] = []
    file_receipts: list[dict] = []
    extracted_pins = {path: set() for path, entry in input_entries.items() if entry.get("definitions")}

    def verified_text(data: bytes, path: str, entry: dict, role: str,
                      raw_key: str, lf_key: str) -> str:
        normalized = data.replace(b"\r\n", b"\n")
        raw_hash, lf_hash = digest(data), digest(normalized)
        if raw_hash == entry[raw_key]:
            verification = "raw_sha256"
        elif lf_hash == entry[lf_key]:
            verification = "pinned_lf_sha256 (line endings only)"
        else:
            raise ValueError(role + " hash mismatch: " + path + "; raw=" + raw_hash
                             + "; LF=" + lf_hash + "; update pins only after source review")
        file_receipts.append({"path": path, "role": role, "raw_sha256": raw_hash,
                              "lf_sha256": lf_hash, "verification": verification})
        return normalized.decode("utf-8")

    def read_input(path: str) -> str:
        entry = input_entries[path]
        text = verified_text((source / path).read_bytes(), path, entry, "pinned leaf input",
                             "content_sha256", "content_lf_sha256")
        for pin in entry.get("definitions", []):
            if digest(block(text, pin["marker"]).encode()) != pin["definition_lf_sha256"]:
                raise ValueError("Pinned extracted definition changed: " + path + ": " + pin["marker"])
        return text

    def read_baseline(path: str) -> str:
        entry = original_entries[path]
        blob_id = entry["original_git_blob"]
        # -C supports both normal checkouts and worktrees with a .git file.
        data = subprocess.check_output(["git", "-C", str(source), "cat-file", "blob", blob_id])
        actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        normalized = data.replace(b"\r\n", b"\n")
        if actual != blob_id or digest(normalized) != entry["original_lf_sha256"]:
            raise ValueError("Original Git blob verification failed: " + path)
        file_receipts.append({"path": path, "role": "baseline", "git_blob": blob_id,
                              "raw_sha256": digest(data), "lf_sha256": digest(normalized),
                              "verification": "immutable_git_blob_and_lf_sha256"})
        return normalized.decode("utf-8")

    def read_candidate(path: str) -> str:
        return verified_text((source / path).read_bytes(), path, original_entries[path], "candidate",
                             "new_sha256", "new_lf_sha256")

    def copy(text: str, path: str, marker: str, version: str = "pinned working source") -> str:
        definition = block(text, marker)
        if path in extracted_pins:
            expected = {pin["marker"]: pin["definition_lf_sha256"]
                        for pin in input_entries[path]["definitions"]}
            if marker not in expected or digest(definition.encode()) != expected[marker]:
                raise ValueError("Unpinned extraction or definition drift: " + path + ": " + marker)
            extracted_pins[path].add(marker)
        receipts.append({"path": path, "version": version, "marker": marker,
                         "definition_lf_sha256": digest(definition.encode()),
                         "source_line": text[:text.index(marker)].count("\n") + 1})
        return definition + "\n"

    # Preflight ALL three files, including the header whose unrelated layout is stubbed below.
    candidates = {path: read_candidate(path) for path in original_entries}
    baselines = {path: read_baseline(path) for path in original_entries}
    driver_path = HERE / manifest["driver_file"]
    driver_data = driver_path.read_bytes()
    driver_text = verified_text(driver_data, manifest["driver_file"], manifest, "test driver",
                                "driver_sha256", "driver_lf_sha256")

    charset_h = read_input("src/oblib/lib/charset/ob_charset.h")
    ctype_h = read_input("src/oblib/lib/charset/ob_ctype.h")
    errno_h = read_input("src/oblib/lib/ob_errno.h")
    utf8_path = "src/oblib/lib/charset/ob_ctype_utf8.cc"
    utf8 = read_input(utf8_path)
    mb_path = "src/oblib/lib/charset/mb_wc.h"
    mb = read_input(mb_path)
    helper_path = "src/oblib/lib/charset/ob_charset_string_helper.h"
    helper = read_input(helper_path)
    charset_path = "src/oblib/lib/charset/ob_charset.cpp"
    charset = read_input(charset_path)
    unicode_h_path = "src/storage/fts/utils/unicode_utils.h"
    unicode_h = read_input(unicode_h_path)
    unicode_cpp_path = "src/storage/fts/utils/unicode_utils.cpp"
    unicode_cpp = read_input(unicode_cpp_path)

    chunks = [r'''// Generated from the source definitions listed in extraction.json.
// The validation, decoder, classification and context method bodies are verbatim.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
using uchar = unsigned char;
using ulong = unsigned long;
using uint16 = uint16_t;
using uint32 = uint32_t;
#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(...)
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORDS_BIGENDIAN
#endif
#define OB_FAIL(expr) ((ret = (expr)) != OB_SUCCESS)
#define OB_SUCC(code) ((code) == OB_SUCCESS)
#define OB_ISNULL(value) ((value) == nullptr)
#define OB_NOT_NULL(value) ((value) != nullptr)
#define OB_LIKELY(value) (value)
#define OB_UNLIKELY(value) (value)
#define UNUSED(value) ((void)(value))
#define STORAGE_FTS_LOG(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define ob_charset_assert(value) assert(value)
''']
    for name in ("ob_wc_t", "OB_CS_ILSEQ", "OB_CS_ILUNI", "OB_CS_TOOSMALL",
                 "OB_CS_TOOSMALL2", "OB_CS_TOOSMALL3", "OB_CS_TOOSMALL4"):
        chunks.append(re.search(r"^#define\s+" + name + r"\s+[^\n]+", ctype_h, re.M).group() + "\n")
    for name in ("OB_SUCCESS", "OB_INVALID_ARGUMENT", "OB_ERR_UNEXPECTED", "OB_NOT_SUPPORTED",
                 "OB_NOT_INIT", "OB_UNEXPECT_INTERNAL_ERROR", "OB_ITER_END"):
        chunks.append(re.search(r"^constexpr int " + name + r"\s*=\s*[^;]+;", errno_h, re.M).group() + "\n")
    for name in ("enum ObCharsetType", "enum ObCollationType"):
        chunks.append(copy(charset_h, "src/oblib/lib/charset/ob_charset.h", name) + ";\n")
    chunks.append(r'''
struct ObCharsetInfo;
struct ObCharsetHandler {
  size_t (*well_formed_len)(const ObCharsetInfo *, const char *, const char *, size_t, int *);
};
struct ObCharsetInfo { const ObCharsetHandler *cset; };
''')
    chunks.append(re.search(r"^#define IS_CONTINUATION_BYTE[^\n]+", utf8, re.M).group() + "\n")
    for marker in ("static int ob_valid_mbcharlen_utf8mb3(",
                   "static inline int ob_valid_mbcharlen_utf8mb4(",
                   "static inline size_t ob_well_formed_len_utf8mb4(",
                   "static int ob_mb_wc_utf8mb4("):
        chunks.append(copy(utf8, utf8_path, marker))
    chunks.append(copy(mb, mb_path, "template <bool RANGE_CHECK, bool SUPPORT_MB4>\nstatic ALWAYS_INLINE int ob_mb_wc_utf8_prototype("))
    # These are complete original template definitions, not a substitute decoder.
    helper_start = helper.index("template<ObCharsetType cs_type>\ninline int ob_charset_char_len(")
    helper_end = helper.index("template<>\ninline int ob_charset_encode_unicode<CHARSET_UTF8MB4>(")
    helper_block = helper[helper_start:helper_end]
    receipts.append({"path": helper_path, "version": "pinned working source",
                     "marker": "char_len/decode template block", "definition_lf_sha256": digest(helper_block.encode())})
    chunks.append("namespace common {\n" + helper_block + "}\nusing namespace common;\n")
    chunks.append(r'''
static uint64_t charset_lookups = 0;
class ObCharset {
public:
  static const ObCharsetInfo *get_charset(ObCollationType coll) {
    ++charset_lookups;
    static const ObCharsetHandler handler{ob_well_formed_len_utf8mb4};
    static const ObCharsetInfo info{&handler};
    switch (coll) {
      case CS_TYPE_UTF8MB4_GENERAL_CI:
      case CS_TYPE_UTF8MB4_BIN:
      case CS_TYPE_UTF8MB4_UNICODE_CI:
      case CS_TYPE_BINARY: // ASCII-only stub coverage; no binary charset integration claim.
        return &info;
      default: return nullptr;
    }
  }
  static bool is_valid_collation(int64_t);
  static ObCharsetType charset_type_by_coll(ObCollationType);
  static int first_valid_char(ObCollationType, const char *, int64_t, int64_t &);
  static ObCharsetType collation_charset_map[CS_TYPE_MAX];
};
ObCharsetType ObCharset::collation_charset_map[CS_TYPE_MAX] = {};
''')
    for marker in ("bool ObCharset::is_valid_collation(int64_t", "ObCharsetType ObCharset::charset_type_by_coll(",
                   "int ObCharset::first_valid_char("):
        chunks.append(copy(charset, charset_path, marker))
    chunks.append(copy(unicode_h, unicode_h_path, "class ObUnicodeBlockUtils final") + ";\n")
    for name in ("is_alpha", "is_arabic", "is_chinese", "is_other_cjk", "check_high_surrogate", "check_low_surrogate"):
        chunks.append(copy(unicode_cpp, unicode_cpp_path, "bool ObUnicodeBlockUtils::" + name + "("))
    chunks.append("struct ObIAllocator {};\nstruct LeafList { explicit LeafList(ObIAllocator &) {} };\n")
    util_path = "src/storage/fts/ik/ob_ik_char_util.h"
    processor_path = "src/storage/fts/ik/ob_ik_processor.cpp"
    for version, files in (("baseline", baselines), ("candidate", candidates)):
        util = files[util_path]
        cpp = files[processor_path]
        start = util.index("class ObFTCharUtil\n")
        end = util.index("} // namespace storage", start)
        actual_class = util[start:end]
        receipts.append({"path": util_path, "version": version, "marker": "complete ObFTCharUtil class and inline definitions",
                         "definition_lf_sha256": digest(actual_class.encode())})
        chunks.append("namespace " + version + " {\n" + actual_class)
        chunks.append(r'''
// Only unrelated object layout and container dependencies are stubbed here.
class TokenizeContext {
public:
  TokenizeContext(ObCollationType, ObIAllocator &, const char *, int64_t, bool);
  int init();
  int prepare_next_char();
  int step_next();
  ObCollationType coll_type_;
  const char *fulltext_;
  int64_t fulltext_len_, cursor_, next_char_len_;
  ObFTCharUtil::CharType next_char_type_;
  uint32_t handle_size_;
  bool is_smart_;
''')
        if version == "candidate":
            chunks.append("  bool utf8_fast_path_;\n")
        chunks.append("  LeafList token_list_, result_list_;\n};\n")
        for marker in ("TokenizeContext::TokenizeContext(", "int TokenizeContext::init()",
                       "int TokenizeContext::prepare_next_char()", "int TokenizeContext::step_next()"):
            chunks.append(copy(cpp, processor_path, marker, version))
        chunks.append("}\n")
    chunks.append(driver_text)
    for path, markers in extracted_pins.items():
        expected = {pin["marker"] for pin in input_entries[path]["definitions"]}
        if markers != expected:
            raise ValueError("Pinned extraction set changed: " + path)
    generated = "\n".join(chunks).encode()
    output.mkdir(parents=True, exist_ok=True)
    cpp_path = output / "utf8_prepare_leaf.cpp"
    cpp_path.write_bytes(generated)
    receipt = {"baseline_source_head": manifest["baseline_source_head"],
               "source_root": str(source), "manifest_sha256": digest(manifest_bytes),
               "generated_cpp_sha256": digest(generated),
               "extractor_sha256": digest(Path(__file__).read_bytes()),
               "test_driver_sha256": digest(driver_data),
               "files": file_receipts, "definitions": receipts,
               "scope": "Actual UTF8 leaf bodies and context methods; framework/logging/containers/get_charset initialization are stubbed; full SQL fixtures remain separate."}
    (output / "extraction.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return cpp_path, receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT,
                        help="Checkout root; default is this file at unittest/storage/fts, parents[3]")
    parser.add_argument("--output-dir", type=Path,
                        help="Default: <source-root>/build/fts-utf8-prepare-leaf")
    parser.add_argument("--run", action="store_true", help="Compile and run only when the parent performance measurement is idle")
    parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--random-cases", type=int, default=250000)
    parser.add_argument("--sanitize", action="store_true", help="Enable ASan+UBSan for GCC/Clang; not supported with cl/clang-cl here")
    args = parser.parse_args()
    if args.random_cases < 0:
        parser.error("--random-cases must be nonnegative")
    source = args.source_root.resolve()
    output = (args.output_dir.resolve() if args.output_dir is not None
              else source / "build" / "fts-utf8-prepare-leaf")
    cpp, receipt = generate(source, output)
    print("GENERATED", cpp)
    print("CPP_SHA256", receipt["generated_cpp_sha256"])
    if not args.run:
        print("NOT_COMPILED_NOT_RUN: use --run after the parent measurement is idle")
        return
    compiler = ([args.compiler] if Path(args.compiler).is_file()
                else shlex.split(args.compiler, posix=os.name != "nt"))
    executable = cpp.with_suffix(".exe" if os.name == "nt" else "")
    name = Path(compiler[0]).name.lower()
    if name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        if args.sanitize:
            parser.error("Use GCC/Clang C++ mode for --sanitize")
        command = compiler + ["/nologo", "/std:c++17", "/EHsc", "/O2", "/W4", str(cpp), "/Fe:" + str(executable)]
    else:
        command = compiler + ["-std=c++17", "-O2", "-Wall", "-Wextra", str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    print("COMPILE", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=cpp.parent, check=True)
    result = subprocess.run([str(executable), str(args.random_cases)], cwd=cpp.parent, text=True, capture_output=True)
    report = {"compiler_command": command, "exit_code": result.returncode, "stdout": result.stdout,
              "stderr": result.stderr, "executable_sha256": digest(executable.read_bytes()),
              "generated_cpp_sha256": receipt["generated_cpp_sha256"], "random_cases": args.random_cases}
    (cpp.parent / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
