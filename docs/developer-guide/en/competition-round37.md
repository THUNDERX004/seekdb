# Round 37: scoped FTS build sorting

Retained production commit: `2e4434ec7230d29aebb992bc0e7b1f2dfc521d0a`.
The FTS auxiliary-schema gate keeps unsupported layouts on their existing
sorting path; the binary single-NUL correction and Windows scalar comparator
fallback accompany the scoped DDL change. Public-source attribution is retained:
PR2358 (`wwwyhyhyh/seekdb-2026`) commits
`ca66bb259de29e2a377d6ece00a135d04ec7108e` and
`86cd2ab9604e39ca409159ed32b06461fc58b01c` informed encoded sorting;
PR2302 (`liwan2/seekdb`) commit `6aeef682f433e93baaee21986e06525d82442f7a`
supplied the FTS-only gating structure and binary single-NUL fix. This is an
adaptation, not an authorship claim over those changes. Existing OceanBase
copyright and Apache-2.0 headers remain intact.

Parent evidence: the native preimage aborts with `0xC0000409`; the scalar
candidate passes 51,480 cases. Official SQL repro **a6** saves both EXPLAINs
and 1,027 encode-off rows, then loses the encode-on connection with the same
process exit code. Attempts a1–a5 were parent-harness failures before SQL.
The fixed-candidate repro passes full off/on comparison and the original-off
reference, with the server alive. No debugger stack was captured.

Final production-linked native receipt
`parent-build-currentlinked-1/native-run-currentlinked.json` records
`pass_bounded_candidate_core`: 13,686,384 pairs, zero core/diagnostic mismatches
and errors, 1,586,970 guard checks and 64 witnesses. It links the current
production encoder archive and real datum comparator. This is bounded
candidate-only evidence; that receipt's native baseline is NOT RUN and it
does not establish SQL callsite coverage or exhaustive collation equivalence.

One complete local A/B (affinity 15; unchanged 20,000-row fixed workload)
records build times **7.996/6.156/3.570 → 7.402/5.184/3.004 seconds**:
build category +13.0241766%, tokenize +1.4965925%, query +0.4757707%,
equal-category overall +4.9988466% time reduction. This is one incremental pair
against the accepted R33 binary, not an official score or stability estimate;
untouched metric changes are not attributed to sorting. The overall reduction
is neither five additional competition points nor a cumulative improvement.

The historical candidate label
`5bce9e06d1281347ad61344e4b01fab9158c58a2+round37-scoped-sort` is incorrect
source metadata, retained unchanged. Its binary SHA-256
`5b511d7cbc882671c3508839da9162e1e7940056ac34a68ef4fb7bc269302095`
matches the successful fixed-repro receipt identifying retained commit `2e4434ec`.
The parent maps this build to accepted R33 (`f1f93b43`) plus the R37 changes;
it is not a candidate built solely on the official original. The A/B control
SHA-256 is `596b93cd688e9ab47449391db1dfeb5acfd56095cee558b064f8d5a4a03f7437`.
Binary hashes and frozen source/patch identity govern this mapping, not the
mistaken label; the official-original binary remains the distinct `bae4a3b9…`.

The parent's focused sidecar passed 22 documents per bin/general_ci table.
The new self-contained repository test preserves its input and membership/byte
checks, adding unconditional finite-positive score validation. This new file
has not been executed during documentation preparation:

```sh
python unittest/storage/fts/test_encoded_fts_build.py --port 2891
```

It needs PyMySQL, rejects port 2881 and recreates only `fts_encoded_build`.
Both real FTS indexes are built; every returned stored field and the complete
OR membership are checked. Exact BM25 values, collation order, ordinary-index
access, spill/error behavior and actual encoded-sort execution are outside
this focused check. Absent EXPLAIN markers do not prove a path was disabled.
