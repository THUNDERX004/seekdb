# round11 query-result materialization proposal

Unapplied auxiliary staging only. The active checkout is
`D:\Projects\OceanBase\seekdb`, HEAD
`36519bd8b3782c90f50565ebf6b233beba542600`. This package snapshots its current
raw files, including round10's three IK changes and the two token-iterator
galloping removals as context. No D: source, official benchmark, build file or
official README is modified. No build, native harness, database, fixture
recording, SQL regression or performance measurement was run for this proposal.

The four complete replacement sources are under `src/`:

- `sql/code_generator/ob_static_engine_cg.cpp`
- `storage/retrieval/ob_i_sparse_retrieval_iter.h`
- `observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`
- `storage/retrieval/ob_sparse_daat_iter.cpp`

`original/src/` contains the exact four original raw files. `context/src/`
contains read-only precondition snapshots, not application payloads. The
data-plane FTS header is also captured because Git status lists a raw
line-ending-only modification there; its normalized blob matches HEAD.
`manifest.json` records HEAD/branch, original HEAD Git blobs, raw Git blobs,
raw SHA256, LF hashes/blobs, replacement hashes and the complete artifact list.
`source.patch` contains only the four source changes; `regression.patch` adds
only the new regression script; `proposal.patch` combines both.

## Eligibility and output contract

`ObStaticEngineCG::generate_spec_final()` runs after normal table-scan spec,
filters, outputs and DAS attachment generation. It performs no new expression
generation and does not inspect SQL text or aggregate/function types.

The output array is cleared only if all conditions below hold:

- Successful code generation, ordinary `PHY_TABLE_SCAN` and `LOG_TABLE_SCAN`.
- Empty physical output, empty residual physical filters, and empty logical
  access expressions. Thus no SQL parent or residual filter consumes ID values.
- No index back, global index, lookup ctdef, index merge, ordering requirement,
  global-index rowkey expressions, partition-routing/PDML expression, group ID,
  transaction expression (including the pushdown expression), pushed aggregate,
  checksum reporting, paused scan or resumable scan.
- The final attach root is IR itself, has exactly one child and that child is
  this table scan's original scan ctdef. Lookup, sort, functional lookup,
  index-merge, match-score and vector wrappers cannot satisfy this root check.
- Natural-language mode, no relevance calculation or projection, no score
  filter, no pushdown top-k. The existing IR result output must contain exactly
  one expression, identical to its non-null internal domain-ID expression.

Only that final IR root's `result_output_` is cleared. It is an existing
serialized output-dependency array (`ObDASAttachCtDef` serialization); it is
not replaced with a serialized flag. Its child scan access/output arrays,
domain-ID expression and datum/object metadata are unchanged.
`ObTableScanCtDef::get_das_output_exprs()` returns this final root output array;
the outer DAS merge consequently sees no values to consume. Sequential DAS
iteration advances using returned counts, including `ITER_END` with a nonempty
tail batch. Eligibility excludes group-fold's group-ID dependency and ordered
consumers. No output pruning is performed on an IR nested inside another DAS
operation, even when the eventual SQL output is empty.

The legacy adapter derives `materialize_results_` from the empty output
contract only for non-top-k, non-functional, ordinary natural-language DaaT
with no score calculation/projection/filter. The parameter defaults to true
for every other user. TaaT and BMW retain their existing materialization even
when a value-free root's external array was pruned. This is a runtime member,
without any ctdef/rtdef serialization change.

## Correctness argument and exact saved work

`cache_result()` still increments input count once per accepted unique hit,
handles offset before output, checks both allocated buffer capacities,
increments returned count and output count, and returns the original limit
status. Only its ID `from_datum()` and score-buffer assignment are guarded.
Output buffers remain allocated exactly as before; allocation errors and
capacity errors therefore remain unchanged. `project_results()` skips value
writes and expression evaluated/projected flags when materialization is off,
but preserves the non-vectorized `count == 1` error check and the batch guard.

`id_proj_expr_` is never nulled. DaaT initialization still uses its type to
select the datum writer and its metadata to initialize the comparator. Token
scanning/decoding, all current internal IDs, heap/direct champion comparison,
duplicate draining, dimension advancement, next-round state, primary-key byte
comparison and ownership, `real_capacity`, offset/limit and end-of-stream
control flow are unchanged. No pointer to an ephemeral champion is saved as
an output substitute. Reuse resets existing merge/count state and retains the
same immutable plan dependency decision. Empty token sets, failures and the
last nonempty batch follow the original iterator code.

The observable result is the same number of rows/batches and the same return
codes. For eligible consumers, no datum is externally visible. Queries that
return IDs, use IDs in expressions/filters, need relevance/ranking, perform
lookup/merge or carry extra output metadata keep the original value path.
COUNT is only one possible consumer: value-free SUM, EXISTS and subquery
consumers can also qualify if the optimizer produces the required dependency
contract. Unsupported/uncertain plans simply fail eligibility.

## Regression, independently recorded expectations

The new independent path is
`unittest/storage/fts/test_query_materialization.py`, with its own expected
fixture `query_materialization_expected.json.gz`. No fixture is included or
generated. The script can run directly from this staging directory, without
copying it into D: first. It follows the existing PyMySQL complete-result
fixture style, and creates/drops only `fts_query_materialization_r11` on the
dedicated port 2891 when the parent explicitly runs it.

Coverage includes BENG/IK and signed, unsigned, binary and composite keys;
unsigned high-bit and integer extrema; binary NUL/trailing spaces; dense and
overlapping union streams; empty/NULL documents and search keys; exact hit
boundaries 1/255/256/257/511/512/513/1023/1024/1025/2048/2051; default and
`rowsets_max_rows` hint requests 1/256/513; zero/one/past-end limits and pages;
COUNT(*), COUNT(1), SUM(1), nullable SUM, EXISTS/NOT EXISTS, derived/scalar and
correlated subqueries, repeated rescans, IN/UNION consumers; complete ID
outputs and arithmetic projections; column/body/expression filters and
aggregates; complete score outputs, ranking and score filters; Boolean and
>256-token fallback/error paths; and SQL warnings. Child-statement variable
assignment adds a scalar-plan scenario using the current code generator's
scalar fallback, with an order-independent SUM result and reset before each
execution. An original server rejection of this assignment scenario or the
Boolean token-limit case is recorded and must match exactly; those cases are
never silently skipped. Hint requests do not prove an effective plan shape.

All rows, column metadata, byte values, NULLs, exact integer/decimal values,
warnings and explicitly allowed server SQL error numbers are compared. Only
floating-point result cells use the established 1e-12 absolute/relative score
tolerance. Unexpected server errors and every client/transport error fail;
there is no unsupported-feature skip. A missing, mismatched or incomplete
fixture fails before connection. Recording is explicit and exclusive (`xb`),
rejects existing paths before SQL, and requires official baseline commit and
independently verified original binary SHA256 metadata. The caller must
verify the live binary; supplied provenance alone cannot authenticate it.

Parent-only recording on the verified unmodified official competition binary:

```powershell
.\.venv\Scripts\python.exe .\setup\fts\round11-proposal\unittest\storage\fts\test_query_materialization.py --record .\setup\fts\round11-proposal\unittest\storage\fts\query_materialization_expected.json.gz --record-source-commit 5bce9e06d1281347ad61344e4b01fab9158c58a2 --record-source-binary-sha256 <VERIFIED_ORIGINAL_BINARY_SHA256>
```

Parent-only candidate comparison, after independent review/application/build:

```powershell
.\.venv\Scripts\python.exe .\setup\fts\round11-proposal\unittest\storage\fts\test_query_materialization.py --expected .\setup\fts\round11-proposal\unittest\storage\fts\query_materialization_expected.json.gz
```

Do not record from round10, round11 or another candidate. Retain the fixture
unchanged for candidate comparisons. SQL has not been executed here; support,
effective plans and scalar/batch execution must be checked by the parent.
Correct result comparisons alone cannot prove eligibility was reached.

## Parent application and remaining validation

Wait for round10's independent SQL/performance decision. Before applying, verify
HEAD and every current target's raw SHA256 against `original_content_sha256` in
the manifest. Verify context hashes too if the experiment is intended to use
exactly this round10-plus-galloping-removal input. If anything drifted, stop and
rebase the proposal rather than overwrite newer changes. Do not apply anything
under `original/`, `context/`, or the staging/audit helpers to the source tree.

After those preconditions, the parent can use the four complete replacement
files and add the independent regression, or review/check/apply `proposal.patch`
with Git in D:. No apply action is performed by any helper in this package.
Existing expected fixtures, official tests, SQL and benchmarks are untouched.

`static-audit.json` describes offline checks and the generated case count.
The combined patch also passed read-only `git apply --check
--whitespace=error-all` against the captured active checkout before delivery.
No C++ build, SQL support, dynamic correctness, optimizer branch reachability,
runtime allocation/error injection, scalar/batch integration or performance
PASS is claimed. First confirm empty-output IR eligibility and representative
ID/filter/ranking exclusions in actual plans, run the independently recorded
fixture and existing regressions, then measure the unchanged official workload
with all compilation/testing stopped. This proposal claims saved operations,
not a measured speedup.
