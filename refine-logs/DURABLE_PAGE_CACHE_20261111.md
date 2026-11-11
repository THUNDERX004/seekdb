# Bounded advice for duplicate durable data pages

Baseline: `9d7930ee4d5d1e58de69567b440b13b3603ec99a`.
The pre-existing `build/performance-round4-20261111/baseline.cpp` is preserved;
its raw SHA256 is
`29cdccb42aac5c0941c2717e5f0c8036cf99565475d1730963be21c4e4ce842e`.
The shared Parser work belongs to the parent and is outside this patch.

## Method and invariants

`FileWriter::sync(bool discard_appended_pages = false)` retains its original
`fflush` and `fsync`/`_commit`, including their error handling. Only a successful
Linux sync can issue optional `POSIX_FADV_DONTNEED`. The table commit call passes
`table->fullyCached()`; WAL and catalog calls retain the default false.

The writer records its starting offset when opened and advances it after each
successful sync. For page size P, advice covers
`[floor(previous_offset/P)*P, floor(current_offset/P)*P)` only when bytes were
appended. This includes an earlier partial tail only once newly completed. It
never requests the current partial tail or a zero-length/to-EOF range. Each
call covers at most 64 KiB, rounded to complete pages. Unknown/unsupported page
sizes and offsets beyond off_t are skipped. An advisory error stops further
hints for that sync, preserves the successful durable commit and restores errno.
The helper allocates nothing, retains no additional file descriptors, and does
not add a sync, background operation, or global cache-reclaim request.

Advice occurs on the already open table descriptor immediately after data
fsync, before the unchanged WAL append/fsync and publication sequence. At that
point base rows are in the full-row cache and new rows remain in the overlay.
The cache can disable during publication or a later budget rebalance; subsequent
reads fetch the same durable bytes. This can increase later read I/O and needs
measurement. Any existing data/WAL sync failure still prevents normal success
and follows the existing recovery-required handling.

## Evidence and limits

Existing `build/performance-quantized-20261111/linux-memory.json` reports
6,217,728..6,225,920 bytes of candidate file-cache memory after load and queries,
with zero shmem, for 10,000 rows x 128D in 100-row durable batches. The benchmark
schema encodes 6,208,890 table bytes for this fixture, so approximately 5.92 MiB
of clean duplicate table pages is the potential target. This is a static estimate,
not a measured saving. The kernel/filesystem may ignore advice or retain pages.
Per-commit advice can reduce accumulation during loading; a single very large
commit can still establish its peak before fsync/advice. Every additional advice
call has overhead, and bounded ranges do not guarantee bounded kernel latency.

Recovery is explicitly unchanged: `Table::load` reconstructs both the row cache
and file cache. This patch targets data loaded through SQL and does not solve
startup/recovery peak memory. Reopening an existing database does not cause a
whole-file discard. Later cache eviction or restart can require additional disk
reads; evaluation must include those workloads, not only the fully cached case.

The previous transaction-spill experiment `505bddb8c` remains separate: its
report retained paired write/query speedups of 0.9720/0.9945 and did not establish
a performance gain. Existing 64 KiB stdio buffering, CRC processing, row/index
cache policy, Value layout, parser and query kernels are unchanged by this patch.

## Validation

`tests/test_durable_page_cache.cpp` is a Linux standalone test using link-time
wrappers only in its test executable. Range tests audit complete pages across
partial tails, repeated syncs, reopen, advice-off syncs, 64 KiB and unsupported
page sizes, advisory errors, errno restoration and a real-sync failure. Database
tests compare exact table/WAL/catalog bytes with real advice versus injected
ENOSYS, verify exactly two syncs per one-table commit, require advice only on
cached table streams, force cache eviction, and verify exact restart contents.
No test requires the host to actually evict a page or uses a timing threshold.

Build/run later on Linux (no heavy concurrent benchmark or build was started):

```sh
c++ -std=c++20 -O0 -DMVD_FORCE_SCALAR tests/test_durable_page_cache.cpp \
  -Wl,--wrap=fsync -Wl,--wrap=sysconf -Wl,--wrap=posix_fadvise \
  -o build/test_durable_page_cache
python -B tests/test_statement_allocation.py build/test_durable_page_cache
```

Linux execution and performance validation are pending. Also retain the existing
commit-publication, commit-allocation, disk-scan, cached-view eviction, extended
recovery and constrained-container suites. Compare fresh-cgroup memory.peak,
memory.stat file/anon, write latency, read I/O and complete persisted contents.
No platform score or end-to-end speedup is inferred from this source change.
