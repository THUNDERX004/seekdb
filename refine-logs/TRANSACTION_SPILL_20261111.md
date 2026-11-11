# Bounded transaction payload follow-up

Base: b235ce1819cf8b41a8426cd01ca114476441ed80, independently accepted by the
platform according to the user on November 11, 2026. This follow-up remains on
an isolated branch while benchmark performance is prioritized.

A scalar UPDATE over20,000rows×4096dimensions retained every full vector in
the transaction overlay. Windows working set grew from12.2MB to347.9MB.
Private transaction payloads now share a bounded store and spill to an anonymous
file under the data directory, with no new persistent storage format.
Row metadata remains proportional to changed keys. The store defaults to8MiB
or detected cgroup memory/16 (minimum256KiB), and can be overridden using
MVD_TRANSACTION_MEMORY_MB; zero forces spill for deterministic regressions.

Changed rows expose whether borrowed row storage is stable. Temporary spilled
rows are copied only by callers retaining them; pending commit records retain
metadata only. Statement publication remains allocation-free after staging.
Commit writes and syncs the same durable records before its WAL commit marker.
Scratch files are automatically removed on rollback, shutdown and process death.

Validation:55Windows commands passed, including normal/scalar public suites,
27,034-statement randomized testing, statement/commit/catalog allocation failures,
five new forced-spill SQL/recovery tests and temporary write/read fault tests.
A16MiB vector fixture completed UPDATE, long INSERT, COMMIT and ROLLBACK within
an additional6MiB live-allocation cap (observed peak delta2,149,104bytes).
The hard-memory Docker regression was extended with long transactions and bulk
updates but has not yet been executed for this branch; no Linux claim is made.

Paired3-run10k×128Dbenchmark againstb235 did not show a stable performance gain
(paired median query speedup0.9945, write0.9720). Keep separate from the active
performance submission until a specific need is established.
Official mysqltest/protocol files and agent/conversation.json are unchanged.
