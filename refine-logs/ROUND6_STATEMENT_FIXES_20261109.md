# Statement validation and failure isolation — 2026-11-09

## Scope and versions

- Parent submission: 82d924b2191fbf87c96836358ec3c1937c5303bf.
- Official round2 branch rechecked at 9f6df2ba5acf871376e22b64a3d0724e4b575044.
- No changes in this round to mysqltest, Dockerfile, CMakeLists.txt, CI or the
  performance evaluator. The default wire protocol is unchanged.
- Tests use isolated temporary databases, never the installed database service.
- The available platform screenshot says only that MySQL admission failed. It
  does not identify the evaluated revision or the failing input. None of the
  defects below is asserted to be that platform failure's proven root cause.

## Reproduced defects and corrections

### Invalid transaction-control syntax changed autocommit

The parent accepted bare START, START WORK and repeated/mixed transaction words
such as BEGIN WORK WORK. After such a statement, an INSERT was unexpectedly
rolled back rather than committed automatically. The missing row persisted
across restart. Invalid syntax must be rejected before changing transaction state.

BEGIN now consumes at most one optional WORK or TRANSACTION; START requires one
TRANSACTION. Trailing tokens are rejected. Existing BEGIN, BEGIN WORK,
BEGIN TRANSACTION, START TRANSACTION, COMMIT WORK, END WORK and ROLLBACK WORK
have successful execution and restart checks. Invalid COMMIT/ROLLBACK tails
still leave previous transaction work available for normal resolution.

### VARCHAR without its length created a persistent table

CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR) used to succeed with an implicit
length of 65535. Subsequent valid CREATE TABLE with the same name failed, and
restart retained the wrongly accepted definition. The task specifies VARCHAR(n).
Missing length is now rejected before any catalog write. Explicit lengths and
Unicode-character length validation remain unchanged.

The final parser regression run against the preserved parent executable fails
with seven failure records; the corrected default and scalar executables pass
all five test methods. See round6-sql-before.txt and round6-validation.json.

### Allocation failure exposed part of a failed statement

A standalone test executable replaces global allocation and enumerates one
allocation failure at a time while calling the real database implementation.
Before the fix, the INSERT scenario's allocation failure #24 exposed id=4 even
though the multi-row INSERT failed: the expected three visible rows became four.
The exact failure output is retained in round6-allocation-before.txt. This is an
allocator-failure simulation, not an OS memory-cap or cgroup measurement.

INSERT, UPDATE and DELETE now construct their complete statement delta first.
Only after all required rows and map nodes exist is it published to the current
transaction. An empty target uses a constant-time map swap. Otherwise equal
allocators transfer map nodes and integer-key comparison cannot throw; conflicts
use optional<Row>::swap, statically checked to be nonthrowing. Potential outer
map allocation occurs before any visible row is changed.

The final standalone default and scalar tests each enumerate 28 INSERT,
22 UPDATE and 11 DELETE allocation failures. Every failing statement leaves
prior work intact; alternating COMMIT/ROLLBACK resolutions are checked after
closing and reopening the Database object from its files. Successful mutation
state and rollback are also verified. These allocation tests reopen the engine
in the same process; separate existing suites cover real process restarts. Allocation
replacement exists only in these separate test executables, not production.

## Final local correctness evidence

The expanded validator passed all 29 commands. This is a count of local commands,
not proof that the platform's 29 formal groups passed.

- Both default and scalar builds pass the public 20 groups / 95 SQL statements,
  previous semantic, random differential, exact-order, selection, stdio and
  SQL-edge suites, plus the five parser and eight pipe-boundary tests.
- The adapter regressions, six commit-publication failure scenarios and the
  longer randomized sequence pass. The production, scalar and fault variants
  were rebuilt from the same final source.
- Both standalone allocation tests pass, including transaction resolution and
  disk reload into a fresh Database object.
- Validation fingerprints confirm that source, tests and binaries did not
  change during the run. The raw transcript retains its final separator lines.

Records: round6-validation.json and round6-validation.txt.

The unittest failure transcript retains its tool-generated trailing header
space, and the full validation transcript retains separator blank lines.
Staged whitespace checks exclude only round6-sql-before.txt and
round6-validation.txt; other source, tests and documents remain checked.

Final worktree fingerprints:

| Item | SHA256 |
| --- | --- |
| src/minivectordb.cpp | 210c9deb57a0296144756c0b3fe934591a24a4d46441c0cc8fdc2420c31312d6 |
| Production executable | 9d3858e1acb253e82546cd160bb9d1aaef6792f8157225e636d94c814302d935 |
| Scalar executable | 70ecd3921db0ef6d564e816874c03f20c5261b3e3af2ba38e755f210cdb5dfd9 |
| Fault executable | 3da7546fd21ca4e920a99486b69218b4e3a5bf6ba6ed105a15002893ffcf9829 |
| Parent executable | 547610fd9fc78a0ed35c290c2bbfb47f59d6e988aa101dd2a33fd8cc41034d89 |

## Performance regression checks

The unchanged evaluator ran old and new executables serially in alternating
order, using fresh data directories. All content, query and restart checks
passed. Normal-cache runs use 10000 rows, 128 dimensions, 1000 queries, K=10,
batch size 100 and a 192 MiB row-cache budget. The low-cache path uses 3000 rows,
40 queries and a 1 MiB row-cache budget, with the other parameters unchanged.

Times are local pipe request/response medians in seconds:

| Group | Rounds | Parent write | Candidate write | Parent query | Candidate query |
| --- | ---: | ---: | ---: | ---: | ---: |
| Initial normal cache | 3 | 0.2827763 | 0.2630309 | 0.2309689 | 0.2102663 |
| Additional normal cache | 7 | 0.2846205 | 0.2736995 | 0.1884751 | 0.1909299 |
| 1 MiB row cache | 3 | 0.0881174 | 0.0874658 | 0.1718659 | 0.1701707 |

Initial samples varied substantially, so the next seven seeds were measured
without discarding the first three. The additional query median is slightly
slower; some individual write and query samples also worsen. The full positive
and negative samples are retained. These results support approximately unchanged
query performance, not a robust query speedup or official score improvement.

Raw reports: bench/results/round6-main.json, round6-main-followup.json and
round6-disk.json. Row-cache limits do not bound the entire process and do not
represent cgroup memory. Physical I/O, cgroup peak memory and official efficiency
or ranking were not measured.

## Remaining acceptance boundary

The complete official driver and withheld test inputs are not in the checked
public package. No Linux container/cgroup execution or formal competition
submission was performed. Local default/scalar/fault checks cannot establish
the platform's final acceptance. The exact pushed revision still needs formal
evaluation; the screenshot alone cannot identify which failure remains.
