# Doris Holistic Subquery Unnesting 第一阶段实现说明

## 1. 交付范围

本实现以 `docs/holistic-subquery-unnesting-2025-design.md` 为架构基线，在 Nereids 中落地：

- 2015 Domain-D 算法的可执行主路径：为相关 binding 构造去重域，把域下推到子查询内部，最后以
  NULL-safe identity join-back；
- 常见相关标量聚合：`MAX`、`MIN`、非 DISTINCT `COUNT`；
- 相关 predicate：普通等值、非等值、多个 outer slot、`AND`/`OR` 组合；
- 子查询内部不访问 outer slot 的 inner/cross join 子树，其中 inner equal join 是主要验收形态；
- Filter、非 DISTINCT Project、global Aggregate 三类 transfer function；
- 2025 root-first/parent-aware 框架雏形：支持两层 scalar aggregate Apply，并从父 binding 与
  nested-left 的真实可达行构造 compound Domain；
- 完整 SQL bag、NULL binding、global aggregate 空输入及 Doris 原有 scalar output contract。

第一阶段有意不放开 outer/full/semi/anti/mark join handler、grouped aggregate、Window、TopN、SetOp、
CTE/recursive CTE、volatile/side-effect expression。未覆盖形态继续走 Doris legacy rewrite 或保持原错误。
功能通过实验 session variable 开启：

```sql
SET enable_holistic_subquery_unnesting = true;
```

默认值为 `false`。

## 2. 在 Doris pipeline 中的位置

新 pass 位于 whole-tree `Subquery unnesting` topic 内：

```text
SubqueryToApply
  -> expression/plan normalization
  -> PullUpProjectUnderApply + MergeProjectable
  -> HolisticApplyEliminator             # 新增，原子尝试
  -> CorrelateApplyToUnCorrelateApply    # 未命中时保留 legacy
  -> ApplyToJoin
  -> ordinary Nereids rewrite / Memo / physical planning
```

`Rewriter` 的普通 whole-tree 路径和 MV rewrite 使用的 CTE-children 路径均接入同一 custom job，避免两条
规划入口能力不一致。成功候选在进入后续规则前已经全部 lower 成 Doris 现有
`LogicalProject/Filter/Aggregate/Join`，没有增加 BE operator，也不会把临时 Domain 节点带入 Memo。

## 3. 分析期融合

### 3.1 多层 outer binding

原 `ExpressionAnalyzer` 只查直接上一层 Scope。开关开启后，未绑定 slot 按 SQL lexical shadowing
顺序沿完整 outer-scope chain 查找；命中祖先 slot 后，把该 slot 记录到所有跨越的 subquery boundary。
因此最内层 Apply 能携带直接父层和祖父层相关列，中间 Apply 也知道需要转发祖先 binding。

默认关闭时仍只查上一层，现有查询行为不变。

### 3.2 Analyzer relaxation

legacy `CorrelatedSlotsValidator` 仍用于单层查询。只有开关开启且子查询计划中已经出现 nested
`LogicalApply` 时，才允许多个相关 access site；Project/Aggregate/Join/Sort 自身直接访问 outer slot 的
现有检查仍保留。这样不会为普通单层 unsupported SQL 宽泛解除白名单。

## 4. 逻辑框架

### 4.1 `CorrelationPlanIndex`

pass 开始时只读扫描原计划，按 `LogicalApply.getId()` 建立：

- parent Apply；
- 当前 Apply 的 outer references；
- RHS 中真正访问这些 outer slot 的 plan nodes。

索引采用 2025 的 root-first 所有权模型。第一阶段只索引树边；CTE DAG 与 recursive edge 留待后续扩展。

### 4.2 `DomainSpec`

`DomainSpec` 是 lazy domain 描述：

```text
source plan + (original outer slot -> source representative)
```

到 stop point 才 materialize：

1. 使用 `LogicalPlanDeepCopier` 复制 source，避免原外表分支与 Domain 分支共享 RelationId/ExprId；
2. 消费 `DeepCopierContext.exprIdReplaceMap` 找到 fresh key；
3. 以 `LogicalAggregate(groupBy=keys, output=keys)` 形成真正 duplicate-free 的 `D`。

第一阶段仅接受可重复、无 `NoneMovableFunction` 的普通关系 source，并限制 source operator 集，防止
duplicate-tree 改变 volatile 或 semantic assert 的求值次数。

### 4.3 `UnnestingState` 与结果合同

`UnnestingState` 保存当前 Apply、outer refs、DomainSpec 和 parent state；
`UnnestingRewriteResult` 同时返回 rewritten plan 与每个 outer ref 在当前 output 中的 replacement。
Project 和 Aggregate 改变 output layout 时显式更新 replacement，父节点不按列名猜 binding。

## 5. 2015 transfer functions

对当前 binding 状态递归处理 RHS：

- 子树不再访问 outer slot 且没有 nested Apply：在该 stop point 插入 `D CROSS JOIN subtree`；
- Filter：递归 child，再以 replacement 改写完整 predicate，因此 OR/non-equi 不需要拆成等值条件；
- Project：改写 project expression，并追加隐藏 binding output；
- global Aggregate：改写 aggregate argument，把 binding representation 同时加入 group-by 和 output；
- 独立 inner equal join：作为 stop-point 子树整体接收 D，保留原 join condition 和 bag 语义；
- Apply boundary：原左输入与 rewritten RHS 以每个 key 的 `NullSafeEqual` join-back，最后用 boundary
  Project 恢复原 Apply output ExprId、顺序和可见列。

标量 global aggregate 使用 left outer join-back：无 inner row 时 `MAX/MIN` 得到 NULL；`COUNT` 的
NULL padding 继续由 `SubqueryToApply` 已有的 `NVL(resultForEmptyInput())` contract 恢复为 0，没有另写一套
COUNT 特例。

## 6. 2025 nested 框架雏形

遇到 nested Apply 时不把 parent D 直接推过整个 Apply：

1. 先用 parent state 改写 nested-left，使其输出 parent binding；
2. 应用只依赖 parent binding 与 nested-left 的安全 conjunct；
3. 从过滤后的 nested-left 生成 child Domain；
4. child Domain key 是 child local outer refs 与所需 ancestor refs 的联合表示；
5. 改写 child RHS，并在 child Apply boundary join-back；
6. child 结果继续透传 parent replacement，供外层 Aggregate/Filter 使用。

验收 SQL 的 child Domain 为真实可达的 `(outer.k1, inner.k2)`，而不是
`DISTINCT outer.k1 × DISTINCT inner.k2`。FE 测试同时断言 compound group key、双 NULL-safe identity，且
最终计划中没有 CROSS_JOIN 形式的独立域乘积。

## 7. 实现亮点

1. **新旧路径互斥且可灰度。** 开关默认关闭；candidate 不完整时返回原 Apply，由 legacy 路径继续处理。
2. **无新物理算子。** Domain 全部 lower 为现有 Aggregate/Join，直接复用 join reorder、distribution、
   runtime filter 和 MPP 执行。
3. **NULL 与普通 SQL equality 分离。** 原 predicate 不改语义；仅 Domain identity 使用
   `NullSafeEqual`，NULL outer binding 不会在 join-back 丢失。
4. **保留 bag。** 只对 Domain key 去重，原外表 duplicate 由最终 join-back 恢复。
5. **复用 Doris empty-input contract。** COUNT=0 沿用 aggregate function metadata/NVL 处理，避免
   `LEFT JOIN + COUNT(*)` 的经典 count bug。
6. **fresh identity。** Domain source deep copy 后按 ExprId map 找新 key，不复用同一 relation subtree。
7. **2025-compatible state。** 第一阶段 operator 少，但 parent/access/replacement 数据流已经按 holistic
   模型组织，后续增加 handler 不需要再推翻单层 rule。

## 8. 测试与验证

### 8.1 FE UT

文件：
`fe/fe-core/src/test/java/org/apache/doris/nereids/rules/rewrite/HolisticApplyEliminatorTest.java`

覆盖：

- MAX/MIN/COUNT；
- equality + non-equality + OR；
- inner equal join；
- no Apply、NULL-safe join-back、Domain Aggregate；
- nested ancestor reference、compound reachable Domain、无独立 Domain CROSS_JOIN。

命令：

```bash
./run-fe-ut.sh --run org.apache.doris.nereids.rules.rewrite.HolisticApplyEliminatorTest
```

### 8.2 Regression

文件：

- `regression-test/suites/query_p0/subquery/holistic_unnesting.groovy`
- `regression-test/data/query_p0/subquery/holistic_unnesting.out`

数据包含 duplicate binding、NULL outer key、inner NULL value 和 empty binding。结果验证 COUNT empty=0、
MAX/MIN empty=NULL，以及 nested query 的结果。

验证命令：

```bash
./run-regression-test.sh --run -d query_p0/subquery -s holistic_unnesting \
  -c 'jdbc:mysql://127.0.0.1:24030/?useLocalSessionState=true&allowLoadLocalInfile=true&zeroDateTimeBehavior=round' \
  -ha 127.0.0.1:23030
```

本地验证使用独立当前分支集群：FE query/http `24030/23030`，BE heartbeat/http `24050/23040`。

### 8.3 Build

```bash
./build.sh --fe --clean -j48
./build.sh --fe -j48
```

FE 主代码、测试代码与 Checkstyle 均通过。

## 9. 后续演进

建议按以下顺序扩展：

1. 用 forced internal CTE 或 physical delim fan-out 替代 duplicate-tree，避免重复昂贵 outer source；
2. 增加 EXISTS/IN 的 Domain handler，但继续复用 Doris Mark/NULL-aware 语义；
3. 补 grouped aggregate/Max1，再逐项增加 Join/SetOp/Window/TopN handler；
4. 为 CTE DAG 建 context-aware access index；
5. 最后增加 Domain/substitution 完整 Apply-boundary候选和 MPP costing，再评估默认开启。

在这些能力具备前，实验开关不应默认开启，也不应扩大 analyzer 白名单。
