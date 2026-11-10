# Round 15: JSON timeout-path hardening

Date: 2026-11-10.
Parent submission: `8e45f90e7b6e2e9d8742a58d7b7d9b7cc9e50f4a`.
Official round2 package: `f4d4e26653af823a7601668802f80e374ad6643c`.

## Failure interpretation

The platform reported one `JSON response timeout`. The unchanged official
transport uses that same message both when the initial ready frame exceeds the
120-second startup budget and when any later complete response exceeds the
60-second request budget. It reports different messages for process exit,
invalid JSON, or a wrong response ID.

The submitted framing itself was rechecked against the official contract:

- The Docker entrypoint runs the database process directly.
- Ready is emitted and flushed before stdin is read.
- Each request is read as one physical JSONL line.
- Every result and error is one newline-terminated JSON object followed by an
  explicit flush.
- Shutdown is acknowledged before exit.

No deterministic newline, flush, or stdout-pollution defect was reproduced.
The remaining response-time risks were a non-sequential disk scan after the
row cache filled and releasing a large rolled-back transaction before sending
the shutdown acknowledgement.

## Changes

- Uncached queries, updates, and deletes now traverse current row versions in
  file-offset order. The former 256-entry hash blocks still caused repeated
  long seeks on large files. SQL without `ORDER BY` remains unordered; physical
  traversal only prevents random I/O.
- The row-cache budget is selected from the Linux cgroup memory limit when no
  explicit `MVD_CACHE_LIMIT_MB` is set. The Windows/unknown-limit fallback stays
  at 192 MiB, while the cache is capped at 768 MiB and reserves memory for
  metadata, transaction overlays, result buffers, and the process.
- Float and vector output uses `std::to_chars` with the same nine significant
  digits and a checked `ostringstream` fallback, avoiding one stream allocation
  per vector component.
- Shutdown performs a logical O(1) transaction discard, flushes its JSON
  acknowledgement, and exits with status zero. The operating system reclaims a
  large private overlay instead of blocking the response on its destructors.
  EOF uses the same no-longer-visible transaction state before a status-zero
  exit.

## Regression coverage

- The non-cached suite now checks an unordered full-table scan in addition to
  ordered TopK and filtered TopK.
- The JSON contract suite creates an 8,000-row pending vector transaction,
  bounds the shutdown acknowledgement time, restarts, and verifies that every
  uncommitted row remains invisible.
- The existing official package remains unchanged. It covers 20 public groups
  and 95 SQL statements.

## Local verification

All 51 commands recorded in `round15-validation.json` passed on the final
source:

- Windows normal, scalar, and fault binaries.
- Official public assertions and the local portable official driver.
- JSONL framing, response IDs, Unicode, large results, restart, and shutdown.
- Crash/commit/catalog/result-replay failure injection.
- Six allocation-failure executables.
- Long differential and selection/recovery suites.

Linux/Docker validation is performed by the branch's GitHub Actions workflow,
including the unchanged official checker and hard-memory resource cases.
Passing public and local suites still does not expose the nine withheld formal
groups.
