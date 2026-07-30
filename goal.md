# Doris Indexed Algebra 渐进式实现计划

## 1. 总目标

在 Nereids 中实现一套可复用的 Indexed Algebra 基础设施，使优化器能够显式表示并高效查询：

- SQL Query Block 之间的词法绑定和自由变量；
- `ExprId` 对应值的生产算子、语义使用点和 CTE 重命名关系；
- 逻辑计划中的 ancestor、root、LCA 和路径属性；
- `LogicalApply` 右子树的 direct dependency 与 inherited dependency；
- 谓词放置、Join Graph、CTE、nullability 等非子查询客户端需要的数据流信息。

第一阶段目标不是立即实现动态 link-cut tree，也不是一次性替换所有 `correlatedSlots`、
`getOutputSet()` 和现有去相关规则。项目应拆成可以独立合并、独立测试、独立回退的小任务，
先证明语义正确和客户端价值，再决定是否引入动态索引。

算法背景和 Doris 当前能力分析见
[Holistic Subquery Unnesting 设计](docs/holistic-subquery-unnesting-2025-design.md)。
Indexed Algebra 论文为
[Asymptotically Better Query Optimization Using Indexed Algebra（PVLDB 2023）](https://db.in.tum.de/~fent/papers/p2755-fent.pdf)。

## 2. 核心模型



### 2.1 分层

```text
SQL name resolution
        |
        v
Binding Graph
  QueryBlock / Scope / BindingUse / FreeVariable
        |
        v
Algebra Dataflow
  IU definition / semantic use / CTE remapping
        |
        v
Plan Path Index
  root / ancestor / LCA / path summary
        |
        v
Optimizer Clients
  subquery / predicate placement / CTE / DPHyper / nullability
```

Binder 只负责回答“这个引用绑定到哪个 `ExprId`”。Indexed Algebra 负责回答“这个
`ExprId` 在哪里产生、在哪里使用、生产者到使用点之间经过了什么”。

### 2.2 Direct 与 inherited dependency

以下查询是第一个核心验收用例：

```sql
SELECT *
FROM t1
WHERE EXISTS (
    SELECT 1
    FROM t2
    WHERE EXISTS (
        SELECT 1
        FROM t3
        WHERE t3.k = t1.k
    )
);
```

预期依赖关系：

```text
inner Apply:
  rightFreeIUs = {t1.k}
  directIUs = {}
  inheritedIUs = {t1.k}

outer Apply:
  rightFreeIUs = {t1.k}
  directIUs = {t1.k}
  inheritedIUs = {}
```

当前单一 `LogicalApply.correlationSlot` 无法表达这个区别。后续所有任务都应维持以下不变量：

1. `directIUs` 的 source 必须位于当前 Apply 的 left subtree；
2. `inheritedIUs` 的 source 不在当前 Apply 的 left/right subtree 中；
3. 最终去相关结果不能残留 free IU；
4. Binder 的词法层级和 Plan 的拓扑层级不能混为一谈。



## 3. 实施原则

1. 原则上一个任务对应一个小 PR；只有纯机械且无法独立测试的任务才合并。
2. 优先增加只读数据结构和 shadow verification，再切换生产行为。
3. 新旧算法同时存在时，由 session variable 控制，并保留快速回退路径。
4. 静态索引必须属于某个 Plan snapshot，不能作为 statement 级全局拓扑缓存。
5. 第一版只运行在确定的 Logical Plan 上，不直接索引 Cascades Memo。
6. CTE 建模为 algebra forest 加显式 IU remapping，不把 producer/consumer 当作普通树边。
7. 路径屏障首先保持当前报错和改写边界不变，后续任务再逐项解除限制。
8. 性能测试不在 CI 中断言墙钟时间；CI 验证正确性，固定 harness 用于趋势和分配量比较。
9. 新增回归结果必须由回归测试脚本生成，不能手写 `.out`。



## 4. 依赖关系与阶段

```text
G0 Baseline and observability
        |
        v
G1 Query-Block-aware Binder
        |
        v
G2 Correlation IR compatibility layer
        |
        v
G3 Pass-local static Algebra Index
        |
        +--------------------+
        |                    |
        v                    v
G4 Subquery client      G5 Predicate placement client
        |                    |
        +----------+---------+
                   v
          G6 CTE / DPHyper / Nullability
                   |
                   v
          G7 Dynamic link-cut index
```

`G0` 至 `G4` 是支持祖父层引用和完整 Indexed Algebra 去相关的必经路径。`G5` 用于证明这套
基础设施不只服务子查询。`G6` 各客户端可以分别立项。只有静态索引重建被证明是热点后才进入
`G7`。

### 4.1 任务状态总览

以下 checklist 是本文唯一的任务完成状态入口。任务只有满足第 14 节 Definition of Done
后才能勾选。

- [ ] G0.1 固化当前相关子查询行为
- [ ] G0.2 增加优化器观测指标
- [ ] G1.1 引入 `QueryBlockId`
- [ ] G1.2 为 `Scope` 增加种类和 owner
- [ ] G1.3 提取 `SlotResolution`
- [ ] G1.4 支持任意层 lexical lookup，但保留执行 gate
- [ ] G1.5 建立 `BindingGraph` 和自由变量闭包
- [ ] G2.1 新增 `CorrelationDescriptor`
- [ ] G2.2 支持 descriptor deep copy
- [ ] G2.3 扩展 `LogicalApply` 的依赖模型
- [ ] G2.4 替换对 `Scope.correlatedSlots` 的特殊读取
- [ ] G3.1 定义 IU definition/use inventory
- [ ] G3.2 建立静态 forest 和 LCA
- [ ] G3.3 建立 IU source/use 索引
- [ ] G3.4 增加静态 path summary
- [ ] G3.5 建模 CTE forest 和 remapping
- [ ] G3.6 增加 shadow verification 开关
- [ ] G4.1 用 Free-IU 替换子查询相关性整树扫描
- [ ] G4.2 对完整 Apply tree 派生 direct/inherited dependency
- [ ] G4.3 增加 holistic subquery custom pass 的 shadow 模式
- [ ] G4.4 接管最小的多层 `EXISTS`
- [ ] G4.5 扩展到 `IN/NOT IN` 和 scalar
- [ ] G4.6 逐个解除 operator barrier
- [ ] G5.1 只读计算谓词目标位置
- [ ] G5.2 启用简单 Inner Join 谓词放置
- [ ] G5.3 逐项支持复杂 barrier
- [ ] G6.1 CTE 列裁剪和谓词映射
- [ ] G6.2 DPHyper Join Graph
- [ ] G6.3 Nullability shadow validator
- [ ] G7.1 动态化准入检查
- [ ] G7.2 独立实现并测试 link-cut tree
- [ ] G7.3 增加 `PlanRewriteTransaction`
- [ ] G7.4 只迁移一个 holistic pass

---



## 5. G0：基线与可观测性



### G0.1 固化当前相关子查询行为

**目标**

记录当前支持和明确拒绝的多层相关场景，避免 Binder 改造无意改变已有错误类型或单层语义。

**改动范围**

- 扩展 `AnalyzeWhereSubqueryTest` 或 `AnalyzeSubQueryTest`；
- 在 `query_p0/subquery` 增加独立的 multi-level suite；
- 当前尚不支持的 SQL 继续使用 `test { sql; exception }`。

**最小用例**

- 真正由最内层 predicate 引用祖父列的 `EXISTS`；
- 内层同时引用父层列和祖父列；
- 中间 Query Block 存在同名列；
- qualified grandparent reference；
- `IN` compare expression 中的传递相关；
- scalar、aggregate、window、limit、set operation 的现有限制。

**验证**

- 使用 `run-fe-ut.sh` 运行相关 Analyzer 单测；
- 使用 `run-regression-test.sh -d query_p0/subquery -s <suite>` 运行新 suite；
- 本任务不改变任何当前成功/失败结果。

**完成标准**

- 当前行为全部有测试覆盖；
- 每个失败用例能区分“名字找不到”“显式语义限制”“legacy unnest 不支持”。



### G0.2 增加优化器观测指标

**目标**

在实现索引前获得可比较基线。

**建议指标**

```text
plan node count / depth
IU count / semantic use count
getOutputSet 调用次数
已物化 outputSet 的总元素数
subquery analyze / SubqueryToApply / unnest topic 时间
InferPredicates / HyperGraph / AdjustNullable 时间
```

**约束**

- 指标只在 trace、debug 或 benchmark 模式开启；
- 默认路径不增加逐表达式日志；
- 不在 CI 中使用固定毫秒数断言。

**完成标准**

- 能对同一 SQL 输出前后可比较的结构和阶段指标；
- 能生成 10/100/300 层合成 Join 或嵌套 Query Block 的基线。

---



## 6. G1：Query-Block-aware Binder



### G1.1 引入 `QueryBlockId`，不改变绑定行为

**目标**

为每个真正的 SQL Query Block 分配稳定身份。

**改动范围**

- 新增 `QueryBlockId`；
- `StatementContext` 增加 generator；
- `CascadesContext` 增加 `currentQueryBlockId`；
- 新增明确的 `newSubqueryContext(...)`。

**边界**

- 普通 subtree context 继承 QueryBlock；
- lambda 不创建 QueryBlock；
- CTE 使用独立分析上下文，但不能错误继承普通相关作用域；
- 只有 `SubqueryExpr` 创建带 lexical parent 的新 QueryBlock。

**验证**

- Query Block ID 生成和继承单元测试；
- 现有 Analyzer 全量相关单测结果不变。



### G1.2 为 `Scope` 增加种类和 owner

**目标**

区分普通局部 Scope、lambda Scope 和子查询边界，消除“所有 outerScope 都表示 correlation”
的隐含假设。

**建议接口**

```java
enum ScopeKind {
    LOCAL,
    LAMBDA,
    SUBQUERY_BOUNDARY,
    STANDALONE
}

Scope.local(...);
Scope.lambda(...);
Scope.subqueryBoundary(...);
Scope.standalone(...);
```

**改动范围**

- `Scope` 增加 `ScopeId`、`ScopeKind`、`ownerQueryBlockId`；
- 替换关键 `new Scope(...)` 调用；
- 暂时保留 `getCorrelatedSlots()`。

**验证**

- lambda 捕获本 Query Block 列时不产生 query correlation；
- 现有单层相关子查询行为不变；
- 所有 Scope factory 有直接单测或被 Analyzer 测试覆盖。



### G1.3 提取 `SlotResolution`

**目标**

让名字解析同时返回声明 Scope 和 provider Query Block。

**建议结构**

```java
record SlotResolution(
        List<? extends Expression> candidates,
        Scope declarationScope,
        QueryBlockId consumerQueryBlock,
        QueryBlockId providerQueryBlock,
        int lexicalDepth) {}
```

**解析规则**

1. 从当前 Scope 向 lexical parent 搜索；
2. 第一个非空候选集立即停止；
3. 最近 Scope ambiguous 时直接报错；
4. 不允许因为祖先 Scope 唯一而绕过中间层 ambiguous；
5. qualified name 继续复用现有 `bindSlotByScope()` 规则。

**验证**

- 当前层命中；
- immediate parent 命中；
- nearest-scope shadowing；
- nearest-scope ambiguity；
- qualified lookup；
- nested struct/variant field 的底层 provider Slot。



### G1.4 支持任意层 lexical lookup，但保留执行 gate

**目标**

让 Binder 能正确找到祖父层及更外层 Slot。

**改动范围**

- `ExpressionAnalyzer.visitUnboundSlot()` 使用 `SlotResolution`；
- 允许沿 Scope 链查找；
- 当 `lexicalDepth > 1` 且 holistic unnest 尚未启用时，返回明确的功能 gate 错误，而不是
`Unknown column`。

**验证**

- 2/3/8 层 Query Block 的 `ExprId` 绑定正确；
- 中间层 shadowing 正确；
- 同一个祖父 Slot 多次引用时，绑定 occurrence 不丢失；
- 默认路径仍不把多层相关 SQL 送入不兼容的 legacy unnest。



### G1.5 建立 `BindingGraph` 和自由变量闭包

**目标**

记录具体 BindingUse，并从子 Query Block 向父 Query Block 传播自由变量需求。

**建议结构**

```java
record BindingUse(
        ExprId targetExprId,
        Slot targetSlot,
        QueryBlockId consumer,
        QueryBlockId provider,
        int lexicalDepth) {}

interface BindingGraph {
    void registerUse(BindingUse use);
    CorrelationDescriptor correlationOf(QueryBlockId block);
}
```

**预期闭包**

```text
Q2 uses Q1.a and Q0.b:
  Q2 requiredOuter = {Q1.a, Q0.b}
  Q1 requiredOuter = {Q0.b}
  Q0 requiredOuter = {}
```

**验证**

- direct use 和 transitive capture 分开；
- 多次 occurrence 保留，但 legacy Slot 投影去重；
- lambda/local Scope 不出现在 Query Block 闭包中；
- Query Block 分析顺序不影响最终闭包。

---



## 7. G2：Correlation IR 兼容层



### G2.1 新增 `CorrelationDescriptor`

**目标**

让 `SubqueryExpr` 不再以裸 `List<Slot>` 作为相关性的唯一真相来源。

**建议结构**

```java
record CorrelationDescriptor(
        QueryBlockId queryBlockId,
        ImmutableSet<Slot> requiredOuterSlots,
        ImmutableList<BindingUse> directUses,
        ImmutableSet<Slot> transitiveCaptures,
        int maxLexicalDepth) {}
```

**改动范围**

- `SubqueryExpr`、`Exists`、`InSubquery`、`ScalarSubquery`；
- 保留接受 `List<Slot>` 的旧构造器；
- `getCorrelateSlots()` 暂时委托给 `requiredOuterSlots`。

**验证**

- 现有 Subquery expression equals/hash/deep copy 测试不退化；
- 单层相关的 legacy Slot 列表完全一致；
- 多层场景能看到 direct use 和 transitive capture。



### G2.2 支持 descriptor deep copy

**目标**

确保 Plan/Expression deep copy 时 descriptor 中所有 Slot/ExprId 同步 remap。

**改动范围**

- `ExpressionDeepCopier`；
- `LogicalPlanDeepCopier`；
- CTE/MV 可能使用的 plan copy 路径。

**验证**

- copy 前后 Query Block 关系不变；
- Slot/ExprId 全部指向 copy 后对象；
- 原计划和 copy 之间不存在意外 Slot identity 混用。



### G2.3 扩展 `LogicalApply` 的依赖模型

**目标**

区分 syntactic subquery metadata、direct correlation 和 inherited dependency。

**建议结构**

```java
record DependentJoinInfo(
        ImmutableSet<ExprId> rightFreeIUs,
        ImmutableSet<ExprId> directIUs,
        ImmutableSet<ExprId> inheritedIUs,
        ImmutableMap<ExpressionUseId, ObjectId> attachmentApply) {}
```

**兼容接口**

```java
getCorrelationSlot(); // 只投影 directIUs
isCorrelated();       // 只判断 directIUs
hasInheritedDependency();
```

**验证**

- 单层相关 Apply 的行为和现有实现一致；
- 祖父引用在 inner Apply 上为 inherited，在 outer Apply 上为 direct；
- inherited IU 不再被错误要求出现在 inner Apply.left output 中。



### G2.4 替换对 `Scope.correlatedSlots` 的特殊读取

**目标**

逐个迁移仍然直接读取可变 `correlatedSlots` 的分析逻辑。

**首批位置**

- `ExpressionAnalyzer.visitInSubquery()`；
- correlated `UNNEST` 检查；
- `FillUpMissingSlots`；
- aggregate/project 相关列排除逻辑。

**方法**

- 先改成读取 `CorrelationDescriptor`；
- 第一版保持当前允许/拒绝行为；
- 每迁移一个位置都增加对应测试。

**完成标准**

- `Scope.correlatedSlots` 只作为 legacy shadow 数据，不再产生新的语义决策。

---



## 8. G3：Pass-local 静态 Algebra Index



### G3.1 定义 IU definition/use inventory

**目标**

显式区分值定义、语义使用和纯 metadata，避免直接把 `Plan.getExpressions()` 当作完整数据流。

**建议接口**

```java
interface PlanExpressionInventory {
    List<IuDefinition> definitions(Plan plan);
    List<OwnedExpression> semanticUses(Plan plan);
    List<IuRemapping> remappings(Plan plan);
    List<AlgebraChildEdge> dataflowChildren(Plan plan);
}
```

**首批覆盖**

- Scan、OneRowRelation；
- Project、Aggregate、Window、Generate；
- Filter、Sort、TopN、Limit；
- Join、Apply；
- SetOperation；
- CTE producer/consumer/anchor。

**重要约束**

- `LogicalApply.correlationSlot` 是 metadata，不是 semantic use；
- `compareExpr/typeCoercionExpr/correlationFilter` 是 semantic use；
- Filter、Sort、Limit 和普通 Join 的透传 Slot 不产生新 IU；
- 不能在每层扫描完整 output list 来推导 source，否则会重新引入二次复杂度。

**验证**

- 每种 Plan 的 definitions 和 uses 精确单测；
- Alias 产生新 IU，普通 Slot 透传不产生新 IU；
- 同一表达式重复使用一个 Slot 时保留多个 occurrence；
- 新增受支持 Logical Plan 类型时，有测试提醒 inventory 未覆盖。



### G3.2 建立静态 forest 和 LCA

**目标**

先提供可靠的 root、ancestor 和 LCA，不实现动态更新。

**建议结构**

```java
Map<Integer, Integer> objectIdToOrdinal;
int[] parent;
int[] depth;
int[][] up;
```

使用 binary lifting：

- 构建 `O(n log n)`；
- ancestor/LCA 查询 `O(log n)`；
- index 使用 `AlgebraSnapshotId + ObjectId` 标识节点。

**验证**

- chain、balanced tree、forest；
- root、ancestor、LCA；
- 不同 snapshot 中相同 ObjectId 不混用；
- 当前 snapshot 中非 GroupPlan ObjectId 唯一。



### G3.3 建立 IU source/use 索引

**目标**

提供：

```java
sourceOf(ExprId);
usesOf(ExprId);
freeIUs(subtree);
isProducedInside(iu, subtree);
```

**ExpressionUseId**

```java
record ExpressionUseId(
        int ownerObjectId,
        int expressionOrdinal,
        int[] expressionPath) {}
```

**验证**

- Scan、Alias、Aggregate output 的 source；
- pass-through Slot 的 source 不变化；
- use owner 和 expression path 正确；
- 未在 snapshot 内生产的 IU 被识别为 free IU；
- direct source 与递归 base lineage 分开。



### G3.4 增加静态 path summary

**目标**

支持从一个节点到祖先节点的路径属性查询。

**首批 barrier**

```text
OUTER_JOIN_NULL_EXTENDING
AGGREGATE
WINDOW
LIMIT_OR_TOPN
SET_OPERATION
VOLATILE_OR_SIDE_EFFECT
RECURSIVE_CTE
```

**约束**

- Outer Join null-extension 是 child-to-parent edge 属性，必须包含 child ordinal；
- 第一版只返回 barrier，不改变当前语义决策；
- 对方向敏感的属性不能使用无方向的简单 bitset 合并。

**验证**

- Left/Right/Full Outer Join 每个 child edge；
- 同一 IU 在 Outer Join 前后使用；
- 多 barrier 路径；
- 返回第一个、最高或最低 barrier 的确定语义。



### G3.5 建模 CTE forest 和 remapping

**目标**

让 CTE producer 和每个 consumer 位于独立 algebra tree 中，通过 IU rename 显式关联。

**规则**

- `LogicalCTEConsumer` 是 consumer IU 的本地 source；
- consumer IU 映射到 producer IU；
- `LogicalCTEAnchor` 的 producer edge 不是普通数据流 parent edge；
- recursive CTE 第一版作为 hard barrier。

**验证**

- 单 consumer、多 consumer；
- producer/consumer ExprId 不混淆；
- consumer 到 producer 双向 remap；
- LCA 不跨越伪造的 CTE Anchor 数据流边。



### G3.6 增加 shadow verification 开关

**建议开关**

```text
enable_indexed_algebra
verify_indexed_algebra
```

`verify` 模式同时执行 legacy 和 index 计算，只比较结果，不改变 Plan。

**首批比较项**

- free IU 集合；
- 单层 correlated slot；
- Apply direct dependency；
- 简单谓词可用位置；
- nullable shadow answer。

**完成标准**

- 默认关闭时没有行为变化；
- verify 失败输出 Query ID、Plan snapshot、IU、legacy answer 和 indexed answer；
- verify 不允许静默回退或吞掉不一致。

---



## 9. G4：Indexed Algebra 子查询客户端



### G4.1 用 Free-IU 替换子查询相关性整树扫描

**目标**

在子查询刚分析完成时，用局部 source/use 索引找出 free IU，替换
`SubExprAnalyzer` 中基于 correlated slot 的 BFS/逐叶扫描。

**第一阶段行为**

- 仍保持现有 Aggregate、Window、Limit、Join、SetOp 限制；
- 只替换“相关使用点在哪里”和“路径上有什么”的分析方式；
- 与旧 validator 做 shadow comparison。

**验证**

- 一个 Query Block 有多个相关使用点；
- direct use 和 descendant subquery transitive use；
- barrier 报错文本和类型保持兼容；
- 分析复杂度随 Plan 深度呈预期趋势。



### G4.2 对完整 Apply tree 派生 direct/inherited dependency

**算法**

```text
rightFree = freeIUs(apply.right)

direct =
  rightFree 中 source 位于 apply.left subtree 的 IU

inherited =
  rightFree - direct
```

**目标**

由索引答案派生兼容 `correlationSlot`，不再由语法最近层级直接决定 Apply dependency。

**验证**

- immediate correlation；
- grandparent-only correlation；
- inner 同时引用 parent 和 grandparent；
- nested Apply chain；
- source 位于 Join 左/右 child 的情况。



### G4.3 增加 holistic subquery custom pass 的 shadow 模式

**建议接入点**

在 RBO 的 Subquery unnesting topic 中：

```text
EliminateUselessPlanUnderApply / MergeProjectable
        |
IndexedAlgebraSubqueryRewriter (shadow)
        |
legacy CorrelateApplyToUnCorrelateApply
        |
ApplyToJoin
```

**目标**

新 pass 只计算 attachment Apply、访问点和 barrier，与 legacy 结果比较。

**完成标准**

- 所有现有单层 subquery suite 的 indexed/legacy 决策一致；
- 多层新用例能产生明确的 direct/inherited/attachment 结果；
- pass-local index 不会被后续 rule 复用为陈旧 snapshot。



### G4.4 接管最小的多层 `EXISTS`

**范围**

只支持：

- 两层或三层 `EXISTS/NOT EXISTS`；
- 相关 predicate 位于 Filter；
- Inner/Cross Join 路径；
- 无 Aggregate、Window、Limit、SetOp、Outer Join；
- 等值和简单非等值 predicate。

**目标**

完成第一个真正执行成功的祖父层相关查询。

**验证**

- 结果正确；
- rewrite 后没有 `LogicalApply`；
- rewrite 后每个 semantic IU use 都有合法 source；
- inner inherited IU 不被误连到 inner left；
- `NOT EXISTS`、NULL 和空输入行为正确。



### G4.5 扩展到 `IN/NOT IN` 和 scalar

**拆分建议**

1. `IN`；
2. `NOT IN` 三值逻辑；
3. scalar 非聚合；
4. scalar aggregate；
5. scalar 多行 `assert_true`；
6. 空输入 `COUNT` 的 `NVL` 语义。

每一项单独增加 FE UT 和回归测试，不能作为一个大 PR 一次引入。

### G4.6 逐个解除 operator barrier

建议顺序：

1. Aggregate；
2. Window；
3. `ORDER BY/LIMIT/OFFSET`；
4. SetOperation；
5. Left/Right Outer Join；
6. Full Outer Join；
7. shared CTE；
8. recursive CTE。

每解除一个 barrier，需要：

- 明确等价变换；
- 保留 bag/nullability/scalar cardinality 不变量；
- 独立正反例；
- 保留不满足前置条件时的明确错误或 fallback。

---



## 10. G5：谓词放置客户端



### G5.1 只读计算谓词目标位置

**范围**

- Filter；
- 确定性 Project Alias；
- Sort；
- Inner/Cross Join。

**算法**

1. 收集 predicate input IU；
2. 查询每个 IU 的 direct source；
3. 对 source 求 LCA；
4. 查询当前位置到 LCA 的 barrier；
5. 返回最深合法目标，不修改 Plan。

**验证**

- 与 `PushDownFilterThroughJoin`、`InferPredicates` 的 legacy target 做 shadow comparison；
- 单表、双表、多表 predicate；
- 16/32 层 left-deep Join；
- Alias chain、constant Alias、volatile Alias。



### G5.2 启用简单 Inner Join 谓词放置

**目标**

用一次 holistic placement 替代支持范围内逐层 `outputSet.containsAll()` 判断。

**约束**

- 谓词推导仍使用现有逻辑，只替换 placement；
- 遇到不支持 barrier 回退到 legacy rule；
- 一个 pass 内先计算全部 placement，再一次性重建 Plan，不能查询已陈旧的 index。

**验证**

- legacy/indexed 结果集和 residual predicates 一致；
- explain shape 等价或更优；
- TPC-H/TPC-DS 无计划质量回退。



### G5.3 逐项支持复杂 barrier

建议分别处理：

- Project Alias 展开；
- Outer Join preserved/null-supplying side；
- Aggregate/Having；
- Window；
- SetOperation；
- volatile 和 side-effect expression。

每种 barrier 独立 PR，不与谓词推导算法重构捆绑。

---



## 11. G6：其他独立客户端



### G6.1 CTE 列裁剪和谓词映射

**目标**

复用 `CteRemapping` 提供：

```java
rewriteAcrossCteBoundary(expression, consumerId);
requiredProducerIUs(cteId);
usesByConsumer(cteId, producerIu);
```

**验证**

- 1/2/8/32 consumers；
- 部分 consumer 没有 filter 时禁止错误下推；
- producer required columns 为所有 consumer 的并集；
- volatile predicate 不被复制；
- nested CTE。



### G6.2 DPHyper Join Graph

**目标**

逐步替换 HyperGraph 内部重复的 Slot-to-node 和 Alias lineage 构建。

**边界**

- 对本次选择的代表逻辑树单独建立 snapshot；
- 不对整个 Memo 建 LCA；
- 首先只比较 edge endpoints，不改变 Join enumeration。

**验证**

- chain/star/tree/clique；
- 8/16/32/64 relations；
- Alias 和多表表达式；
- 每条 edge 的 left/right required node 精确一致。



### G6.3 Nullability shadow validator

**目标**

实现：

```text
nullableAt(iu, use) =
  nullableAtSource(iu)
  OR source-to-use path crosses a null-extending edge
```

**第一阶段**

- 不删除 `AdjustNullable`；
- 只逐 use 比较路径答案和现有 `SlotReference.nullable`；
- SetOperation、scalar Apply、CTE 等特殊来源单独建模。

**验证**

- Left/Right/Full Outer Join；
- 同一 IU 在 Outer Join 前后使用；
- 多层 Outer Join；
- CTE remap；
- scalar subquery 空集；
- `COUNT/SUM` 差异。

---



## 12. G7：动态 link-cut index



### G7.1 动态化准入检查

只有同时满足以下条件才开始实现：

- 至少有两个生产客户端使用静态 Algebra Index；
- correctness shadow verification 在目标测试集上无差异；
- 静态重建是复杂查询规划中的可重复热点；
- 重建成本高于动态维护预期成本，而不是客户端自身改写占主导；
- Plan snapshot 和 rewrite transaction 边界已经明确。

未满足准入条件时，继续使用 pass-local 静态索引。

### G7.2 独立实现并测试 link-cut tree

**目标**

先实现通用动态树，不接入 Plan rewrite。

**操作**

```text
link
cut
expose
findRoot
findLCA
path aggregate
```

**验证**

- 与朴素树实现做 randomized differential testing；
- 随机 link/cut 后 root/LCA/path summary 一致；
- 检测重复 parent、cycle 和非法 cut；
- 验证摊还趋势，但 CI 不断言固定耗时。



### G7.3 增加 `PlanRewriteTransaction`

**建议协议**

```text
begin(root, snapshotEpoch)
cut old edges
remove disappeared definitions/uses
refresh same-ObjectId operator content
create new nodes
link new edges
validate
commit
```

未接入事务的 rewrite job 必须将 index 标记为 invalid，下一次查询前重建。

**验证**

- replace leaf/subtree；
- move subtree；
- same ObjectId expression refresh；
- new ObjectId insertion；
- rule 失败和事务回滚；
- snapshot epoch 不匹配时拒绝查询。



### G7.4 只迁移一个 holistic pass

先让 `IndexedAlgebraSubqueryRewriter` 使用动态索引。确认收益和一致性后，再考虑谓词放置。
第一阶段不修改所有 `PlanTreeRewriteJob` 和 Memo exploration。

---



## 13. 测试矩阵



### 13.1 Binder


| 场景                   | 必须验证                            |
| -------------------- | ------------------------------- |
| 当前层命中                | 不搜索 outer                       |
| immediate parent     | provider/consumer QueryBlock 正确 |
| grandparent / 8 层祖先  | ExprId 和 lexical depth 正确       |
| nearest shadowing    | 不绑定到祖父列                         |
| nearest ambiguous    | 直接报错，不回退                        |
| qualified reference  | 绑定指定 relation                   |
| parent + grandparent | direct/transitive closure 正确    |
| repeated reference   | occurrence 保留、Slot 投影去重         |
| lambda capture       | 不产生 Query Block correlation     |
| struct/variant field | 记录底层 provider IU                |




### 13.2 Algebra Index


| 场景                     | 必须验证                       |
| ---------------------- | -------------------------- |
| Scan/Project/Aggregate | IU source 正确               |
| pass-through           | 不产生新 IU                    |
| repeated Slot use      | ExpressionUseId 不冲突        |
| chain/balanced tree    | root/ancestor/LCA 正确       |
| free use               | 子树外 source 正确识别            |
| Outer Join edge        | path summary 有方向           |
| CTE                    | forest/remap 正确            |
| snapshot               | 相同 ObjectId 不跨 snapshot 污染 |




### 13.3 Subquery


| 场景                       | 必须验证                                    |
| ------------------------ | --------------------------------------- |
| nested EXISTS/NOT EXISTS | direct/inherited 和结果正确                  |
| IN/NOT IN                | NULL/UNKNOWN 正确                         |
| scalar                   | 空集、多行、nullable 正确                       |
| aggregate                | group key 和空输入正确                        |
| window/topN              | partition/row number 语义正确               |
| set operation            | bag/set 语义正确                            |
| outer/full join          | preserved side 和 null-safe reconnect 正确 |
| CTE                      | producer 只转换一次                          |




### 13.4 性能趋势

固定生成：

- Query Block 深度 `1/2/4/8/16`；
- Join 数 `8/16/32/64/100/300`；
- 每表列数 `1/16/64`；
- CTE consumer 数 `1/2/4/8/32`；
- Outer Join 深度 `1/4/8/16`。

每次记录：

```text
index build time / allocated entries
source/LCA/path query count
free-IU analysis time
subquery unnest time
predicate placement time
total FE planning time
```



## 14. 每个任务的 Definition of Done

一个任务只有在满足以下条件后才能标记完成：

- [ ] 改动不超出任务定义的语义范围；
- [ ] 新核心逻辑有 FE unit test；
- [ ] 用户可见 SQL 行为变化有 regression test；
- [ ] 预期错误使用 `test { sql; exception }`；
- [ ] 结果测试有稳定顺序；
- [ ] shadow 模式发现不一致时显式失败；
- [ ] 未声称运行没有实际运行的测试；
- [ ] FE checkstyle 通过；
- [ ] 相关设计和 feature flag 已记录；
- [ ] 实际修改以独立、符合仓库规范的 commit 提交。



## 15. 当前推荐执行顺序

近期只推进以下任务：

```text
G0.1  固化基线
G0.2  增加观测指标
G1.1  QueryBlockId
G1.2  ScopeKind
G1.3  SlotResolution
G1.4  任意层绑定 + gate
G1.5  BindingGraph closure
G2.1  CorrelationDescriptor
G2.2  Deep copy
G3.1  PlanExpressionInventory
G3.2  Static LCA
G3.3  IU source/use/free-IU
G3.6  Shadow verification
G4.1  子查询局部 Free-IU
G4.2  Apply direct/inherited
G4.3  Holistic pass shadow
G4.4  最小 nested EXISTS
```

完成 `G4.4` 后进行第一次架构复盘：

1. Binder/Apply 模型是否足以表达多层依赖；
2. 静态索引是否比现有遍历更简单、可维护；
3. 是否已经观察到可量化的规划时间或内存收益；
4. 谓词放置是否值得成为第二个生产客户端；
5. 是否继续扩展 operator 覆盖，还是先调整 IR。

在这次复盘前不启动全局 `getOutputSet()` 替换、Memo 索引或动态 link-cut tree。