#!/usr/bin/env bash

set -euo pipefail

capture_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
doris_host=${DORIS_HOST:-127.0.0.1}
doris_query_port=${DORIS_QUERY_PORT:-9035}
doris_user=${DORIS_USER:-root}
doris_database=${DORIS_DB:-regression_test_nereids_tpch_p0}

mysql_args=(
    mysql
    --host="${doris_host}"
    --port="${doris_query_port}"
    --user="${doris_user}"
    --database="${doris_database}"
    --connect-timeout=5
    --default-character-set=utf8mb4
    --batch
    --raw
)

query=$(<"${capture_root}/query.sql")
query=${query%;}

common_session="
SET disable_join_reorder=false;
SET parallel_pipeline_task_num=1;
SET enable_nereids_distribute_planner=true;
"

mkdir -p \
    "${capture_root}/environment" \
    "${capture_root}/parsed" \
    "${capture_root}/analyzed" \
    "${capture_root}/logical" \
    "${capture_root}/logical-process" \
    "${capture_root}/memo" \
    "${capture_root}/physical" \
    "${capture_root}/shape" \
    "${capture_root}/distributed"

"${mysql_args[@]}" -e "
SHOW FRONTENDS;
SHOW BACKENDS;
SELECT NOW() AS captured_at, DATABASE() AS database_name;
SELECT 'customer' AS table_name, COUNT(*) AS row_count FROM customer
UNION ALL SELECT 'orders', COUNT(*) FROM orders
UNION ALL SELECT 'lineitem', COUNT(*) FROM lineitem;
" > "${capture_root}/environment/result.tsv"
sed -i 's/[[:blank:]]\+$//' "${capture_root}/environment/result.tsv"

capture_explain() {
    local dphyp=$1
    local explain_prefix=$2
    local output_file=$3
    local extra_session=${4:-}

    "${mysql_args[@]}" --skip-column-names -e "
${common_session}
SET enable_dphyp_optimizer=${dphyp};
${extra_session}
${explain_prefix} ${query};
" > "${capture_root}/${output_file}"
    sed -i 's/[[:blank:]]\+$//' "${capture_root}/${output_file}"
}

capture_explain false "EXPLAIN PARSED PLAN" "parsed/result.txt"
capture_explain false "EXPLAIN ANALYZED PLAN" "analyzed/result.txt"
capture_explain false "EXPLAIN LOGICAL PLAN" "logical/result.txt"
capture_explain false "EXPLAIN LOGICAL PLAN PROCESS" "logical-process/result.tsv"

capture_explain false "EXPLAIN MEMO PLAN" "memo/cascades.txt" "SET dump_nereids_memo=true;"
capture_explain true "EXPLAIN MEMO PLAN" "memo/dphyp.txt" "SET dump_nereids_memo=true;"

capture_explain false "EXPLAIN PHYSICAL PLAN" "physical/cascades.txt"
capture_explain true "EXPLAIN PHYSICAL PLAN" "physical/dphyp.txt"
capture_explain false "EXPLAIN SHAPE PLAN" "shape/cascades.txt"
capture_explain true "EXPLAIN SHAPE PLAN" "shape/dphyp.txt"
capture_explain false "EXPLAIN DISTRIBUTED PLAN" "distributed/cascades.txt"
capture_explain true "EXPLAIN DISTRIBUTED PLAN" "distributed/dphyp.txt"

artifacts=(
    environment/result.tsv
    parsed/result.txt
    analyzed/result.txt
    logical/result.txt
    logical-process/result.tsv
    memo/cascades.txt
    memo/dphyp.txt
    physical/cascades.txt
    physical/dphyp.txt
    shape/cascades.txt
    shape/dphyp.txt
    distributed/cascades.txt
    distributed/dphyp.txt
)

printf 'artifact\tlines\tbytes\tsha256\n' > "${capture_root}/manifest.tsv"
for artifact in "${artifacts[@]}"; do
    line_count=$(wc -l < "${capture_root}/${artifact}")
    byte_count=$(wc -c < "${capture_root}/${artifact}")
    checksum=$(sha256sum "${capture_root}/${artifact}")
    checksum=${checksum%% *}
    printf '%s\t%s\t%s\t%s\n' \
        "${artifact}" "${line_count}" "${byte_count}" "${checksum}" \
        >> "${capture_root}/manifest.tsv"
done

echo "Captured Nereids EXPLAIN artifacts under ${capture_root}"
