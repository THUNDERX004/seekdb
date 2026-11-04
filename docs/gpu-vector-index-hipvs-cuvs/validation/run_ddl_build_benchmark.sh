#!/usr/bin/env bash
set -Eeuo pipefail

CONTAINER=${1:?isolated container name required}
BASE_TSV=${2:?container TSV path required}
OUTPUT_DIR=${3:?output directory required}
DATABASE=${4:-p0_build_bench}
LIB=${5:-cuvs}
LABEL=${6:-candidate}

[[ "$DATABASE" =~ ^[a-zA-Z0-9_]+$ ]] || exit 2
[[ "$LIB" == "cuvs" || "$LIB" == "vsag" ]] || exit 2
[[ "$LABEL" =~ ^[a-zA-Z0-9_.-]+$ ]] || exit 2

mkdir -p "$OUTPUT_DIR"
trap 'echo "FAIL line=$LINENO output=$OUTPUT_DIR" >&2' ERR

sql() {
  docker exec "$CONTAINER" mysql --local-infile=1 -h127.0.0.1 -P2881 -uroot -ANB \
    -e "set ob_query_timeout=300000000; $1"
}

row_width=$(docker exec "$CONTAINER" head -n 1 "$BASE_TSV" | awk -F '\t' '{print $2}')
dim_count=$(printf '%s\n' "$row_width" | awk -F ',' '{print NF}')
((dim_count == 128 || dim_count == 256)) || {
  echo "expected a 128- or 256-dimensional TSV, got $dim_count" >&2
  exit 2
}

sql "create database if not exists $DATABASE; use $DATABASE;
drop table if exists items;
create table items(id bigint primary key, v vector($dim_count));
load data local infile '$BASE_TSV' into table items
fields terminated by '\\t' lines terminated by '\\n' (id,v);"

rows=$(sql "select count(*) from $DATABASE.items")
((rows > 0)) || { echo "empty benchmark table" >&2; exit 1; }
probe=$(docker exec "$CONTAINER" head -n 1 "$BASE_TSV" | cut -f2)

start_ns=$(date +%s%N)
sql "use $DATABASE; create vector index idx on items(v)
with (distance=l2,type=hnsw,lib=$LIB,m=16,ef_construction=200,ef_search=64);"
ddl_end_ns=$(date +%s%N)
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" \
  > "$OUTPUT_DIR/$LABEL-first-query.ids"
query_end_ns=$(date +%s%N)

result_rows=$(wc -l < "$OUTPUT_DIR/$LABEL-first-query.ids")
((result_rows == 10)) || { echo "first query returned $result_rows rows" >&2; exit 1; }

ddl_ns=$((ddl_end_ns - start_ns))
first_query_ns=$((query_end_ns - ddl_end_ns))
total_ns=$((query_end_ns - start_ns))
printf 'label,lib,rows,dim,ddl_ns,first_query_ns,total_ns\n' > "$OUTPUT_DIR/$LABEL.csv"
printf '%s,%s,%s,%s,%s,%s,%s\n' \
  "$LABEL" "$LIB" "$rows" "$dim_count" "$ddl_ns" "$first_query_ns" "$total_ns" \
  >> "$OUTPUT_DIR/$LABEL.csv"

cat "$OUTPUT_DIR/$LABEL.csv"
