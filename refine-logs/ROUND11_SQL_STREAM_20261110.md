# SQL identifier resolution and byte streams

Parent: a4bc7ff6ac35e6c07203ee8bc131a175325eef75.

The official branches were rechecked on 2026-11-10: 2026-competition remains
5bce9e06d1281347ad61344e4b01fab9158c58a2, and 2026-competition-round2 remains
9f6df2ba5acf871376e22b64a3d0724e4b575044. Both are ancestors of this branch.
Official mysqltest files, Dockerfile, CMake and existing CI files are unchanged.

## Reproduced corrections

The parent mistakes an ordinary l2_distance column or alias for a function
call, producing an expected-parenthesis error. Function recognition now also
requires an opening parenthesis. In addition, an unaliased distance expression
uses its source column as a display label. That label must not become a SQL
alias and silently change ORDER BY vector_column into distance ordering.
Explicit distance aliases still resolve normally. tests/test_sql_round11.py
has three methods and reproduced eight failing assertions on the parent;
all now pass.

On Windows, text-mode standard streams discarded CR in literal CRLF, treated
control-Z as EOF, and changed literal LF to CRLF in output values. The entry
point sets stdin and stdout to binary mode before disabling stdio sync.
tests/test_protocol_round11.py reproduced three failures on the parent and now
passes all eight methods. This is a local byte-preservation correction, not
proof about an unpublished official output encoding.

## Local verification

Rebuilt normal, scalar, fault-injection and six isolated allocation-test
executables pass all 42 commands in tests/validate_all.py. The run includes
both public 20-group / 95-SQL checks, current SQL/stream regressions, the long
random differential run, fail-stop recovery and allocation-failure checks.
Source, test and executable fingerprints remained unchanged during validation.
Raw results: round11-validation.json and round11-validation.txt.

Executable SHA256 values:

| Mode | SHA256 |
| --- | --- |
| Normal | 09b2e66673ecb11141b0ad561b4efd8130416f3619644d6107f8b4cfc9e478c4 |
| Scalar | 3abb0441117fef2f8ec8c2248f977bba91a8a1e4d3d0176726ed12b9d7ebff02 |
| Fault | ac29ee919e1c7e258dff29e9176da3f4d8146d0f43e67936327479d9cc484647 |

## Serial paired measurements

The unchanged evaluator compares the archived exact parent against the current
normal executable. The standard workload runs five rounds of 10000 rows,
128 dimensions, 1000 queries, batch 100 and K=10. The 1 MiB row-cache workload
runs three rounds of 3000 rows and 40 queries, with the other parameters equal.
Both use seeds beginning at 20261510, an integer seed rather than a date.
Each executable/run has a fresh database. Every query and all-column restart
check passes; executable order alternates and measurements are serial.

Median request/response seconds:

| Workload | Parent write | Candidate write | Parent query | Candidate query | Paired write ratio | Paired query ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Standard, 5 rounds | 0.2891380 | 0.2860178 | 0.1890912 | 0.1883566 | 1.0109091 | 1.0039000 |
| 1 MiB row cache, 3 rounds | 0.0948529 | 0.0915188 | 0.1730039 | 0.1725953 | 1.0298503 | 1.0007520 |

Ratios are paired parent/candidate medians; below one is slower. Standard
query ratios range from 0.9607431 to 1.0296823. The low-cache candidate has a
0.5361314-second write sample, with paired ratio 0.1769210; that negative sample
is retained. These timings do not establish a speedup or zero regression.
Raw reports: ../bench/results/round11-main.json and round11-disk.json.
Windows process diagnostics are not cgroup memory, physical disk I/O or an
official performance score. The row cache cap is not a process memory limit.

## Linux and formal acceptance

A new, separate .github/workflows/minivectordb.yml builds the existing pinned
Dockerfile, tests its extracted Linux executable plus scalar/fault/allocation
variants built with the same pinned GCC image, and checks the actual image
entrypoint, mounted durability and EOF rollback under a 256 MiB container cap.
It has read-only repository permission and no retained checkout credentials.
YAML parsing and shell syntax checks pass locally. Linux execution results
remain pending until its run finishes; no existing workflow was changed.

The public package still withholds nine groups' inputs and explicitly omits
the full driver. No actual platform failure input/log or result for this
candidate is available. These fixes are proven local defects, not a diagnosis
of the reported formal failure. No formal evaluation is submitted here.
