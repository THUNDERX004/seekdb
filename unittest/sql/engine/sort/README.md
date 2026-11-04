# Shared sort metadata regression

Run on a dedicated seekdb instance after build/startup:

```sh
python3 unittest/sql/engine/sort/test_sort_metadata.py --port 2891 \
  --plans /tmp/seekdb-sort-plans.json
```

The default `sort_metadata_expected.json.gz` fixture contains 315 complete
ordered result sets recorded from official commit
`5bce9e06d1281347ad61344e4b01fab9158c58a2` (binary SHA256
`bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762`). The
test compares every row, column metadata and warning, and writes a new plan
report to the required `--plans` path. It creates and drops only its dedicated
`sort_metadata_r13` database.

Coverage includes one-to-six sort keys, direction and collation ties, NULLs,
binary boundaries, TopN/LIMIT/OFFSET, GROUP BY, ordered GROUP_CONCAT, windows,
rescans, unions, merge candidates and BENG/IK retrieval. Plan hints do not
guarantee an operator; inspect the plan report for actual coverage. The
official original lost its connection when `enable_newsort` was explicitly forced on,
so that mode is excluded and the failed attempts are retained in the round-13
evidence. Default, rowset-size and explicit encode-off modes remain.

This regression was recorded before a rejected sort-cache experiment and also
passed on its candidate. It is independent of the cache and remains useful for
future shared sort changes. No benchmark, build or CI files were modified for
the test.
