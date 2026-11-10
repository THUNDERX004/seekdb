# Round 14: official JSONL protocol alignment

Date: 2026-11-10. Parent implementation: 97136509bfae6fe9b5b26a6acbf39a8a90d2c194.
Official protocol/package: f4d4e26653af823a7601668802f80e374ad6643c.
Both official branch heads were checked again during this change; the original
2026-competition baseline remains 5bce9e06d1281347ad61344e4b01fab9158c58a2.

## Confirmed mismatch and correction

The parent executable defaulted to raw SQL input and text output, with readiness
only on stderr. A real-pipe probe received no stdout handshake before input in
a 0.5-second diagnostic window. This is a contract mismatch, not an execution
of the formal 120-second timeout or proof of a particular historical platform
failure. The newly published protocol specifies JSONL, exact pre-input ready,
typed execute responses and acknowledged shutdown.

The default executable and unchanged Docker entrypoint now use that protocol.
The request parser decodes JSON strings/Unicode, rejects duplicate keys, and
preserves validated ID tokens without floating-point conversion. SQL is parsed
as one complete statement, optionally terminated by a semicolon; a second
statement is rejected before mutations execute.

Responses preserve BIGINT integers, vector arrays, finite distances, escaped
strings, column order, empty results, affected-row counts and boolean status.
Every response ends in one physical newline and is flushed. SQL errors do not
terminate a healthy session. An additional diagnostic regression exposed an
incomplete UTF-8 byte in errors for unsupported Unicode SQL tokens; JSON string
encoding now preserves valid characters and replaces invalid diagnostic bytes.

Each request sets its own exact/ann mode. Shutdown rolls back pending work,
acknowledges, flushes and exits; EOF also rolls back. Recovery precedes ready.
The legacy local interface requires explicit --stdio=text. Existing local
text clients now request it rather than influencing the production default.

Large SELECT results retain the existing 1 MiB in-memory budget. JSON row bodies
are staged with their delimiters on the data filesystem, not accumulated into
a second full response. Replay failures remain fail-stop and never append an
ordinary error frame to partial JSON. Small TopK responses remain memory-only.

## Windows verification

Final source SHA256: 8bff3ce5e850415afbecbdba1a60ba2a7df876ad1c8993973385e1aa09bb8a85.
Final normal executable SHA256: dbcdcdad97a57c14225cde8016e577c1095fdaf0a552664f26f10f5299597102.

All 51 commands in round14-validation.json/.txt passed on unchanged final
source, tests and binaries: normal/scalar SQL and stream regressions, JSONL
tests, public assertions, differential checks, transaction/catalog/result
fault tests and six independent allocation-failure executables.

Both normal and scalar passed the 20 published groups / 95 SQL through default
JSONL. tests/test_official_json_public.py imports the official case driver and
assertions unchanged and supplies portable thread-based pipes. This is not an
unchanged official transport run: its selectors implementation does not support
native Windows anonymous pipes. Linux CI runs protocol_check/run.py unchanged.

tests/test_json_protocol.py additionally checks typed/large/nested IDs, all
string controls and Unicode, SQL delimiters, vectors, empty results, repeated
mode changes, errors followed by success, shutdown/restart and results over
1 MiB. Its three optional malformed-input/EOF checks passed separately on both
normal and scalar. Malformed-request recovery is a local robustness check, not
a newly invented formal requirement. Result-replay faults verify exact partial
prefixes, nonzero exit, cleanup, production-hook isolation and complete retries
in both text and JSON modes.

## Performance observations

Three serial paired runs used the established 10000-row / 128-dimensional /
1000-query / K=10 workload, independent exact oracles and full-field restart
checks. All queries passed, with Recall@10=1.0 for both filtered/unfiltered
groups, and all 10000 recovered rows checked per run and executable.

Final default-JSON candidate versus the parent text interface, median seconds:

| Phase | Parent text | Final candidate JSON |
| --- | ---: | ---: |
| Durable writes | 0.2864180 | 0.2852066 |
| 1000 queries | 0.2012078 | 0.3125226 |

These are different wire/client paths. Timers include JSON serialization,
parsing, IPC and conversion to the existing oracle representation; they do not
isolate database CPU. The query increase is retained and is not advertised as
a speedup. Final raw samples and binary hashes: bench/results/round14-json-final.json.
The earlier pre-diagnostic-fix JSON run and text/text control are also retained
as round14-json-contract.json and round14-text-control.json, with their own
binary hashes; they are not measurements of the final executable. None of
these Windows measurements is an official cgroup metric, score or ranking.

## Linux and remaining scope

The workflow now exercises the unchanged official handshake/public checker on
the actual image and the scalar executable, the 51-command correctness suite,
mounted durability/EOF rollback, and default-JSON large results under 256 MiB
and 64 MiB hard limits with swap disabled. The resource oracle streams one row
at a time, retaining only a bounded parsing buffer and the unique-ID bitmap.
These stress outputs deliberately exceed the official checker's 16 MiB response
limit and are separate resource tests, not additional formal cases.

Linux results are pending the pushed revision's CI run when this report is
written. The official protocol/package files are preserved unchanged. Nine
formal groups still withhold inputs; public success cannot establish all
29 formal groups or identify historical failures without a platform result
for the exact locked revision. No formal submission is triggered here.
