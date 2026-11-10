# Bounded payload retention for ordered scalar projections

Parent: 6fb385e05fa47ac6bb3da91584c0506c5addd3ac.

## Measured problem and fix

With 20000 rows, VECTOR(1024), a short VARCHAR and an 8 MiB row-cache cap,
the vector payload alone is 81920000 bytes. SELECT id,tag FROM t ORDER BY id
previously copied every unprojected vector into the general sorting buffer.
The independently built exact parent returns correct rows but reaches a
99385344-byte Windows peak working set during that query, versus 15917056
bytes after writes. The final implementation stays at 15900672 bytes in both
phases, and all 20000 ID/tag results are checked exactly.

The general sorting path now preserves only output source columns, sorting
source columns and the primary key in ephemeral row copies. Stable cache and
transaction-overlay rows remain borrowed. The mask is initialized only when
a disk row actually survives selection. SELECT *, unprojected sort keys,
distance-only output and hidden distance ordering retain every required field.
The specialized distance TopK path is unchanged.

Reproduce the local process-memory comparison:

~~~text
python -X utf8 -B bench/evaluate_projection_memory.py --baseline <parent-executable> --candidate <candidate-executable> --report bench/results/projection-memory.json
~~~

Raw output: ../bench/results/round12-projection-memory.json. This is Windows
process-memory evidence with an explicit cache cap, not an OS memory limit.

## Verification and rejected variants

tests/test_selection_retention.py adds 34 independent exact-field queries with
2048 disk-backed rows, a primary key outside the first column, long VARCHAR,
projected vectors, hidden distances, aliases, OFFSET, unlimited ordering,
SELECT *, uncommitted overlays, rollback and committed replacements. Its
expected ordering and payloads are generated independently in Python. The
suite is included in both modes of tests/validate_all.py.

The finalized normal, scalar, fault and six allocation-test executables pass
all 44 validation commands, with stable source/test/binary fingerprints.
Raw full output is round12-validation.json and round12-validation.txt.

Two broader variants were not retained: an eager per-query mask and a shared
lazy mask used by the specialized TopK path. Their five-round standard-query
paired parent/candidate ratios were 0.9523963 and 0.9611816 respectively.
Their raw records remain round12-eager-main.json and
round12-shared-lazy-main.json in bench/results. These are intermediate-source
experiments, not validation of the final executable. The first exploratory
run may overlap sidecar regression work, so it is not an isolated performance
estimate. The final comparison ran after sidecar database testing ended.

Final serial paired measurements against the exact archived parent, seconds:

| Workload | Parent write | Candidate write | Parent query | Candidate query | Paired write ratio | Paired query ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Standard, 5 rounds | 0.2641922 | 0.2641774 | 0.1820435 | 0.1785035 | 1.0271611 | 1.0244271 |
| 1 MiB row cache, 3 rounds | 0.0869737 | 0.0819877 | 0.1684664 | 0.1677769 | 1.0468152 | 1.0004017 |

The standard workload uses 10000 rows, 128 dimensions, 1000 queries, batch 100
and K=10. The low-cache workload uses 3000 rows and 40 queries, with the other
parameters equal. Both use integer seeds starting at 20261610. Each run owns
a fresh database, alternates executable order, verifies every query and all
columns after restart, and does not run benchmark executables concurrently.
Ratios are paired parent/candidate medians; below one is slower. Standard
query ratio minimum is 0.9807532 and low-cache minimum is 0.9860047. All samples
are retained. Small timing differences are not an official speedup or score.
Raw records: ../bench/results/round12-main.json and round12-disk.json.

Final source SHA256: 82514d10e678bb6ba682b27250a9d1d7b655584edf3ce5a299998633f285f7f0.
Normal executable: da87738c6f484ae80abccbbf9ce4afffdc4e0d4fb69c0d28cedc6a7baed44e72.
Scalar executable: 125b5ac3de5b6a2c8a8a3a6c94817184618586e2df8ddeca13028b2194ea9b3c.
Fault executable: 14ab99e787cbdd01667d12fa5e7c2fe1132877d696ad305542f1db05dbeeb333.

## Linux resource check and acceptance boundary

The preceding parent completed the actual Docker build, all 42 Linux
correctness commands and container entrypoint/durability/EOF smoke checks.
Its exact run evidence is round11-linux-validation.json.

The updated Linux workflow also runs tests/test_container_resources.py with
327680000 bytes of vector payload under a 256 MiB hard limit, no swap, no
network and the image's default cache setting. A second check uses 81920000
bytes under 64 MiB with an explicit 8 MiB row-cache cap. Each verifies writes,
exact and filtered TopK, the full ordinary ordered scalar projection and two
EOF restarts. Containers have unique ownership labels, bounded waits, OOM
diagnostics and cleanup. Resource failures remain ordinary test failures.
These additional Linux results are pending until the new CI run completes.

No official fixtures, scoring inputs, Dockerfile or CMake were changed.
The public package still omits the full driver and nine groups' inputs.
Local/CI success does not establish all 29 formal cases or diagnose the
previous platform failure without a corresponding platform log/result.
