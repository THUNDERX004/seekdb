# SQL input-boundary corrections — 2026-11-09

## Version and scope

- Parent submission: 5c8e7f664465c0a94892f64d76f3af47b54fa227.
- Official round2 branch was checked through GitHub and still points to
  9f6df2ba5acf871376e22b64a3d0724e4b575044. Its mysqltest files are unchanged.
- Tests use independent temporary databases, not the installed seekdb service.
- No third-party implementation, test, or document was imported.
- Prompt material remains excluded at the user's request. The manifest's
  stale prompt-delivery entry was removed; no replacement was generated.

## Demonstrated defects

### Overlapping block-comment delimiters

The streaming scanner reused the opening star of /* as the beginning of a
closing */. This input executed the DELETE hidden inside the comment:

~~~sql
CREATE TABLE t(id BIGINT PRIMARY KEY);
INSERT INTO t VALUES(1);
/*/;DELETE FROM t;*/SELECT id FROM t;
~~~

Before the fix, the process returned a syntax error, then OK 1 for DELETE,
then another syntax error. A separate restart confirmed that row 1 was gone.
The expected SELECT result is row 1. The scanner now consumes both opening
characters before examining the comment body. Three focused tests cover
comment-only input, mutation isolation/restart, and separate pipe writes.
All three failed against the original binary and pass after the correction.

### Malformed numeric tokens accepted as VARCHAR values

The lexer classified arbitrary mixtures of digits, exponent letters, dots
and signs as numbers. VARCHAR handling then accepted tokens such as 1e+
verbatim. Consequently, malformed INSERT/UPDATE could change data, and
DELETE ... WHERE s=1e+ could delete a row containing the valid string '1e+'.

Number scanning now follows sign, digits, optional fraction, and optional
exponent; an exponent requires digits. Remaining unexpected tokens are
rejected by the existing parser before execution. Existing numeric conversion
and quoted string handling are unchanged.

The independent regression exercises seven malformed forms through INSERT,
UPDATE, SELECT and DELETE. Each form has a separate table, successful prior
transaction work, COMMIT and a subsequent process restart. The previous binary
fails this regression; the corrected binary passes it.

## Local correctness evidence

The existing validation command ran all 23 commands successfully:

~~~text
python -X utf8 -B tests/validate_all.py --normal build/minivectordb.exe --scalar build/scalar/minivectordb.exe --fault build/round3-fault/minivectordb.exe --prefix refine-logs/round5-input-validation
~~~

The record includes normal/scalar public 20-case, 95-SQL runs, all existing
correctness suites, 12 stdio tests, 11 SQL-edge tests, public-adapter tests,
six commit-failure scenarios and the additional long differential sequence.
Source/test/binary fingerprints were unchanged during validation.

- Full record: round5-input-validation.json.
- Raw transcript: round5-input-validation.txt.
- The transcript preserves the harness's final separator blank lines; the
  staged whitespace check excludes only this generated transcript.
- Validated worktree source SHA256:
  63644985b21b2ddd7de243bf552f0c735e506e6d77e87394cf12870eacd21b09.
- Normal binary SHA256:
  547610fd9fc78a0ed35c290c2bbfb47f59d6e988aa101dd2a33fd8cc41034d89.
- Preserved pre-fix binary SHA256:
  f1fd4e9c348a3c0544ec13708b061dc654d59fe1d20f65d8c8d7149d1cee8d7c.

## Performance regression checks

All three reports passed their correctness checks and bind the baseline and
candidate binary hashes listed above. The existing evaluator was not changed.
Executables alternate order, use fresh databases, and are never measured in
parallel. Each run uses 10,000 rows, 128 dimensions, 1,000 queries, batch size
100 and K=10. Times are local request/response measurements in seconds.

| Sample group | Rounds | Baseline write median | Candidate write median | Baseline query median | Candidate query median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Initial normal cache | 3 | 0.3105467 | 0.3085178 | 0.2117535 | 0.2072306 |
| Additional normal cache | 7 | 0.2800204 | 0.2792502 | 0.1857109 | 0.1839301 |
| 1 MiB row-cache budget | 3 | 0.2786459 | 0.2822806 | 14.3693299 | 14.3036302 |

The initial three query samples had substantial variability. All are retained;
the additional seven use the next seven seeds rather than discarding any
initial run. Results are approximately unchanged, not a demonstrated speed
optimization. The additional seven filtered-query medians worsened from
0.0528518 to 0.0536677 seconds, and the low-cache write median also worsened.

Raw reports: bench/results/round5-input-main.json,
bench/results/round5-input-main-followup.json, and
bench/results/round5-input-disk.json. A 1 MiB row-cache budget is not a process
memory limit or a cgroup/physical-I/O measurement. It exercises the existing
disk-read path; it does not establish operation above the formal memory cap.

## Remaining validation boundaries

These are Windows development results, not official platform acceptance.
The complete official driver, withheld inputs and failure logs remain
unavailable. No Linux container or cgroup run was executed on this host.
No formal submission or new CI workflow was started. There are no existing
GitHub Actions runs attached to the parent submission checked in this round.
The unspecified official shutdown signal and process protocol were not
changed speculatively. Neither defect is asserted to be the platform failure's
root cause without a corresponding official reproduction.
