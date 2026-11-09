# Direct float32 decimal input conversion

## Scope and parent

Parent: 398575eb23bc47d37e209c8c041f377c765f41f2, branch
2026-competition-round2. The official upstream tip was rechecked on
2026-11-10 and remains 9f6df2ba5acf871376e22b64a3d0724e4b575044.
The official mysqltest fixtures, existing evaluator, Dockerfile, CMake and
CI files are unchanged. This is a normal numeric SQL-correctness correction,
not evidence that the earlier platform failure has been diagnosed.

## Concrete wrong TopK result

The old parser converts a decimal token to double, then casts it to float.
The decimal 1.0000000596046448 is strictly greater than the exact midpoint
between float32 1 and its successor 1 + 2^-23. Its closest float32 is that
successor. The intermediate double instead lands on the midpoint, and the
second rounding chooses 1. A SQL counterexample is:

~~~sql
CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1));
INSERT INTO t VALUES(1,'[1.0000000596046448]'),(2,'[1]');
SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1]') LIMIT 1;
~~~

The parent returns ID 1; the exact decimal-to-float32 interpretation requires
ID 2. This is a membership/order error, not just a printed distance differing
within the allowed tolerance. A similarly constructed query probe can choose
the wrong row even when both stored vectors already have exact float32 values.
Decimal literals just below the overflow midpoint may also be incorrectly
rejected after the intermediate double rounds to that midpoint.

## Repair

parseFloat32Token parses directly into float with from_chars, so INSERT,
UPDATE, selected-distance probes and hidden ORDER BY probes share the same
conversion. There is no decimal-to-double-to-float path.

from_chars reports a result rounded to zero as a range error. Such a fully
consumed numeric token is distinguished from overflow by comparing its exact
decimal order with one, using its first nonzero digit, decimal position and
explicit exponent. This path performs no floating-point conversion. If the
exponent cannot fit int64, its sign decides which side of the bounded
significand order it lies on. Tiny values become signed zero; positive or
negative overflow is still rejected. NaN, Infinity, malformed numeric values
and statement atomicity checks are not relaxed.

## Independent regression evidence

tests/test_float32_literals.py uses Fraction and a binary search over the
finite float32 lattice to choose nearest representable values with even ties.
It does not use Python's decimal-to-double conversion as the rounding oracle.
Midpoints and values on either side are constructed as exact rational numbers,
then emitted as exact terminating decimals. The oracle has its own tie and
direction self-checks.

The final suite has one oracle method and six database methods. It covers 52
ordinary/subnormal/large boundary literals, stored TopK membership, hidden and
selected query probes, explicit primary-key ordering, UPDATE, both signs near
the overflow midpoint, nine extreme-underflow/zero literals, and atomic
rejection of overflow/nonfinite values. Returned vector values are checked
back to float32 bits; both signed zeros may be printed as 0, which does not
change their numeric value or L2 distance.

round10-float32-before-final.txt records the final strict suite on the archived
parent: seven methods, 31 failed assertions. round10-float32-after.txt records
the fixed default executable passing all seven methods. The earlier
exploratory output round10-float32-before.txt is not this final regression:
its signed-zero output assertion was unnecessarily strict and was corrected
before the final comparison. It is not counted as additional proven defects.

The complete rebuilt default/scalar/fault set and six allocation-test programs
pass all 38 local validation commands, with unchanged source/test/binary
fingerprints throughout. This includes the public 20 groups / 95 SQL in both
arithmetic modes, all previous SQL/stream/differential/selection/recovery
contracts, and the new numeric suite in both modes. A local command count is
not a formal platform group count. Raw full output is round10-validation.json
and round10-validation.txt.

## Serial local performance comparison

The unchanged evaluator ran five standard-cache rounds (10000 rows, 128
dimensions, 1000 queries, batch=100, K=10) and three 1 MiB row-cache rounds
(3000 rows, 40 queries, other parameters unchanged), with seeds starting at
20261410. Those seeds are integers, not dates. Execution order alternates,
every database is fresh, and every run passes query and all-column reload
checks. The baseline is the exact archived parent default executable.

Request/response medians, seconds:

| Workload | Parent write | Candidate write | Parent query | Candidate query | Paired write ratio | Paired query ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Standard cache, 5 rounds | 0.3438390 | 0.2927000 | 0.1903661 | 0.1840649 | 1.01842 | 1.00449 |
| 1 MiB row cache, 3 rounds | 0.5382334 | 0.5212541 | 0.1702156 | 0.1705144 | 0.96343 | 0.99833 |

Paired ratios are parent/candidate; a value below one is slower. The low-cache
paired write median is therefore a negative observation, despite its ratio
of absolute medians pointing in the opposite direction. Standard-cache
candidate writes include a 1.3013704-second sample, and the smallest paired
write ratio is 0.39324. No samples are excluded. Query totals are approximately
unchanged; these measurements do not prove a speedup or zero regression.

Raw reports are bench/results/round10-main.json and round10-disk.json. Timing
includes serial pipe exchange and durable writes, excluding SQL generation,
oracle, startup, DDL, shutdown and reload verification. Windows process
diagnostics are not physical-device I/O, cgroup memory, official E or ranking.
The cache budget is not an OS-enforced total memory limit.

## Fingerprints

Canonical source blob: dd0d7340af7b7b03245147ffbe1b68fb38f0ab4d.

| Item | SHA256 |
| --- | --- |
| Worktree source | aa8b25629993ef08aded8186665b7198c718d4e262c673beb628a3d37a976217 |
| Default executable | a9196c7871c313863816d3641ae07d2e41696ece712a97a71b62e1c5f4151627 |
| Scalar executable | 792a8acddd716e0ba4ae2ea91eb7b76436678f7934903d3da944497966a69eb6 |
| Fault executable | 4f6e1cd71e0574247c6596f3137de0cfd20eb8178e416256fdfeb4026a81656e |

## Formal acceptance boundary

The official branch still provides no complete process driver, and a direct
request to the supplied task-document URL returned HTTP 403 in this follow-up.
No alternate access path was used. There is no new formal failure log or
formal platform result for the candidate. Docker is not available as an
installed command; no Docker/WSL installation, external Linux executor,
container execution, cgroup measurement or formal submission was performed.
The unchanged local stdout interface is not asserted to match the unknown
formal framing. The full objective remains unproven.
