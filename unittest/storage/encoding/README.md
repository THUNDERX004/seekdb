# String-diff batch regression

`test_stringdiff_batch.cpp` is a standalone native regression entry point linked
against the actual production storage and allocator libraries. Compile this TU
with the production storage-unity flags and replace the observer `main` object
when linking; do not compile a copied decoder or add a private-access macro.
This test is separate from the official competition benchmark and is not wired
into its build or timing.

The 230 deterministic cases compare complete batch datum flags, lengths and
bytes with unchanged scalar decoding and independently generated source
strings. They cover fixed/variable layouts, raw/hex payloads, common-span shapes,
NULLs, duplicate/reordered row IDs, capacities 0/1/2/7/64, lengths around 64 and
128, allocation failures, a real forwarding row-index error after a published
prefix, same-array retry, allocation guards, output ownership and retained
batches. A successful run exits zero and prints `PASS\tcases=230` after 13,649
CASE/B/T/PASS record lines. Compare complete output between separately linked
control and candidate binaries, not just the final summary.

The exact source was linked and run on Windows x64 with the production flags
against both the original common-copy loop and the descriptor-major candidate;
full outputs were identical. Runtime evidence and actual link commands are
recorded with the full-text performance experiments. This does not establish
Linux CI coverage, encoder selection, malformed-metadata safety, concurrent
use, or absence of out-of-bounds reads. Fixtures use real packers, headers,
arenas and row indexes, but manually construct valid encoded metadata.
