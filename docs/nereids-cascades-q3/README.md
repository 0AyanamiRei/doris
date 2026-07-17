# Nereids Cascades 三表教学查询结果

这里保存《掌握 Doris Cascades 框架》配套查询的真实 `EXPLAIN` 输出。查询使用
TPC-H 的 `customer`、`orders`、`lineitem` 三张表，但不是标准 TPC-H Q3：它去掉了
聚合和 `LIMIT`，只保留三表连接、三个过滤条件、四列投影与 `ORDER BY`，便于第一次
学习时追踪每个阶段。

## 目录

| 目录 | 命令 | 作用 |
| --- | --- | --- |
| `parsed/` | `EXPLAIN PARSED PLAN` | parser 生成的未绑定逻辑树 |
| `analyzed/` | `EXPLAIN ANALYZED PLAN` | relation、slot、类型绑定后的树 |
| `logical/` | `EXPLAIN LOGICAL PLAN` | normalization/rewrite 收敛后的逻辑树 |
| `logical-process/` | `EXPLAIN LOGICAL PLAN PROCESS` | 每条 rewrite rule 的 before/after 过程 |
| `memo/` | `EXPLAIN MEMO PLAN` | Cascades 与 DPHyp 两种模式的完整 Memo |
| `physical/` | `EXPLAIN PHYSICAL PLAN` | 最终 Nereids PhysicalPlan、统计与 cost |
| `shape/` | `EXPLAIN SHAPE PLAN` | 去掉易变 ID/统计后的紧凑物理形状 |
| `distributed/` | `EXPLAIN DISTRIBUTED PLAN` | fragments、exchanges、workers 与 instances |
| `environment/` | `SHOW FRONTENDS/BACKENDS` 与行数 | 采样环境与数据规模 |
| `manifest.tsv` | 行数、字节数与 SHA-256 | 确认一组结果是否来自同一次完整采样 |

每个物理阶段同时保存 `cascades.txt` 和 `dphyp.txt`。三表查询默认不会因 join 数自动
进入 DPHyp；`dphyp.txt` 由 `enable_dphyp_optimizer=true` 显式启用。最终形状可能相同，
但 Memo 的搜索空间并不相同。

## 重新采样

默认连接本机 FE `127.0.0.1:9035`，数据库为
`regression_test_nereids_tpch_p0`。可通过环境变量覆盖：

```bash
DORIS_QUERY_PORT=9035 \
DORIS_DB=regression_test_nereids_tpch_p0 \
bash docs/nereids-cascades-q3/capture.sh
```

若需要密码，让 MySQL 客户端通过其标准配置或 `MYSQL_PWD` 获取；不要把密码写入脚本
或结果文件。采样固定 `parallel_pipeline_task_num=1`，目的是避免单 BE 教学环境生成大量
重复 instance 条目；它不用于性能结论。

## 当前采样边界

- 当前源码基线的普通 query 规划入口固定创建 `NereidsPlanner`；已标记为 `REMOVED` 的
  `enable_nereids_planner` 和 `enable_fallback_to_original_planner` 不是新老优化器选择开关，
  因此采样脚本不设置它们。
- `physical/` 是 Nereids `PhysicalPlan`；`distributed/` 中出现的 `PlanNode`、`Expr`、
  `PlanFragment` 是 translator 之后复用的执行层结构，不能据此判断 SQL 使用了旧优化器。
- 源码版本、tablet ID、ExprId、Group ID、instance ID、cost 和行数都可能随环境变化。
- `logical-process/result.tsv` 和 `memo/*.txt` 是完整原始输出，适合搜索，不适合从头逐行读。
- `EXPLAIN MEMO PLAN` 展示最终 Memo；DPHyp 的临时 DP table 仍需结合 DPHyp trace 或断点观察。
- HTML 教学文档中的“怎么读”章节给出了每份结果的入口、跳读顺序和本次输出中的关键证据。
