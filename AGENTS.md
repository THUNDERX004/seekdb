# Project Agent Guidance

## C++ Type Naming

- New C++ classes, including interface classes, do not need the legacy `Ob` prefix.
- Keep the `I` prefix for interface classes. For example, use `ICacheMemoryGetter` instead of `ObICacheMemoryGetter`.
- Do not rename existing types only to remove the `Ob` prefix unless the task explicitly requires it.

## C++ Headers

- Use `#pragma once` in new C++ header files instead of macro-based `#ifndef`/`#define` include guards.

## Code Review

- For pull request or diff review tasks, read and follow `.agents/skills/code-review/SKILL.md`.

## Test Placement

- Prefer adding regression and correctness coverage to the repository's established test suites and test directories.
- Avoid adding test-only or benchmark-only files under `tools/` unless the task specifically requires a reusable developer tool.

## 2026 Competition Baseline

- Base competition submissions on the official `upstream/2026-competition` branch, not `master`.
- The official baseline verified on 2026-11-04 is `5bce9e06d1281347ad61344e4b01fab9158c58a2` (`upload 2026-competition testcases`). Verify the remote branch before future competition work.
- The corrected full-text submission branch is `codex/fts-performance-competition`. The original `codex/fts-performance` at `a678df23244166a50e622f3e8a4ee89b70adb340` used the wrong master baseline and is historical only.
- Before submitting, verify that the official baseline is an ancestor of HEAD, and compare the submission with that baseline. Preserve the official benchmark data, SQL, timing, scoring, baseline JSON, and build/CI files.
- Previous FTS performance reports identify master `e3c04befd2c9b6559f0005c4bf72c62e244cf06c` as their source; they do not validate competition performance. Regression fixtures were independently re-recorded on the official competition original during migration. Read `docs/developer-guide/assets/fts-performance/competition-migration.json` for the build configuration, binary hashes, and validation evidence.
