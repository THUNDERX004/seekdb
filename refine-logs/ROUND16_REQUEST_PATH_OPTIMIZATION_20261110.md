# Round 16: request, storage, and ordered-output path optimization

Date: 2026-11-10.
Parent submission: `f65e5f6af3a54881072c498ff642f12a3e0ecb37`.

## Goal

Focus first on keeping every legal request below the judge's response deadline,
then improve measured paths without changing SQL, transaction, recovery, or
wire-format semantics.

## Changes

- Quoted vector literals are parsed in one pass instead of first constructing a
  token object for every component. Large vector batches no longer create
  hundreds of thousands of transient token and token-string objects.
- Multi-row INSERT moves parsed rows into the transaction map and reserves its
  duplicate-key set. The full input batch is no longer copied a second time.
- Row records write their fixed 32-byte header and payload directly into the
  append file. The on-disk bytes are unchanged, but commit no longer allocates
  and copies a second complete record containing the vector payload.
- Scalar ORDER BY keeps only the primary key and sort keys for the bounded
  candidate heap. Complete projected rows are retained only up to an 8 MiB
  budget. Larger results are emitted by a second current-row lookup, preventing
  full vector payloads from being resident merely to sort by `id` or another
  scalar key.
- An already-built physical row order is extended with newly appended committed
  offsets. Interleaved writes and queries no longer require a full metadata
  re-sort; the index is rebuilt only after stale entries exceed a bounded ratio.
- Database reads distinguish I/O failure from a clean/torn EOF, and writer
  construction closes its stream if initialization fails.

## Correctness verification

All 51 commands in `round16-validation.json` passed:

- Normal, scalar, and fault-injection builds.
- 20 public groups / 95 SQL in both executable modes.
- JSONL framing, Unicode, large responses, transaction visibility, restart and
  shutdown.
- Differential, exact-distance, statement allocation, commit allocation,
  catalog allocation, result replay and publication-failure suites.

The non-cached disk suite now checks unordered full scans, full `SELECT * ...
ORDER BY id`, and an inserted row after the physical index has already been
built. The container resource oracle now also checks a complete ordered vector
projection while streaming one row at a time.

## Development performance

Three paired Windows runs against `f65e5f6af` used the same seed:

| Workload | Phase | Parent median | Candidate median | Change |
| --- | --- | ---: | ---: | ---: |
| 10,000 x 128, 1,000 queries | durable writes | 0.539627 s | 0.493774 s | -8.53% |
| 10,000 x 128, 1,000 queries | queries | 0.195871 s | 0.181471 s | -7.35% |
| 5,000 x 4,096, 4 queries | durable writes | 5.665780 s | 4.852047 s | -14.36% |

Raw samples are in `bench/results/round16-optimization.json`. These are paired
Windows development measurements, not official Linux cgroup time, I/O, memory,
E, or ranking results.

Linux/Docker verification is provided by the branch workflow. Public success
does not expose the nine withheld formal groups.
