# Doris 任意嵌套相关子查询改写：Holistic Unnesting 详细设计

> 文档状态：设计提案，不包含优化器行为变更。Doris 基线为
> `opt-research@7630bd1db9a`；修订日期为 2026-08-26。外部实现按固定提交研究，避免把变化中的
> 上游主干当作稳定接口。

本文以 `docs/doris-holistic-unnesting-architecture-survey.html` 为调研入口，重新核验了论文和实现源码：


| 类别      | 固定基线                                                               | 本文使用方式                                                         |
| ------- | ------------------------------------------------------------------ | -------------------------------------------------------------- |
| 2015 论文 | Neumann、Kemper，*Unnesting Arbitrary Queries*                       | 绑定域 `D`、dependent join 下推和 substitution 基线                     |
| 2025 论文 | Neumann，*Improving Unnesting of Complex Queries*                   | top-down/holistic 状态、嵌套 dependent join 和复杂算子规则                 |
| 形式化报告   | Neumann，*A Formalization of Top-Down Unnesting*（2024 v1 / 2026 v2） | bag 语义、属性隔离、`D` 覆盖与正确性条件                                       |
| Spark   | `eb327b68ab8571d425ed08d820ae8ccbafabf32f`                         | `DomainJoin` 中间态、递归 transfer function、COUNT 修正和测试矩阵            |
| DuckDB  | `6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d`                         | holistic parent state、binding replacement graph、Delim/CTE 共享执行 |
| Calcite | `2ceaf277b8eb16c6dbdce0e6129a8ad7c186a390`                         | `CorDef`、`UnnestedQuery` 输出映射和实验性 top-down visitor             |


“任意嵌套”在本文中的目标含义是：对任意有限相关深度，只要节点属于已声明支持的关系算子集合、
表达式满足按 binding 批处理或按 outer-row identity 隔离的合同，就能生成不含 free outer reference 和
dependent join 的等价普通计划。
它不等于第一批 PR 立即开放所有 SQL 形态，也不承诺在未证明安全时移动 volatile/有副作用表达式。

## 1. 行为契约与结论



### 1.1 终局行为契约

成功走 holistic 路径的查询必须同时满足：

1. 完整 lexical scope chain 中的父层、祖父层及更外层引用都有唯一 provider，不能用裸 `Slot`
  或名字猜测归属；
2. 对每个 outer binding，改写前后的 SQL bag multiplicity、NULL/三值逻辑、scalar 0/1/>1 行、
  aggregate 空输入、outer-join unmatched row、每 binding 的 window/TopN 语义一致；
3. 嵌套 dependent join 从外向内作为一个整体处理，不构造原计划不可达的独立域笛卡尔积；
4. holistic pass 原子成功或在改写前返回明确的 `DECLINED/UNSUPPORTED`，禁止把半改写计划交给旧规则；
5. 进入普通 Nereids rewrite、Memo 和 physical planning 前，不残留 `LogicalApply`、outer reference 或
  domain placeholder；binding/marker helper slot只在普通算子内部存活，不泄漏到用户 visible output；
6. 生成的最终节点仍是 Doris 已有的 join/aggregate/project/window/CTE 等普通节点，使 join reorder、
  distribution、runtime filter 和 MPP 执行继续生效。



### 1.2 结论

1. Doris 已能把常见的 `IN/NOT IN`、`EXISTS/NOT EXISTS` 和标量相关子查询改写成
  semi/anti/mark/outer join，且已经处理三值逻辑、标量子查询行数校验、空输入上的
   `COUNT` 等重要语义。
2. 当前实现是“分析期限制 + bottom-up 局部 pattern rule”，不是完整的关系代数去相关算法。
  它没有显式绑定域 `D`，没有跨多层 `Apply` 的统一状态，也不能表示祖父层及更外层引用。
3. 这意味着当前最主要的问题是**能力不完备和错失选择性下推**。不能直接说 Doris 已经遭遇
  2015 Domain-D 算法的笛卡尔积缺陷；论文中的典型深层案例在 Doris 中通常会更早被拒绝。
4. 不应继续以单条 `Apply -> Join` rule 修补。应增加 feature flag 控制的专用
  `HolisticApplyEliminator`：先建立完整作用域绑定和 dependent-join IR，再以
   `accessing + parent + domain + repr + scoped equivalence` 状态处理整棵计划。
5. 第一阶段采用 Spark 式 lazy `DomainSpec/DomainJoin`，在独立 lowering 阶段变成普通
  aggregate + join；状态和输出映射采用 Calcite/DuckDB 的显式模型；共享执行逐步演进到
   Doris forced internal CTE，最后才评估专用 physical delim fan-out。
6. 第一版在无法证明安全的停止点一律选择显式 `D`。以后可以比较 `D` 与 substitution，
  但两个局部子树并不等价：substitution 可能产生超集。只有包含最终 join-back 的完整计划
   才能作为同一 Memo group 的等价表达式。

目标不是照抄某个单机数据库实现，而是保留 2025 算法的语义不变量，并让生成的普通
aggregate/join/window 计划继续进入 Doris 现有的 Cascades、MPP 分布和执行实现。

### 1.3 非目标与边界

- 本文不设计 FE 逐外表行发起子查询的执行方式；可选 physical Apply 是独立后续能力。
- 第一批 PR 不同时解决所有 join/set-op/recursive CTE，也不删除 legacy 规则。
- 本文不把未复现的相邻 Nereids 问题纳入实现范围；每个阶段只解除其 handler 已覆盖的 analyzer
白名单。
- Materialize 仅作为算法对照。当前主仓库以 BSL 1.1 为主，不能向 Apache Doris 复制其实现。



## 2. 算法基线



### 2.1 2015：以绑定域 `D` 消除 dependent join

[Unnesting Arbitrary Queries（2015）](https://dl.gi.de/handle/20.500.12116/2418)
把相关子查询规范化为 dependent join：

```text
L depjoin R
```

其中 `R` 的表达式可以引用 `L` 的列。算法先尝试简单规则：

- 相关 filter 可以合并到 join condition；
- 相关 map/project 可以移动到 join 之上。

不能简单移动时，从左输入提取无重复绑定域：

```text
D = DISTINCT π_outerRefs(L)
L depjoin_p R
  ≡
L JOIN_{p AND L.keys <=> D.keys} (D depjoin R)
```

然后把 `D` 沿 `R` 向下推：

- filter/project：携带并重写绑定列；
- aggregate：把绑定列加入 group key；
- join：把绑定信息推到访问外部列的分支；
- 到达不再访问外部列的位置后，改成普通 join，或以局部等价列替换绑定列。

最终与 `L` 重连时必须使用 NULL-safe equality，因为绑定域中的 `NULL` 代表一个真实绑定值，
不能让普通 `=` 把 `NULL` 绑定丢掉。

这一转换的重要性质是：`D` 不必精确等于 `π_outerRefs(L)`；它可以是这个集合的无重复超集，
最终与 `L` 的 join 会过滤掉多余绑定。这一点在
[A Formalization of Top-Down Unnesting](https://arxiv.org/abs/2412.04294)
的定理 4.1 中被形式化。

### 2.2 2015 bottom-up 的根本缺陷

2015 算法单独、bottom-up 地处理每个 dependent join。对多层相关子查询，这会分别构造多个
独立的 `D`。如果内层同时依赖父层和祖先层，独立绑定域可能先形成并不存在于原查询中的
组合，再由 aggregate 对这些组合分组。

```text
真实可达绑定: (a1,b1), (a2,b2)
独立域乘积:   (a1,b1), (a1,b2), (a2,b1), (a2,b2)
```

深度增加时，中间绑定组合可能呈乘法增长。问题不只是 cost estimate 不准，而是转换提前制造了
原查询从未访问的绑定组合。

### 2.3 2025：一次处理整棵查询的 Holistic Unnesting

[Improving Unnesting of Complex Queries（BTW 2025）](https://dl.gi.de/handle/20.500.12116/45881)
把算法改成 top-down，并统一处理所有 dependent join。核心不是一组额外 pattern，而是三个部分：

1. **识别访问关系。** 对每个 dependent join，标出右子树中哪些 operator 访问它提供的外部列。
  论文以 producer、accessing operator 的最近公共祖先为基础，并使用
   [Indexed Algebra（PVLDB 2023）](https://www.vldb.org/pvldb/vol16/p3018-fent.pdf)
   高效回答树位置问题。
2. **尽量做 simple unnesting。** 线性路径上的 filter 可并入 join，map 可上移。所有访问都被
  消掉时，dependent join 直接变成普通 join，不必创建 `D`。
3. **需要** `D` **时 top-down 传播统一状态。** 遇到嵌套 dependent join，内层继承外层状态；
  先处理内层左侧绑定来源，再合并访问标记处理其右侧。不会让两个互不相关的 `D` 穿过
   dependent join 相乘。

论文中的全局和局部状态可概括为：

```java
record UnnestingInfo(
        DependentJoin join,
        Set<OuterReference> outerRefs,
        Plan domain,
        UnnestingInfo parent) {}

record UnnestingState(
        UnnestingInfo info,
        EqualClasses cclasses,
        Map<OuterReference, Slot> repr) {}
```

- `parent`：表示嵌套 dependent join 的祖先状态；
- `cclasses`：当前已知等价列的并查集；
- `repr`：每个外部引用在当前子树中的可用表示；
- `accessing`：一个 operator 仍访问哪些 dependent join；
- `D`：本次转换的无重复绑定域。

2025 正文引用的是形式化技术报告的 2024 v1；本文同时核验当前 arXiv v2（2026），其对 aggregate
只观察显式 referenced attributes 和 schema disjointness 的前提写得更明确。实现时必须保留：

- 使用 bag/multiset 语义证明，而不是默认 set 语义；
- `D` 必须无重复，并覆盖左输入的绑定投影；
- `D` 的属性命名空间必须与被推入的子树分离，冲突时显式 rename；
- group-by 增加 `D` 列后，aggregate 本身不能错误地消费这些辅助列；
- 遇到嵌套 dependent join 时，外层 `D` 只进入内层左侧；内层 join 再把表示传给右侧；
- 每个 operator 至多访问一次，最终不再残留 dependent join。



### 2.4 2015 规则基础与 2025 扩展的复杂 operator

2015/formal 的通用规则与 2025 论文新增的复杂结构合起来覆盖：

- left/right/full outer join：分别维护两侧表示，以 NULL-safe equality 重连；full join 的代表列
使用 `COALESCE(left_repr, right_repr)`；
- window：把绑定表示加入 `PARTITION BY`；
- `ORDER BY ... LIMIT/OFFSET`：改写为按绑定分区的 `ROW_NUMBER`，再过滤行号；
- set operation：2015/formal 要求把绑定列传入所有 child，并保持各自 bag/set 语义；
- scalar aggregate：为空输入的每个绑定保留一行，不能只做普通 inner join；
- shared CTE DAG：把 DAG 视为 producer tree 加 consumer proxy tree，producer 只转换一次；
- recursive CTE：把绑定列同时穿过 seed、recursive term 和 work table；
- full join condition 中同时引用 join 两侧的 singleton subquery（论文 §4.1 的明确前提）。



### 2.5 论文证明边界与 Doris 必须补齐的语义

形式化报告的 Theorem 4.1 对 inner dependent join 的分解给出严格前提：`D` 必须无重复，
`schema(D)` 等于 RHS 的 free-variable schema，覆盖左输入在这些列上的去重投影，并与 RHS 及下推路径
上每个中间 schema 隔离。报告另外证明了 project/filter/map、bag set-op、inner/cross、group、semi、anti、
left outer 和 nested dependent join 的步骤。

不能把“论文题目中的 arbitrary”解释成所有 SQL 语义都已经有证明：

- right/full outer、window、TopN、CTE 和 recursive CTE 主要来自 2025 正文的规则或伪码，不在该报告的
完整证明集合中；
- scalar subquery 的 canonical lowering 是 dependent single join，但 2015/2025 主算法没有展开
per-binding `Max1`/多行报错；
- 2015 的普通 group-by 下推没有单独处理 SQL global/static aggregate 的空输入一行，2025 才明确要求
domain outer/group join；
- mark join、`IN/NOT IN` 的 UNKNOWN、Doris aggregate 的各类 empty default 需要以 Doris 现有语义为准；
- 论文把表达式当作数学函数，未讨论 volatile、side effect 或异常求值次数。purity barrier 是 Doris
必须增加的工程约束，不应冒充论文结论。

因此，论文提供算法骨架和部分证明；Doris 的验收标准仍然是 SQL 语义矩阵、结构 invariant 与
differential/fuzz 三层证据。

## 3. 开源与公开源码实现核验及设计取舍



### 3.1 横向结论


| 维度         | Spark                                   | DuckDB                                                      | Calcite TopDown                                         | Doris 取舍                              |
| ---------- | --------------------------------------- | ----------------------------------------------------------- | ------------------------------------------------------- | ------------------------------------- |
| 主入口        | `DecorrelateInnerQuery`                 | `FlattenDependentJoins`                                     | `TopDownGeneralDecorrelator`                            | 独立 whole-plan pass                    |
| 遍历/状态      | 单个 inner plan 的带状态递归，2015 DomainJoin 路线 | 完整计划后 outer-to-inner，child flattener 继承 parent              | top-down visitor + sub-decorrelator                     | 从第一版保留 parent/access contract         |
| domain IR  | `DomainJoin` placeholder                | `LOGICAL_DELIM_JOIN` type + `LogicalDelimGet`/generated CTE | `DedupFreeVarsNode`                                     | lazy `DomainSpec` + 临时 placeholder    |
| 输出映射       | 递归内部 outer-ref map + join conditions    | binding vector + `BindingReplacementGraph`                  | `UnnestedQuery.corDefOutputs/oldToNewOutputs`           | `RewriteResult` 显式返回两类映射              |
| domain 执行  | outer 上 distinct aggregate，逻辑上可复制 outer | physical DelimJoin；当前默认也可降成共享 materialized CTE              | aggregate/value generator + 普通 join                     | 普通计划先行，forced internal CTE 共享演进       |
| NULL/empty | null-safe domain key；专门 COUNT-bug 分支    | delim key null-safe；count rewrite/null propagation          | `IS NOT DISTINCT FROM`；static aggregate value generator | 全局 invariant，不散落在 handler             |
| 复杂形态       | 覆盖广但仍有 analyzer/deep-correlation 限制     | 生产路径覆盖最广，含 CTE/recursive/full join 等大量分支                    | `@Experimental`，opt-in，unsupported 可保留旧 plan            | 行为 oracle 取 DuckDB，Java 状态表达取 Calcite |
| 成熟度        | 生产                                      | 生产                                                          | 默认关闭                                                    | feature flag + 分阶段白名单                 |


三者共同证明了一个工程事实：递归函数不能只返回 `Plan`。Project、Aggregate、Join、SetOp 和 CTE
都会改变列身份或位置，父节点必须拿到 correlation binding 的新表示和原输出映射。

### 3.2 Spark：适合第一条 vertical slice 的 DomainJoin 外壳

固定提交中的
`[DecorrelateInnerQuery.scala](https://github.com/apache/spark/blob/eb327b68ab8571d425ed08d820ae8ccbafabf32f/sql/catalyst/src/main/scala/org/apache/spark/sql/catalyst/optimizer/DecorrelateInnerQuery.scala)`
内部递归返回三元组：新 inner plan、与 outer 的 join conditions、outer reference 到 inner/domain attribute
的 replacement map；公开入口最终返回 plan 和 conditions。其关键 transfer function 是：

1. 子树不再相关且待绑定集合为空，原样停止；待绑定集合非空则插入 `DomainJoin`，为每个 key 分配
  fresh attribute，并生成 `fresh <=> OuterReference(original)`；
2. Filter 把 conjunct 分成 correlated/uncorrelated，只有可安全跨 aggregate 的等值项才作为
  substitution/join condition；其余表达式整体以 domain attribute 改写，因此 OR/non-equi 不必强拆；
3. Project/Aggregate 透传上层 join condition 仍需的属性，Aggregate 把 binding 加入 group/output；
4. global aggregate 在启用 `handleCountBug` 时增加 `alwaysTrue` marker、left `DomainJoin`，并按 aggregate
  的 zero-tuple result 修复右侧 null padding，而不是只硬编码 `COUNT(*)`；
5. Join 根据 join type 和两侧 correlation 决定单侧或双侧传播，双侧用 null-safe key 对齐；SetOp
  统一所有 child 的 domain 列位置和 attribute identity；
6. Limit/Offset 的相关分支把紧邻 Sort 改写成按 domain partition 的 `ROW_NUMBER`。不能把它扩大解释为
  任意 Sort：独立 Sort 仍走通用 unary 路径，Sort/Window 自身的 outer reference 仍受限制；
7. `[rewriteDomainJoins](https://github.com/apache/spark/blob/eb327b68ab8571d425ed08d820ae8ccbafabf32f/sql/catalyst/src/main/scala/org/apache/spark/sql/catalyst/optimizer/DecorrelateInnerQuery.scala#L387-L465)`
  最后以 outer 上的 distinct Aggregate 和普通 Join 替换 placeholder。

`DomainJoin` 只有 Inner 与 COUNT/static-aggregate 修正所需的 LeftOuter 两种临时类型，没有 physical
实现。普通 Inner placeholder通常只表示 domain attachment，non-equi residual 仍可能在上层 Filter，
不能把所有 lowering 画成“带原相关 predicate 的 Domain Join”。每个 placeholder 都从 outer plan
逻辑复制一棵 domain source，也没有跨 SetOp branch 的共享执行合同。

可移植部分是 placeholder/lowering 边界、递归返回值、operator 测试矩阵和 zero-tuple aggregate
求值接口。不宜照搬的是集中在一个超大对象中的分支、依赖 Catalyst attribute 复制的 outer plan、
以及把 Spark 当成任意深层行为 oracle；其 lateral/deep-correlation 测试仍明确保留最外层引用不支持的
场景。

### 3.3 DuckDB：holistic 状态和共享执行的行为基线

固定提交中的
`[flatten_dependent_join.cpp](https://github.com/duckdb/duckdb/blob/6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d/src/planner/subquery/flatten_dependent_join.cpp)`
已经先保留完整 `LogicalDependentJoin` 计划，再统一 decorrelate。实现中最值得 Doris 借鉴的是：

- `UnnestingState` 同时携带当前 binding layout 和 `BindingReplacementGraph`；每次 child rewrite 后通过
显式 old/new binding layout 修补父节点，不靠位置猜测；
- `SubtreeAccess {correlated, volatile_expression}` 以 plan identity 缓存，决定何处仍需 active domain；
- 遇到 nested dependent join 时，`PrepareDependentJoinLeft` 先在 parent flattener 中处理 inner left，
再创建 child flattener 处理 inner right，最后合并 replacement graph；
- Projection/Aggregate/Window/Limit/SetOp/Distinct/Join/FullOuter/CTE/recursive CTE 都有独立 handler；
full outer join 在上层 Project 用 `COALESCE(left_binding,right_binding)` 形成代表；
- `FinalizeDependentJoin` 会验证每个 active binding 恰由一个输入拥有，再生成 delim join 和 null-safe
conditions；这类 assert 比“缺列就跳过”更符合 Doris 的编码约束。

对不能按 distinct binding 合并求值的 volatile path，DuckDB 可在 LHS 增加 synthetic row number，
以 row identity 驱动每条 outer row，而不是错误地共享相同业务 key；这启发 Doris 的
`RepeatabilityContract`，但不意味着所有 side effect 的 SQL 语义已经由论文定义。

DuckDB 的 `[plan_delim_join.cpp](https://github.com/duckdb/duckdb/blob/6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d/src/execution/physical_plan/plan_delim_join.cpp)`
让原始 LHS 与 HashDistinct 产生的 domain 在同一 pipeline 中共享，并把多个 DelimScan 连接到同一数据集。
同一固定提交还包含
`[delim_join_cte_rewriter.cpp](https://github.com/duckdb/duckdb/blob/6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d/src/planner/subquery/delim_join_cte_rewriter.cpp)`：
`[delim_join_as_cte](https://github.com/duckdb/duckdb/blob/6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d/src/include/duckdb/main/settings.hpp#L839-L847)`
在该提交中默认 `true`，会将 delim 结构降成 materialized CTE，并继续做 domain 消除和谓词处理。
历史 [PR #23098](https://github.com/duckdb/duckdb/pull/23098) 合入时不建议默认开启，后续
[PR #23333](https://github.com/duckdb/duckdb/pull/23333) 才翻转默认值；设计不能把“存在 CTE rewrite”
误写成它从一开始就是稳定默认路径。

DuckDB 适合作为复杂语义和 plan-shape oracle，但不适合逐行移植：其 table index、projection offset、
binding layout 和执行 pipeline 都不同；CTE rewriter 本身也远不止“包两层 CTE”。Doris 应复用
`ExprId`、`LogicalCTEProducer/Consumer` 和已有 MPP property 体系重新表达。

该快照仍拒绝 correlated multi-column IN/ANY/ALL、percent/non-constant LIMIT/OFFSET、部分 lateral/
right-full lateral 等 shape。因此“生产 holistic 路径”描述的是 framework 和主规划阶段，不是所有 SQL
组合已经支持。

### 3.4 Calcite：最清晰的 Java 状态/输出 contract

固定提交的
`[TopDownGeneralDecorrelator.java](https://github.com/apache/calcite/blob/2ceaf277b8eb16c6dbdce0e6129a8ad7c186a390/core/src/main/java/org/apache/calcite/sql2rel/TopDownGeneralDecorrelator.java)`
与 Doris 同为 Java，几个对象可直接映射设计职责：

- `CorDef(correlationId, field)` 明确 outer field 的 owner；
- `hasCorrelatedExpressions` 预标注子树访问，`mapRelToUnnestedQuery` 缓存 rewrite 结果；
- `UnnestedQuery` 保存 old/new node、`corDefOutputs` 和 `oldToNewOutputs`，解决 operator 改变 output
layout 后父节点如何重写；
- `DedupFreeVarsNode` 表示去重自由变量域；nested Correlate 由共享父状态的 sub-decorrelator 合并；
- Aggregate 的 static case使用 D left join，Sort 的 OFFSET/FETCH 改写为 window，Join/SetOp 都统一
correlation output；full outer join同样以 `COALESCE` 形成代表。

它也给出两个警示。第一，预处理 `HepPlanner` 明确设置 `noDag=true`，因为同一 RelNode 在不同
correlation context 下的 annotation 不同；Doris 的缓存键必须包含 context fingerprint，或在 CTE
阶段显式 tree-cut。第二，类仍标记为 `@Experimental`，属性
`[topDownGeneralDecorrelationEnabled](https://github.com/apache/calcite/blob/2ceaf277b8eb16c6dbdce0e6129a8ad7c186a390/core/src/main/java/org/apache/calcite/config/CalciteConnectionProperty.java#L153-L159)`
默认 `false`。虽然类注释仍写“not yet integrated”，固定提交的
`Programs.subQuery` 和 `DecorrelateProgram` 已接入 opt-in 全链路；应表述为“已 opt-in 集成但非默认
生产路径”，不能只引用过时注释或反过来称其成熟。

其 unsupported dispatcher 会抛出后由最外层捕获，保留完整旧 plan；独立 Window RelNode、recursive
union、CTE producer/consumer 等并没有通用 handler。这正说明状态模型可借鉴，覆盖面不能照抄成 Doris
能力声明。

### 3.5 Materialize 和许可证边界

Materialize 的 decorrelation 也能观察到“distinct outer bindings + RHS + join-back”的 2015 路线，
适合补充理解持续查询/增量语义。当前仓库根许可证说明主体为 Business Source License 1.1，个别文件
可另有许可证头。本文只记录架构现象，不把其源码作为 Apache Doris 实现来源；实现评审中应能将
每个 substantive hunk 追溯到论文、Apache 项目、DuckDB 的公开接口思想或 Doris 自身推导。

### 3.6 落到 Doris 的明确选择


| 选择               | 采用                                                   | 不采用/延后                               |
| ---------------- | ---------------------------------------------------- | ------------------------------------ |
| traversal        | DuckDB/2025 的完整计划、outer-to-inner、parent-aware        | 每个 Apply 独立 bottom-up                |
| logical IR       | Spark 的 lazy placeholder 边界                          | 第一版就新增 physical dependent join       |
| binding identity | Calcite `CorDef` 思路 + Doris `ScopeId/ApplyId/ExprId` | DuckDB table-index/offset 算术         |
| rewrite API      | Calcite/DuckDB 的 output map + replacement graph      | 只返回 Plan 的局部 rule                    |
| correctness      | DuckDB/Spark 测试矩阵 + Doris 现有 mark/scalar 语义          | 依赖单一系统结果作为证明                         |
| execution        | 普通 Aggregate/Join；随后 forced internal CTE             | 第一版复制 DuckDB physical delim pipeline |
| cost choice      | 先总是显式 D；后续比较完整计划                                     | 在局部不等价子树间建 Memo 等价组                  |




## 4. Doris 当前实现



### 4.1 当前主链路

分析期在
[Analyzer.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/jobs/executor/Analyzer.java#L210-L218)
bottom-up 执行 `SubqueryToApply`。它把子查询变成
[LogicalApply.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/trees/plans/logical/LogicalApply.java#L40-L114)，
节点保存：

- `IN_SUBQUERY`、`EXITS_SUBQUERY`（源码当前拼写）、`SCALAR_SUBQUERY` 类型；
- `List<Slot> correlationSlot`；
- 可选 `correlationFilter`、compare expression 和 mark slot。

rewrite 期的
[Rewriter.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/jobs/executor/Rewriter.java#L458-L485)
依次执行：

```text
PullUpProjectUnderApply / filter normalize
        |
AggScalarSubQueryToWindowFunction  -- 窄范围 WinMagic
        |
CorrelateApplyToUnCorrelateApply   -- bottom-up 局部规则
        |
ApplyToJoin                        -- scalar / IN / EXISTS
        |
普通 LogicalJoin
```

`CorrelateApplyToUnCorrelateApply` 只有五组局部规则，见
[CorrelateApplyToUnCorrelateApply.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/rewrite/batch/CorrelateApplyToUnCorrelateApply.java#L31-L51)。
例如
[UnCorrelatedApplyAggregateFilter.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/rewrite/UnCorrelatedApplyAggregateFilter.java#L82-L122)
把相关 filter 拉到 `Apply`，把内侧表达式加入 aggregate group key，再改写为 join。
这里没有从已过滤的左输入产生显式 `D`。

最后的转换包括：

- scalar：cross join、left outer join 或 left semi join；
- IN/NOT IN：semi、anti、NULL-aware anti 或 mark join；
- EXISTS/NOT EXISTS：semi/anti join；
- 非相关 EXISTS 等场景的 limit/count 简化。

当前没有找到 dependent join/Apply 的物理实现；正常路径要求 rewrite 后只留下普通关系算子。

### 4.2 已有能力和正确性处理

当前实现已有下列有价值的基础，不能在重构中回退：


| 能力                                     | 当前处理                                                                                                                                                        |
| -------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `IN/NOT IN`、`EXISTS/NOT EXISTS`、scalar | 转成 semi/anti/mark/outer join                                                                                                                                |
| OR 等复杂布尔上下文                            | mark join 保存布尔结果                                                                                                                                            |
| `NOT IN` + NULL                        | uncorrelated nullable 场景使用 NULL-aware left anti，相关场景显式补 NULL 条件                                                                                             |
| scalar 行数                              | 非聚合 scalar 增加 `count + any_value + assert_true`                                                                                                             |
| 空输入 `COUNT`                            | left join 后以 `NVL` 修复应返回 0 的语义，见 [SubqueryToApply.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/analysis/SubqueryToApply.java#L524-L563) |
| 简单相关 aggregate                         | 把内侧相关表达式加入 group key                                                                                                                                        |
| 特定 aggregate scalar                    | `AggScalarSubQueryToWindowFunction` 可用窗口避免重复扫描                                                                                                              |
| 后续优化                                   | Apply 最终变成普通 join，可继续参加 join reorder、分布规划和 CBO                                                                                                              |




### 4.3 明确的分析期和 rewrite 限制

当前 binder
[ExpressionAnalyzer.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/analysis/ExpressionAnalyzer.java#L308-L340)
明确只查“previous level”的 outer scope。它只能记录直接父层相关列，不能表达祖父层引用；
相关列直接出现在 subquery 内 join conjunct 时也会报错。

[SubExprAnalyzer.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/analysis/SubExprAnalyzer.java#L138-L182)
和其 validator 还会拒绝：

- correlated scalar 的 `ORDER BY ... LIMIT`；
- outer reference 出现在 project、aggregate、join、order-by 表达式中；
- 相关点之上出现 limit、window、join、set operation；
- 两处 operator 同时访问外层列；
- 带 group-by 的相关 aggregate 等结构。

对应实现集中在
[SubExprAnalyzer.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/analysis/SubExprAnalyzer.java#L385-L528)。
现有单测也把 correlated TopN 失败作为预期结果，见
[AnalyzeSubQueryTest.java](../fe/fe-core/src/test/java/org/apache/doris/nereids/rules/analysis/AnalyzeSubQueryTest.java#L250-L267)。

join condition 中的子查询要求每个 conjunct 只有一个 subquery，并且 subquery、比较表达式和相关列
只能整体属于左侧或右侧。
[SubqueryToApply.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/analysis/SubqueryToApply.java#L281-L352)
把同时依赖两侧的情况归为 `UnSupported`（源码当前拼写）。

scalar 的最后转换还要求所有相关 predicate 都是 `EqualTo`，见
[ScalarApplyToJoin.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/rewrite/ScalarApplyToJoin.java#L77-L97)。
仓库回归测试明确断言这些错误，见
[test_subquery_conjunct.groovy](../regression-test/suites/nereids_syntax_p0/test_subquery_conjunct.groovy#L42-L68)。

### 4.4 能力矩阵


| 场景                         | 当前 Doris          | 2025 目标             | 差距性质                  |
| -------------------------- | ----------------- | ------------------- | --------------------- |
| 单层等值 correlated EXISTS/IN  | 支持                | 支持                  | 已具备                   |
| 单层 scalar aggregate        | 部分支持              | 支持                  | operator/predicate 受限 |
| scalar 非等值相关谓词             | 拒绝                | 支持                  | rewrite 不完备           |
| 父层 + 祖父层引用                 | binder 无法表达       | 支持                  | IR/作用域缺失              |
| 两处访问同一外层绑定                 | 拒绝                | 支持                  | 全局状态缺失                |
| correlated TopN/OFFSET     | 拒绝                | 分区 ROW_NUMBER       | operator rule 缺失      |
| correlated window          | 拒绝                | 扩展 partition key    | operator rule 缺失      |
| correlated set operation   | 拒绝                | 各 child 传播绑定        | operator rule 缺失      |
| outer join 内相关访问           | 多数拒绝              | 按 preserved side 处理 | operator rule 缺失      |
| full join condition 同时依赖两侧 | 拒绝                | 先支持论文 singleton 特例  | join condition IR 缺失  |
| shared correlated CTE      | 无 holistic DAG 处理 | producer 一次转换       | DAG 索引缺失              |
| correlated recursive CTE   | 不支持；测试中被注释        | 绑定穿过迭代              | recursive rule 缺失     |
| 选择性绑定域下推                   | 无显式 `D`           | 支持                  | 主要性能缺口                |
| `D` 与列替换 cost choice       | 无                 | Apply 边界完整等价候选      | CBO 集成缺失              |
| unsupported fallback       | 规划失败              | 可选物理 Apply          | 执行能力缺失                |




## 5. Doris 的核心缺陷



### P0：绑定模型只能描述直接父层

`List<Slot> correlationSlot` 没有 provider scope、consumer、层级和 dependent-join 身份。
即使增加更多 rewrite rule，也无法区分同名/同源列属于哪一层，无法正确继承祖先状态。

这是实现 2025 算法的第一阻塞项。

### P0：局部 bottom-up rule 没有 holistic 状态

当前每个 `LogicalApply` 独立转换。没有 `parent`、`accessing`、`repr`、`cclasses`，因而不能在
嵌套 Apply 处把外层绑定先送入内层左侧，再将可达的组合送入右侧。

需要强调：当前没有显式 `D`，所以不是“当前 Doris 已生成多个 `D` 并发生 2015 式爆炸”。
真实现状是：

- 论文中的深层祖先引用通常在 binder/validator 阶段失败；
- 对已支持的局部嵌套模式，多个 Apply 仍被相互独立地 bottom-up 改写；
- 对单层相关 aggregate，内表往往先按全部相关 key 聚合，错失只处理外层实际绑定集合的机会。



### P0：operator 覆盖面靠白名单

TopN、window、set operation、join、grouping 等在分析阶段被禁止，而不是由代数规则转换。
因此支持范围由 plan shape 决定，SQL 的小改动可能让同一语义从可规划变成报错。

### P1：没有显式 `D`，选择性无法尽早进入右子树

假设外表经过 filter 后只剩 100 个 customer，而内表有一亿个 customer 的事实数据。
当前 group-key 扩展仍可能扫描并聚合内表的全部 customer，再与外表 join。显式 `D` 可以先把
100 个 customer 广播或 shuffle 到内侧，以 join/runtime filter 限制扫描和预聚合输入。

这对 Doris MPP 场景可能比单机系统更重要，因为它同时减少 scan、network exchange、
hash table 和 aggregate state。

### P1：正确性逻辑分散在专用规则中

mark join 三值逻辑、scalar cardinality、空输入 `COUNT`、nullable 修正分别散落在不同规则中。
增加 operator 后，容易在某条新路径中漏掉：

- NULL-safe 绑定重连；
- scalar aggregate 空输入一行；
- scalar 子查询最多一行；
- `NOT IN` 的 UNKNOWN；
- outer join unmatched row 的绑定表示。

新 pass 必须以统一不变量为入口，并复用现有表达式和 join 类型，而不是重写一套语义。

### P1：两侧 join condition、CTE DAG 和递归没有统一表示

`SubqueryToApply` 先把 join condition 子查询挂到某一个 child，天然无法表达同时依赖 join 两侧的
情况。CTE producer/consumer 是 DAG，递归 CTE 还有循环边；普通树形 bottom-up rule 无法知道
全部访问点。

### P2：没有 cost-based 的停止策略

2025 算法在访问消失处有两个保持最终结果的策略：

1. 显式把 `D` 与当前子树 join；
2. 若 scoped equivalence 已证明存在局部代表，以该列替换外部引用，并依靠最终 join-back过滤局部超集。

前者可能提供强 runtime filter，后者省去一次 join。当前 Doris 没有这个逻辑选择，也没有绑定域
组合 NDV 和分布成本。二者只有连同最终 join-back 的完整候选才等价，局部 fragment 不能共用 Memo
logical group。

### P2：没有执行期 Apply 兜底

去相关失败时只能报错。完整的物理 Apply/nested-loop fallback 不是 2025 rewrite 的前置条件，
但它能把“暂未覆盖”从功能错误降级为性能较慢。该 fallback 必须有明确的相关执行协议、批量绑定
和资源限制，不能退化为 FE 逐行发起查询。

## 6. 目标架构



### 6.1 阶段位置

建议的新主链路：

```text
Bind owner-aware OuterReference
  -> Normalize all subqueries to LogicalApply/dependent semantics
  -> Build preliminary index + purity/semantic-safe atomic simple elimination
  -> Rebuild final CorrelationPlanIndex + preflight capabilities
  -> HolisticApplyEliminator (outer-to-inner candidate rewrite)
  -> DomainLowering (Aggregate+Join / forced internal CTE)
  -> Semantic lowering (Mark / Single / empty aggregate)
  -> HolisticUnnestingValidator
  -> ordinary Nereids rewrite / Memo / implementation
```

新 pass 置于当前 `Subquery unnesting` topic 内，在 column privilege check 和普通 join reorder 之前，
以 session variable `enable_holistic_subquery_unnesting` 灰度。`AggScalarSubQueryToWindowFunction`
第一阶段可继续作为已证明 shape 的快路径；终局应作为与完整 unnest plan 等价的优化选择，而不是
holistic correctness 的前置条件。

pass 对 immutable 输入构造完整候选，返回三种正常结果：

```java
sealed interface HolisticResult {
    record Success(Plan plan, RewriteDiagnostics diagnostics) implements HolisticResult {}
    record DeclinedLegacySafe(Reason reason) implements HolisticResult {}
    record Unsupported(Reason reason, int inputNodeId) implements HolisticResult {}
}
```

- `DeclinedLegacySafe` 只用于 legacy analyzer 原本就接受的 shape，随后可运行旧规则；
- feature flag 新放开的 shape 若 capability preflight 不通过，返回稳定的 `Unsupported`，不能送入旧规则；
- preflight 之后 handler 缺失、repr 不可见或 validator 失败均是优化器 bug，直接 check/fail fast，不得
伪装成用户不支持；immutable candidate保证失败时不会把半改写树交给后续规则。



### 6.2 完整作用域绑定

引入不可歧义的外部引用：

```java
record ScopeId(long value) {}

record ApplyId(long value) {}

record OuterRefKey(ScopeId providerScope, ExprId exprId) {}

final class OuterReferenceSlot extends Slot {
    OuterRefKey key;
    Slot originalSlot;      // delegate type/name/nullability/lineage
    int lexicalDepth;
    // constructors/visitor/withXxx omitted; every copy must preserve provider
}
```

`ScopeId` 标识列的 lexical owner，`ApplyId` 标识一次 dependent-join/binding 边界，二者不能复用：
同一 outer scope/ExprId 可被两个 sibling subquery 访问，对应同一个 `OuterRefKey`，但属于两个不同 Apply。

`OuterReferenceSlot` 必须是 leaf `Slot`，而不是把 `originalSlot` 藏在普通 Expression 字段中的 wrapper：
Nereids 的 final `Expression.getInputSlots()` 只收集表达式树里的 `Slot`，leaf slot 才能让现有 dependency
分析看到 outer access。`provider + ExprId` 是 owner-aware 稳定身份；`lexicalDepth` 只用于分析期校验、
EXPLAIN 和诊断，因为 plan rewrite 可能改变物理深度。`originalSlot` 只委托 type、nullable、qualifier 和
lineage，不能单独充当 identity。

该类需要专用 visitor/deep-copy/replace 支持；`equals/hashCode` 包含完整 `OuterRefKey`，所有
`withNullable/withDataType/withQualifier/withName/withExprId/withSubPath/withIndexInSql` 都保留 provider
（`withExprId` 同步更新 key 的 ExprId）。owner-sensitive 分析必须收集 `OuterReferenceSlot`/key，不能只用
`getInputSlotExprIds()`，因为后者按设计只返回 ExprId。

改造 `ExpressionAnalyzer.visitUnboundSlot`：

1. 每个 query-block lexical scope 分配 statement-local `ScopeId`，每个 subquery/Apply 边界另分配
  `ApplyId`；块内为 Project/Aggregate/Sort 等创建的派生 `new Scope(...)` 必须继承同一 `ScopeId`，
   不能按 Java `Scope` 对象身份反复分配；
2. 当前 scope 未命中时沿已有 `Scope.outerScope` 完整链查找，不再只 bind previous level；
3. 命中后用 provider scope 的 ID 和原 slot `ExprId` 生成 `OuterReferenceSlot`；
4. subquery 汇总按 key 排序的 references，并保留每个 expression 的 local access site；
5. JOIN condition 不因出现 outer reference 立即拒绝，由 dependent IR 表达它依赖的左右 provider；
6. legacy 路径在边界处显式转换成当前 `correlationSlot` 形式，不能让两种表达混用。

如果同名列在多个作用域可见，继续遵循 SQL 最近作用域规则；不要把多层搜索误实现为多候选匹配。

### 6.3 统一 dependent-join IR

为控制第一批改动范围，继续使用 `LogicalApply` 作为 planning-only dependent-join IR，但增加不可为空的
`CorrelationSpec`，并把当前零散 boolean 收敛为明确的结果语义：

```java
enum ApplySemantics {
    LATERAL_INNER,
    LATERAL_LEFT,
    EXISTS,
    NOT_EXISTS,
    IN,
    NOT_IN,
    SCALAR
}

record CorrelationSpec(
        ApplyId applyId,
        ScopeId rhsScope,
        ImmutableList<OuterRefKey> outerRefs,
        ApplySemantics semantics,
        ApplyConditionSet conditions,
        Optional<MarkJoinSlotReference> markSlot,
        CardinalityContract cardinality,
        EmptyInputContract emptyInput,
        RepeatabilityContract repeatability) {}

record ApplyConditionSet(
        ImmutableSet<PredicateId> rhsCorrelatedPredicates,
        ImmutableList<Expression> boundaryTheta,
        Optional<Expression> semanticCompare) {}
```

`SCALAR` 的合同是每 binding 0 行补 NULL、1 行输出、超过 1 行报错。只有相关谓词/FD 固定完整 unique
key、单一 global grouping set（或 grouping proof明确每 binding最多一组），或已经按 binding 改写且
`limit <= 1` 真正证明 `<=1` 时才能移除 Max1；普通/mixed grouped aggregate 仍可能每 binding 产生多个
group，给它增加 binding key不等于证明 scalar 单行。
`IN/NOT_IN` 继续复用 Doris mark/null-aware join 的三值语义，去相关 pass 不自行发明 Boolean marker。

condition ownership 必须在 normalize 阶段定死：RHS Filter 中的 correlated predicate留在原 operator，
由 handler以 D repr恰好重写一次；lateral/Apply 自身 theta 才进入 `boundaryTheta`；IN 的 outer-vs-subquery
comparison 属于 `semanticCompare`，由 Mark/Semi/Anti lowering消费。每个 predicate分配 `PredicateId`，
`ConditionLedger` 记录 `AT_RHS / RESIDUAL_AT_BOUNDARY / CONSUMED_BY_SEMANTICS / PROVEN_REPLACED`，禁止把
同一 `correlationFilter` 既留在 RHS 又在 join-back重挂。

规范化后 RHS 中仍保留 owner-aware outer-reference expression。当前 `LogicalApply` 具备普通 plan/memo
复制接口，但没有 physical implementation；新阶段合同必须保证 holistic success 后它不会到达 Memo，
且所有表达式只引用 child output。若以后单独引入 `LogicalDependentJoin`，它应替换而不是再包一层
`LogicalApply`。

### 6.4 计划索引与访问标注

为一次 rewrite 建立只读 `CorrelationPlanIndex`：

```java
record NodePosition(
        int nodeId,
        int parentId,
        int depth,
        int preorder,
        int subtreeEnd) {}
```

索引至少维护：

- `IdentityHashMap<Plan, NodePosition>`，以及 parent/depth/Euler interval；
- `OuterRefKey -> defining scope/output producer`、`ExprId -> producing node`；
- `node -> local outer accesses`，不能把 child access 误算成本节点 access；
- `node/contextFingerprint -> subtree access BitSet`；
- `(ApplyId, accessSite, OuterRefKey) -> binding Apply/LCA`，避免 sibling subquery 的相同 key串线；
- `ApplyId -> {rhsScope, outerRefs, accessSites, parentApply, repeatability}`；
- CTE consumer/producer、recursive scan/union 的非树边；
- operator capability 和 purity summary，供改写前一次性 preflight。

当前 `AbstractPlan` 已有 `ObjectId` 和 `getId()`，但需要审计所有 `withChildren/withXxx` 是否稳定继承
ID；不能未经验证就把它当作 rewrite 全程稳定身份。第一版可以由本 pass 为输入树分配临时连续
`nodeId`，避免把全局 ID 语义扩散到其他规则。

对普通树，用 Euler interval 判断包含关系。第一版的 LCA 即使用 parent walk，也封装在 index 后面；
相关 ID 数量较小时可用 dense ordinal + `BitSet` 汇总访问，避免每个 handler 反复 `contains` 扫树。
holistic rewrite 期间不更新 final index；handler 只查询该 immutable 输入树的位置，输出身份由
`RewriteResult` 管理。simple elimination 是此前的独立 candidate phase：preliminary index 已汇总
repeatability、may-throw、`NoneMovableFunction`、Mark/Single 等 semantic barriers；只有移动合法、
能完整消掉一个 Apply且不跨其他 active Apply 时才原子采用，然后重建 index。失败时不保留已移动的
Filter/Map。若以后要
照 2025 Fig.3 保留“部分 simple movement”，每次采用后必须重建或增量更新 topology/access，不能继续
使用旧 Euler/LCA annotation。

CTE 不能简单把 producer 内联多次。索引层将 DAG 分解为：

- producer 为独立转换树；
- 每个 consumer 是访问代理叶子；
- 先汇总所有 consumer 的 accessing/binding requirements，构造按 owner/key 排序的 canonical union
binding layout；
- 只触发一次 producer rewrite，各 consumer 再用 branch-local fresh Slot map投影所需子集；
- branch-local substitution/equivalence 不穿过 tree-cut，producer输入只接受 canonical exact-D state；
- owner、row-identity 或 repeatability requirements无法合并时，preflight 原子拒绝，不能克隆 materialized
producer来假装支持。

同一共享节点在不同 active-correlation 集合下可能有不同 access summary。缓存键必须包含排序后的
`ApplyId` 及其 outer-owner `ScopeId` fingerprint；不能像普通树那样只以 Plan identity 缓存。Phase 1
尚不支持 CTE DAG 时，
preflight 明确拒绝跨 CTE 的相关访问，不能静默 tree-ify 或重复 producer。

这里要区分两个 key：`AccessContextKey(plan, active Apply/Scope fingerprint)` 只缓存只读 annotation；
`RewriteContextKey` 还包含 `DomainId`、canonical binding layout、repr provenance 和 scoped equivalence，
不能仅凭 active-ID set复用 rewritten subtree。user CTE producer不按 RewriteContextKey复制，而是在上述
requirements union后建立一次 canonical rewrite；consumer adapter负责回到各 branch context。

### 6.5 Top-down 状态

建议数据结构：

```java
record UnnestingInfo(
        CorrelationSpec correlation,
        ImmutableSet<OuterRefKey> outerRefs,
        DomainSpec domain,
        Optional<UnnestingInfo> parent) {}

final class UnnestingState {
    UnnestingInfo info;
    ScopedEquivalenceGraph equivalences;
    BindingReplacementGraph replacements;
    ImmutableMap<OuterRefKey, BindingRef> repr;
    Optional<BindingRef> outerRowIdentityRepr;
    AccessSet remainingAccesses;
    SemanticContract semantics;
}

record BindingRef(
        Slot slot,
        Provenance provenance,       // EXACT_DOMAIN | LOCAL_SUPERSET | ROW_ID
        EqualitySemantics equality) {}

record UnnestingRewriteResult(
        Plan plan,
        ImmutableMap<OuterRefKey, Slot> bindingOutputs,
        Optional<Slot> outerRowIdentityOutput,
        ImmutableMap<ExprId, Slot> oldToNewOutputs,
        BindingReplacementGraph replacements,
        ConditionLedger conditions,
        AccessSet remainingAccesses,
        SemanticContract semantics) {}
```

Doris 已有
[ImmutableEqualSet](../fe/fe-core/src/main/java/org/apache/doris/nereids/util/ImmutableEqualSet.java#L32-L112)
可复用 union/find 的基础操作，也已有
[NullSafeEqual](../fe/fe-core/src/main/java/org/apache/doris/nereids/trees/expressions/NullSafeEqual.java#L28-L62)。
但不能把普通 `=` 与 `<=>` 塞进无标签的同一等价类：前者只在 predicate 为 TRUE 的 surviving rows
上等价并蕴含 null rejection，后者允许 NULL identity；outer join 还会使等价事实只在特定 branch
有效。`ScopedEquivalenceGraph` 应在内部复用 equal-set，同时保存 equality kind、scope 和
`requiresNotNull`，Phase 1 只接受 filter/inner-join 中直接 Slot 等值并保留原 predicate。

`BindingReplacementGraph` 必须支持传递 resolve、branch clone/merge、冲突和环检测，以及在每个
parent-child output boundary 验证 old public output 可达；这是 DuckDB #22162 最值得移植的合同，
不是照搬其 table-index 实现。

每个 binding 创建 canonical、重命名后的 `SlotReference`。必须保证：

```text
domain output ExprId ∩ target subtree output ExprId = ∅
```

不要依赖 qualifier 避免冲突，关系代数正确性依赖属性身份分离。

### 6.6 主算法

伪代码：

```text
tryUnnest(plan):
  normalized = normalizeAllSubqueries(plan)
  preliminary = CorrelationPlanIndex.build(normalized)
  simplePlan = atomicSimpleEliminationFixedPoint(normalized, preliminary, semanticBarriers)
  index = CorrelationPlanIndex.build(simplePlan)
  capabilityRegistry.preflight(simplePlan, index) or return DECLINED/UNSUPPORTED
  candidate = rewriteRootToLeaf(simplePlan, noActiveCorrelation)
  lowered = DomainLowering.lower(candidate)
  semanticPlan = lowerMarkSingleAndEmptyContracts(lowered)
  validator.validate(normalized.visibleOutput, semanticPlan)
  return SUCCESS(semanticPlan)

eliminateApply(apply J, optional parentState):
  if index.accessSitesFor(J.applyId).isEmpty():
    return regularizeAndContinueParent(J, parentState)

  left = J.left
  if parentState exists:
    leftAccesses = parentAccessesIn(J.left)       // may be empty
    left = unnestOperator(J.left, parentState, leftAccesses)
    rewrite J.conditions.boundaryTheta and moved maps through ConditionLedger with parentState.repr

  domain = DomainSpec.exact(
      source = left,
      keys = requiredOuterBindings(J, parentState),
      aliases = freshExprIds())

  childState = stateFor(J, domain, parentState)
  rightAccesses = index.accessSitesFor(J.applyId, J.right)
                  union parentAccessesIn(J.right)
  right = unnestOperator(J.right, childState, rightAccesses)

  assert every childState.outerRef has a visible repr in right
  residualTheta = childState.conditions.takeResidualBoundaryTheta()
                                     .rewriteWithVisibleOutputs()
  joinBack = residualTheta
             AND each outer key <=> corresponding repr
             AND optional L.rowIdentity <=> childState.outerRowIdentityRepr
  return finishApplySemantics(
      J, left, right, joinBack, J.conditions.semanticCompare, childState)

unnestOperator(node, state, accesses):
  if accesses is empty:
    return attachExplicitDomain(node, state)   // Phase 1 deterministic choice
  handler = registry.requireHandler(node.type)
  return handler.rewrite(node, state, accesses)
```

嵌套 dependent join 是关键特例：

```text
outer state
    |
rewrite inner.left with outer state
    |
construct reachable inner bindings
    |
merge outer accesses into inner.right
    |
rewrite inner.right
```

禁止把外层和内层各自独立生成的 `D` 直接 cross join。

四个细节是实现验收点：

1. nested Apply 有 parent state 时必须始终处理 inner left；即使 `leftAccesses` 为空，也要在正常 stop
  point 把 parent D/representative 附到 left，child D 才来自实际可达的联合 binding；
2. `DomainSpec` 是 lazy 描述，直到 stop point 才插入 placeholder；如果祖先 pass 先停止，后代 Apply
  之后仍按 root-first 顺序独立处理；
3. `attachExplicitDomain` 为每个 branch 生成 fresh binding slots，返回 output map；不能把同一 `ExprId`
  同时放在 domain 和 target subtree；
4. handler 未覆盖应在 preflight 返回 unsupported；若 rewrite 阶段才发现 handler 缺失、repr 不可见或
  replacement 冲突，则作为内部错误终止；任何情况都不留下半相关计划。



## 7. 各 operator 的转换规则



### 7.1 Handler contract

每个 handler 必须显式声明并实现：

```java
interface OperatorUnnestingHandler<P extends Plan> {
    Capability preflight(P node, CorrelationPlanIndex index, SemanticContract semantics);
    UnnestingRewriteResult rewrite(P node, UnnestingState state, AccessSet accesses);

    boolean isLinearForSimpleElimination();
    PreservedSide preservedSide();
    boolean canEmitUnmatchedRows();
    boolean isCardinalitySensitive();
}
```

`preflight` 是用户 shape 是否支持的唯一入口；`rewrite` 内只处理已经证明存在的路径，违反前置条件用
check/assert 报 optimizer bug。handler 不得自己搜索祖先、重新构造 domain 或猜 outer reference owner。

### 7.2 Transfer-function 矩阵


| Operator               | 规则                                                                                        | 关键正确性点                                                               |
| ---------------------- | ----------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| Filter                 | 用 `repr` 重写 outer ref；从安全 conjunct 更新 scoped equivalence                                  | OR 保持原形；普通 `=` 不升级为 NULL-safe，也不擅自删除                                 |
| Project/Map            | 携带仍被访问的代表列；重写表达式；更新 alias 映射                                                              | 不得丢失后续 operator 所需 `ExprId`                                          |
| Aggregate              | 把 binding repr 加入 group key/grouping sets，输出辅助列                                           | aggregate function 不消费辅助列；保持 bag 语义                                  |
| Static Aggregate       | 先按 binding 聚合，再用 exact D left join恢复缺组                                                    | empty result按函数 metadata；不能 `COUNT(*)` null-padding 行                |
| Window                 | binding repr 加入所有相关 window 的 `PARTITION BY`                                               | 不改变每个绑定内部 order/frame                                                |
| Inner/Cross Join       | 按 access 推一侧或 fork 两侧；双侧逐 key `<=>`                                                       | branch state 独立 clone 后合并，无跨 binding 笛卡尔积                            |
| Semi/Anti/Mark Join    | binding 传播到实际 output/preserved side                                                       | Mark/NOT IN 的 UNKNOWN 继续由现有 join contract 实现                         |
| Left Join              | 优先使用 preserved left repr；必要时两侧 NULL-safe 重连                                               | unmatched right row 不能丢 binding                                      |
| Right Join             | 与 left 对称                                                                                 | preserved right                                                      |
| Full Join              | 两侧分别传播，输出 `COALESCE(left_repr,right_repr)`                                                | 两侧 unmatched row 都保留                                                 |
| Union All              | 每个 child 追加同构 binding output                                                              | 保持重复                                                                 |
| Union/Intersect/Except | 每个 child 带 binding 执行原 ALL/DISTINCT 语义                                                    | binding 成为比较的一部分，schema/nullable/位置完全对齐                              |
| Distinct               | binding 加入 distinct target                                                                | 只隔离 binding，不改变原 distinct 列集合的用户可见输出                                 |
| Sort                   | 无 Limit 且子查询语义不观察顺序时可由既有规则删除；否则每 binding 排序                                               | 不能把 global order 当作 per-binding order                                |
| Limit/TopN             | `ROW_NUMBER() OVER (PARTITION BY binding ORDER BY keys)`，过滤 `offset < rn <= checkedUpper` | 常量 limit/offset；checked overflow；原 NULL order；WITH TIES/PERCENT 另行处理 |
| CTE Producer/Consumer  | producer 转换一次，consumer 传递 canonical binding slots                                         | DAG 全部访问点一致                                                          |
| Recursive Union        | binding 穿过 seed、recursive term、work table                                                 | 迭代 schema、distinct/union-all 和终止条件不变                                 |
| Values/OneRow/Generate | 每个 row/expression改写并追加 binding layout                                                     | table function 的 repeatability/side effect 单独判定                      |


Join 的单侧优化条件不能只问“另一侧是否访问 outer ref”。还必须考虑当前 join 是否会从另一侧产生
unmatched row，以及 semi/anti/mark/single 是否对 partner 数量敏感；只要不能证明单侧代表覆盖全部输出，
就 fork 两侧并以 `<=>` 对齐。

Doris `LogicalUnion` 还可能在 `constantExprsList` 中直接保存没有 child 的常量/VALUES 分支。SetOp handler
必须同时处理 `regularChildrenOutputs` 和这些 constant rows：需要 correlation时先把常量 row规范化为
OneRow/Values branch，再与 D 组合并追加 canonical binding layout。D2b 首版若不实现该 normalization，
preflight 必须要求 `constantExprsList` 为空，不能只遍历 children 后漏掉常量分支。

TopN 的 `checkedUpper = offset + limit` 必须复用 Doris `Utils.addOverflows` 或等价 checked arithmetic；
溢出表示上界不再限制（只保留 `rn > offset`），不能让 signed long 回绕。`limit=0`、`Long.MAX_VALUE`/
无上限 sentinel 和接近上界的 offset 都是 handler 单测，而不是依赖 SQL parser永远给小值。

### 7.3 Static aggregate、空输入与 HAVING

给 global aggregate 直接追加 binding group key 会把“空输入仍有一行”变成“缺失该 binding 的组”，
必须统一 lowering：

```text
DomainBoundInput
  := D JOIN rawInner ON rewrittenCorrelationPredicate

Grouped
  := Aggregate(
       groupBy = D.keys,
       output  = originalAggs + D.keys + TRUE AS match)
       over DomainBoundInput

Restored
  := D LEFT JOIN Grouped ON D.keys <=> Grouped.keys

Project
  := for each aggregate output a:
       IF(match IS NULL, evaluateOnEmpty(a), a)
     + D.keys
```

`match` 必须是新建且不可空的 marker，不能复用 nullable 业务列。`evaluateOnEmpty` 应成为统一的
aggregate-function contract：`COUNT(*)/COUNT(expr)` 为 0，`SUM/MIN/MAX/AVG` 通常为 NULL，其他
builtin/UDAF 按函数 metadata 或已有 zero-tuple evaluator 返回；无法取得 empty result 的函数在
preflight 阶段拒绝。

在追加 D/helper slots 前，aggregate 必须已经固定其显式 referenced inputs；`COUNT(*)`、row/json
aggregation 或 UDAF 不能因为 child schema 扩展而开始消费辅助列。这是 formal v2 的必要前提，也应由
handler 单测直接验证。

原查询有 HAVING 时，先恢复 empty aggregate values，再在每个 binding 上求原 HAVING。普通非 static
GROUP BY/grouping set把 binding keys加入每个 group，空输入仍返回零行，不能套用上述恢复。

混合或重复 grouping sets（尤其同时含 `()`、`GROUPING()/GROUPING_ID`）还需要保留 grouping-set
instance/discriminator：每个 static instance分别恢复一行，重复 grouping set的 multiplicity不能被 D
left join合并。Phase 3 初版只支持 global `groups=()` 与所有 grouping set均非空且不重复的形态；mixed、
duplicate 和 grouping metadata function 在专用 `Repeat`/grouping-set handler 完成前由 preflight 拒绝。

另一种 `D LEFT JOIN rawInner` 后直接聚合的实现只有在用 `COUNT(nonNullMatchMarker)` 并逐个重写所有
aggregate 时才可能正确；`COUNT(*)` 会把 outer join 的 null-padding 行计成 1，第一版不采用该形态。

### 7.4 Scalar、EXISTS 和 IN 的独立语义层

去相关只消除 free variables，不得顺便消掉子查询结果合同：

- **Scalar/Single**：每 binding 的 RHS bag 为 0 行时返回 NULL，1 行时返回该值，2 行及以上报错；
两个相同值、两个 NULL 也算两行。只有 `CardinalityContract` 证明单一 global grouping set、
grouping/相关等式/FD 固定每 binding最多一组或完整 unique key，或已经完成 per-binding lowering 的
`limit <= 1` 时才可省 Max1；否则
第一版在 domain-bound RHS 上增加不可空 row marker，按 binding 计算 `COUNT(marker)` +
`ANY_VALUE(value)`，用现有 `AssertTrue(count <= 1)` 逐 binding 校验；终局可增加 partition-aware
Max1/Single 物理合同。
- **EXISTS/NOT EXISTS**：只有顶层 filter 的安全 conjunct可转 semi/anti；位于 OR、SELECT、CASE、join
predicate 等需要向上返回值的上下文必须保留 existence Mark，但其结果始终二值；outer join 产生的
missing marker需按现有语义 `NVL(..., FALSE)`，RHS predicate 的 UNKNOWN 只是不匹配。global aggregate
即使 raw input 为空也有一行，因此必须在 static aggregate 恢复之后判断 existence。
- **IN/NOT IN**：相关谓词中的 outer ref 被普通 slot 取代，但 TRUE/FALSE/UNKNOWN、RHS empty、
outer NULL 和 inner NULL 仍交给 Doris 现有 Mark/NULL-aware join lowering；形式化论文的二值 predicate
模型不能作为这部分三值语义的证明。
- **Lateral left**：无 RHS row时要 null-extend；lateral inner/cross 保留原 bag multiplicity。

SQL quantified comparison `ANY/ALL` 需要额外的 quantifier + comparison-operator IR 和独立 3VL truth
table；当前 Nereids/本文 `ApplySemantics` 不表达它，故不列入 Phase 3 能力，不能借 `compareExpr` 暗含。

### 7.5 Repeatability 与求值次数

对 volatile、non-deterministic 或有副作用表达式，domain batching 可能改变求值次数。第一版应在明确
判定此类表达式时报告不可去相关，或走物理 Apply；不能把它们当作普通纯函数移动。终局可参考
DuckDB 的思路，将合同分为：

```text
DETERMINISTIC_PER_BINDING  -> DISTINCT(binding keys)
ONCE_PER_OUTER_ROW         -> binding keys + fresh outer row identity，不去重同行
SIDE_EFFECT_UNSPECIFIED    -> explicit unsupported / physical Apply
```

capability/purity summary 不能只检查 `isDeterministic`：还要识别 Doris 的 `NoneMovableFunction`、
may-throw expression 和 semantic assert。`AssertTrue` 即使确定性也不得被 Project 移动/复制/裁掉；
`ScalarSingleLowering` 生成的 per-binding assert必须作为可执行 barrier 保留到 physical plan，不能因为
用户不选择其 Boolean helper output 就被 column pruning 删除。

row identity 必须来自 LHS 的逻辑行实例，不能用可能重复或可空的业务列。它会扩大 D 并关闭按相同
binding 共享 RHS 的优化，因此必须由语义需要触发，而不是通用防御性字段。identity 必须在 LHS
fan-out **之前生成一次**，由同一 forced CTE/spool producer同时提供给原 L 与 D，并加入 D key 和最终
join-back；若在两棵 duplicate tree各算一次，行号没有共享身份，语义仍不成立。

在 MPP 中该 identity 还必须对整个 producer/source 唯一；每 tablet/driver各自从 1 开始的本地
ROW_NUMBER 会碰撞。Phase 4 必须在“需要 exchange 的 global row number”和“稳定复合 token，例如
producer partition/instance identity + local ordinal”之间给出 physical contract，并确保 token随 forced
CTE materialization进入所有 consumers。多 tablet、多并行 instance 是必测环境。

### 7.6 Full-join condition 中的双侧相关子查询

2025 §4.1 的 singleton 特例不能塞进普通单 LHS `LogicalApply`。它需要独立 normalization：为 full join
左右输入分别建立 `D_R/D_S`，在 `D_R × D_S` 上对已证明 singleton 的子查询 T 求 predicate marker，
再按论文规定的 left/full outer 两层顺序与 S、R 重连，使匹配行按真实 binding产生，而两侧 unmatched
row各只产生一次。至少需要：

- `MultiInputCorrelationSpec(leftProviders, rightProviders, singletonProof, marker)`；
- 两个 domain 的 fresh schema、cross-domain size/cost guard 和各自 null-safe join-back；
- T 的 `CardinalityContract` 证明 singleton，不能用运行时任取一行；
- 对 marker TRUE/FALSE/UNKNOWN 与两侧 unmatched multiplicity 的专用 validator。

这不是 Phase 3 普通 Join handler 的一个 case。Phase 4 在单独 mini-design、algebra golden plan 和 truth/
bag matrix合入前继续由 preflight 拒绝；当前文档只固定 phase boundary，不宣称现有 `CorrelationSpec`
已经足够表达。

## 8. `D` 的构造与 MPP 执行



### 8.1 `DomainSpec` 与逻辑构造

```java
record DomainSpec(
        DomainId id,
        Plan source,
        ImmutableList<OuterRefKey> keys,
        ImmutableList<DomainKeySemantics> keySemantics,
        ImmutableMap<OuterRefKey, Slot> freshAliases,
        Optional<Slot> outerRowIdentity,
        DomainCoverage coverage,        // EXACT | PROVEN_SUPERSET
        RepeatabilityContract repeatability,
        DomainMaterialization policy) {}
```

第一版只产生 `EXACT` 且 `outerRowIdentity` 为空：

```text
D = LogicalAggregate(
      groupBy = source.outerRefSlots,
      output  = source.outerRefSlots AS freshDomainSlots,
      child   = real rewritten Apply.left result)
```

这里的 source 必须包含 outer query 在 Apply 左侧已发生的 filter/join，nested case 则必须是 parent state
处理后的真实 reachable left，不得退回 base scan 或把各层单列域相乘。`LogicalAggregate` 给 tuple key
集合语义：重复 outer rows只形成一条 D row，复合 NULL key也作为一个真实 binding。

若 DataTrait 已证明 source 对完整 key tuple 在 `IS NOT DISTINCT FROM` 语义下无重复，可以用 Project +
fresh Alias 取代 distinct aggregate。普通 nullable UNIQUE 往往允许多行 NULL，本身不足以证明 D 的
集合唯一性；除非所有 key 已证明 non-null，或 trait 明确采用 NULLS NOT DISTINCT。证明缺失时必须保留
Aggregate，不能根据 row-count/NDV 估计猜唯一。无论是否省略 Aggregate，都要创建 fresh `ExprId`，
并验证 domain schema 与 target subtree 及下推路径中的每个 schema 不相交。

每个 `DomainKeySemantics` 必须证明该 DataType/Collation 同时支持 Doris GROUP BY/DISTINCT、
`NullSafeEqual`、hash/exchange，且这些路径采用一致的 not-distinct 等价（包括 NULL、float NaN 和
collation）。JSON/复杂或其他不可比较 key 在没有统一语义前由 preflight 稳定拒绝或走 physical Apply，
不能生成一个可 distinct 但不可 join-back 的 D；Spark 对不可比较 domain type 的 fail-fast 可作为测试参照。

最终重连：

```text
left
  JOIN
rewritten-right
  ON left.k <=> rewritten-right.D_k
```

多列绑定必须逐列 `NullSafeEqual`，不能只对 nullable 列特殊处理；这使规则与统计信息是否准确解耦。

原 SQL correlation predicate 保持自己的 `EqualTo`、非等值和三值语义。`NullSafeEqual` 仅用于 D identity、
两个带 D branch 的对齐和最终 join-back；后续 `NullSafeEqualToEqual` 只有在真实 non-null trait 证明下才可
降级。

### 8.2 Domain lowering 与共享策略

`LogicalDomainJoin`（若实现为临时节点）只允许存在于候选 rewrite 内，不能进入 Memo。lowerer 按策略生成：

1. `DUPLICATE_TREE`：像 Spark 一样复制 `DETERMINISTIC_PER_BINDING` source，一份保留为原 L，一份
  计算 D；文件少、适合 Phase 1 vertical slice，但 plan size 和执行成本更高。`ONCE_PER_OUTER_ROW`、
   volatile、`NoneMovableFunction`、may-throw 或 side-effect source 禁止使用；Phase 1 只接受
   repeatable + total 的 source，否则必须先有 forced sharing或返回 unsupported。不能把同一个 Plan
   对象或同一组 `ExprId` 直接挂到两个 sibling；D 分支使用现有
   `LogicalPlanDeepCopier + DeepCopierContext.exprIdReplaceMap`，先把 key映射到 copied slots，再生成
   fresh domain aliases。
2. `FORCED_INTERNAL_CTE`：生成一份 `LogicalCTEProducer(source)`，原 L 和 D 各用一个 consumer；D 再由
  consumer 上的 distinct aggregate 产生。该 CTE 是 correctness/sharing contract，不得被普通阈值内联。
3. `PHYSICAL_DELIM`：同一 LHS pipeline 同时供给原分支与 distinct domain，多处 domain scan 共享；
  只有 CTE 路径的正确性和性能数据证明不足时才进入 BE 设计。

Doris 已有 `LogicalCTEAnchor/Producer/Consumer`、physical CTE 节点和
`StatementContext.addForceMaterializeCTE`。当前较新的 `CTEInliner` 在常规候选路径检查 force 标记，
但 consumer-count 的 empty-relation 快路会先消除/内联；较早的 `CTEInline` 没有该检查。引入 internal
domain CTE 时必须补齐并测试两条路径：非空 internal producer不可因用户阈值展开成两棵 source；静态
empty producer若选择安全消除，应作为显式例外而不是绕过标记。internal domain CTE 还应使用独立
ID/name space；force contract 同时覆盖 `enable_cte_materialize=false` 和任意
`inline_cte_referenced_threshold`，用户 CTE 策略不能改变 internal sharing correctness。

lowerer 不能只拼三类 CTE plan node：必须分配新的 `CTEId`、每个 consumer 的 `RelationId`、
consumer-to-producer Slot map 和 output/replacement map。主 Subquery topic 位于当前 whole-tree
`CTEInline` 之前，可让后续 `CTEInline/RewriteCteChildren/Optimizer.refreshCteContext` 收集这些节点；但
所有 planner 入口必须经测试证明会刷新 `StatementContext/CascadesContext`，绕过该序列的入口则需显式
注册。Phase 4 单测要覆盖 context refresh、两个 consumer 的不同 ExprIds 及 canonical producer mapping。

用户查询原有的 CTE 是另一问题：其 producer schema 和所有 consumers 需要传播 correlation bindings，
且 context-sensitive access index 只能转换 producer 一次。不要用“为 domain 新建 CTE”假装已经支持
相关 user CTE DAG。

### 8.3 分布选择

显式 `D` 进入普通 Memo 后，允许现有实现选择：

- 小 `D` broadcast 到内侧；
- 两侧按 binding hash shuffle；
- 与已有 distribution key colocate；
- 由 `D` join 生成 runtime filter 下推到 scan；
- inner aggregate 按 binding key local/pre-aggregate。

新增统计项：

- binding tuple 的 composite NDV，而不是简单相乘单列 NDV；
- `D` 行数上界和 unique trait；
- NULL fraction；
- binding 与内表 key 的 selectivity；
- broadcast bytes、shuffle bytes、预聚合前后行数。

CTE producer/consumer 还需要暴露多 consumer 所需物理属性：若一个 consumer 需要 broadcast D、另一个
保留原 L distribution，producer materialization 与各 consumer exchange 的成本必须分别计算。runtime
filter 穿过 CTE 的能力也应纳入，而不是默认 D join 一定能下推到 scan。

### 8.4 显式 `D` 与 substitution 的 cost choice

2015 论文给出的局部关系是包含而非相等：substitution 可能让 T 产生没有 D partner 的额外 tuple，最终
原 dependent-join 位置的 join-back 才会过滤它们。因此下面两个 **局部** fragment 不能进入同一 Memo
group：

```text
Fragment A: D <=>-JOIN T
Fragment B: Project(localEquivalent AS binding, T)  // possible superset
```

合法的 cost choice 必须在完整 Apply 边界构造等价候选：

```text
Alternative A (exact domain):
  L JOIN-BACK (D JOIN T)

Alternative B (superset substitution):
  L JOIN-BACK Project(localEquivalent AS binding, T)
```

两者到 Apply 边界后才有相同 visible output 和 bag multiplicity。Alternative B 还必须证明：

- 每个 outer key 有 branch-local representative；
- 普通 `=` 的 null-rejecting predicate仍保留，或显式补 `IS NOT NULL`；
- outer/full join 的 equivalence scope 没有越过 null extension；
- `BindingRef.provenance=LOCAL_SUPERSET`，最终 join-back 在 validator 前不可被删；
- scalar/mark/empty-input semantic operators在两个候选中位置等价；
- substitution 只可能省掉 stop-point 的 D join，不能删除仍被 static-aggregate restorer用作 empty-binding
value generator 的 exact D。若没有等价 generator，COUNT/COALESCE/HAVING 或 global-aggregate EXISTS
的 substitution proof 直接失败；
- stop point 到最终 join-back 之间若有 may-throw expression、semantic assert、volatile/side effect，
LOCAL_SUPERSET 可能在本应被 join-back过滤的额外 tuple 上新增错误或求值，因此禁止 substitution；
除非计划已保证 join-back 在这些表达式求值之前发生。

Phase 1 总是选 A。后续可让 `UnnestingAlternativeBuilder` 在 Apply 边界生成两棵完整普通计划，再把
它们插入同一 Memo group；不能在 stop point 用 `LogicalCorrelationDomainChoice` 欺骗 logical property。
若计划替代数量可能指数增长，只在有完整 substitution proof 的 stop point生成，并设置 statement-level
alternative budget。

cost 对比至少包括：

```text
cost(D join) =
  build/probe + broadcast_or_shuffle + extra_rows
  - runtime_filter_scan_saving
  - pre_aggregation_saving

cost(substitution) =
  project_cost + downstream_cardinality_cost
```

不能只凭 “有等价列就 substitution”，因为显式 `D` 可能以一次小 join 换来数量级的 scan/aggregate
缩减；也不能总是保留 `D`，因为内侧已经被强选择性过滤时 join 可能纯属开销。

## 9. 正确性不变量

新 pass 完成时由专用 validator 和 Java check/precondition 验证：

1. **Ownership**：每个 outer reference唯一归属一个 provider `ScopeId`；每个 access site在当前
  `ApplyId` 下只消费其 binding boundary 可见的 key；sibling Apply 不串线，最近作用域遮蔽正确；
2. **Complete lowering**：计划中不存在 `OuterReferenceSlot`、free correlated slot、待处理
  `LogicalApply` 或 `LogicalDomainJoin`；
3. **Visible output**：根的用户可见 output 数量、顺序、`ExprId`、type、nullable 与候选前一致；隐藏
  binding/marker/row-id 在最后 consumer 后全部裁掉；
4. **Domain coverage**：exact D 等于真实 rewritten L 的 binding projection；superset D/representation
  必须带 proof，并保留最终 join-back；
5. **Domain set semantics**：每个 binding tuple multiplicity 为 1，NULL tuple 参与集合；只去重 D，
  不去重原 L/R bag；
6. **Fresh schema**：每次 domain/branch alias 均为 fresh `ExprId`，与下推路径所有 schema 不相交；
7. **Replacement graph**：无环、无冲突，每个 old public output 和 `repr(c)` 在当前 boundary 有且仅有
  一个真实可见 target；
8. **Equality kind**：domain identity/双 branch/join-back 使用 `NullSafeEqual`；原 SQL 普通 `=`、
  non-equi 和 3VL 不改变；
9. **Per-binding barriers**：Aggregate、Distinct、Window、SetOp、TopN 和 Max1 都包含完整 binding
  partition/layout；
10. **Static aggregate**：每个 D binding 即使 raw inner 为空也经过 empty-result/HAVING 语义；match
  detection 不使用 nullable 业务列；
11. **Scalar**：0/1/>1 合同仍由可见的 Max1/assert/Single 等价结构保证，除非上述
  `CardinalityContract` 有完整 `<=1 per binding` 证明；
12. **Mark semantics**：EXISTS/NOT EXISTS marker始终二值；IN/NOT IN 的 TRUE/FALSE/UNKNOWN、RHS
  empty、outer/inner NULL 继续满足现有 truth table；
13. **Join preservation**：outer/full unmatched row拥有正确代表，full representative来自两侧
  `COALESCE`；
14. **Repeatability**：distinct D 只用于 `DETERMINISTIC_PER_BINDING`；row-sensitive path包含在 fan-out
  前生成的稳定 row identity，且 D key、RHS partition 和最终 join-back都包含它，否则明确 fallback；
15. **DAG/recursive**：access annotation按 context隔离，但同一 materialized producer在 statement 内
  汇总 compatible requirements后只 rewrite/execute一次；所有 consumers 可从 canonical union layout
    投影所需子集，seed/step/work-table binding schema 同构；
16. **Traversal**：普通树节点对一个 active context至多由主算法访问一次；DAG cache 以 context
  fingerprint 区分，不能串用 annotation；
17. **Condition ownership**：每个 original `PredicateId` 在 ledger 中恰有一个终态；RHS predicate、
  boundary theta 和 semantic compare 无 duplicate/missing，final condition 的 input slots均来自对应 children。

这些是成功候选的确定性条件，不应写成“条件不满足就继续生成计划”的防御分支。破坏不变量是 FE
内部错误；只有 capability preflight 才能返回用户可见 unsupported，只有明确 legacy-safe 才能回退。

## 10. 分阶段实现



### 10.1 Phase 0：foundation/shadow mode，不改用户行为

- 为现有 lexical scope/Apply 分别分配 statement-local `ScopeId/ApplyId`，构建 `OuterRefKey` adapter；
- 实现普通树 `CorrelationPlanIndex`、access summary、capability registry 和 diagnostics；
- 在旧成功/失败 SQL 上 shadow 计算 annotation，不放开任何 analyzer 白名单；
- 增加 ownership/access/plan-index/replacement-graph 单测和 rewrite 前后 output invariant；
- 记录 planning time、Apply 数量、相关深度、access 数和 legacy error category。

交付标准：同一输入的用户 plan/result/error 不变；每个现有 correlated slot都有唯一 owner/access；shadow
phase 不生成 domain 或改变 plan。

### 10.2 Phase 1：显式 D vertical slice

- 支持 `Scalar Apply -> [Project] -> Global Aggregate -> [Project] -> Filter -> 单输入子树`；
- 等值 shape 继续走 legacy 快路径，非等值/OR/多 outer refs 走 exact D；
- handler 仅包含 Filter/Project/Global Aggregate/leaf，duplicate source 仅允许 repeatable + total tree；
- Domain placeholder 单独 lower 为 duplicate-tree Aggregate + Join；
- 第一批 aggregate 为 MIN/MAX/SUM/AVG，COUNT/用户 aggregate在 static contract完成前保持 unsupported；
- 不支持 HAVING/grouping sets；Aggregate 上方 Project 只允许 direct alias 或已证明在所有 aggregate
empty-result 为 NULL 时仍 null-propagating 的表达式，`COALESCE/IFNULL` 等延后到 static restore；
- 以 2015 Q2（id/year/major + OR + non-equi）和 Doris 现有 non-EQ error cases 为 vertical test；
- 成功后运行 no-Apply/no-outer-ref/no-domain validator。

交付标准：Q2 由规划失败变为正确普通计划；重复/NULL outer binding 保持 bag；除本 shape 外没有 analyzer
能力变化。

### 10.3 Phase 2：真正的 holistic nested Apply

- 先实现 nested correctness 必需的最小 `StaticAggregateRestorer`：global COUNT/SUM/MIN/MAX/AVG、
empty binding恢复、无 HAVING/grouping sets；这是 ancestor key穿过 nested-left 的前置，不是可延后优化；
- `ExpressionAnalyzer` 沿完整 outer scope chain，RHS 中保留 owner-aware expression；
- root-first 调度、parent state、nested-left-first、parent accesses 合并到 child right；
- 支持 2～6 层父/祖父引用，先限于 Phase 1 的 unary operator 集；
- 加入 2025 §2.3 三层最小例和六层 `crash.sql` 形态的结构/规模测试；
- 证明 aggregate 前只出现真实 reachable compound binding，不出现 `D1 × D2 × ...`。

交付标准：深层查询完成且候选 plan size/intermediate binding 数随真实可达组合增长；祖先 pass 提前 stop
时，后代仍能在后续 root-first 顺序完成。

### 10.4 Phase 3：SQL 核心语义和普通关系算子

- 补全 static aggregate HAVING/函数 metadata，并支持受限的非空不重复 grouping sets；
- per-binding Scalar Max1、EXISTS 二值 Mark，以及 IN/NOT IN 三值 Mark truth table；
- inner/left/right/full/semi/anti/mark join handler；
- Distinct、UNION/INTERSECT/EXCEPT 的 ALL/DISTINCT variants；
- Window 和常量 LIMIT/OFFSET 的 per-binding ROW_NUMBER；
- 明确拒绝 WITH TIES/PERCENT/non-constant limit 及未覆盖 Generate。

交付标准：当前 `correlated_scalar_subquery.groovy` 中对应 unsupported case 按 handler 逐项转为确定结果；
未迁移项仍保持原错误，而不是被宽泛放开。

### 10.5 Phase 4：共享、DAG、递归与 repeatability

- forced internal CTE lowering，使原 L 与 D 共享一个 producer；统一两条 CTE inliner 对 force 标记的处理；
- user CTE tree-cut、context fingerprint、producer 一次转换和 consumer canonical binding layout；
- recursive CTE seed/step/work table schema 扩展；
- full-join condition singleton-subquery 的独立 multi-input mini-design；
- `ONCE_PER_OUTER_ROW` 的 row-identity path，side-effect 未定义形态继续拒绝；
- 评估可选 batch physical Apply fallback，但不让它阻塞 logical holistic 正确性。

交付标准：共享 producer只执行/转换一次；两个 correlation context 不串 annotation；recursive binding
不跨 binding 混合；duplicate-tree 与 CTE lowering 只在 deterministic-per-binding corpus 上做
differential result，row-identity corpus必须验证单一 producer。

### 10.6 Phase 5：Memo、MPP cost 与默认开启

- 在完整 Apply 边界生成 `D join` / substitution 等价计划；
- composite NDV 和 binding unique trait；
- broadcast/shuffle/runtime-filter costing；
- WinMagic 作为另一个等价表达式，而不是 holistic pass 前的特例；
- 比较 forced CTE、duplicate-tree 和可选 physical delim；
- 灰度默认开启，达到迁移阈值后才删除重叠 legacy rules。

交付标准：正确性不依赖 cost；关闭统计信息也能产生正确计划，有统计信息时避免明显的全内表聚合。

### 10.7 建议的 PR 边界


| PR  | 只包含                                                         | 明确不包含                     |
| --- | ----------------------------------------------------------- | ------------------------- |
| A   | owner/access/index/replacement graph shadow infrastructure  | analyzer 放开、结果变化          |
| B   | 单层 Q2 explicit-D vertical slice + validator                 | nested、COUNT、Join/SetOp   |
| C0  | global builtin static restore/COUNT/empty binding（无 HAVING） | nested、Mark、grouping sets |
| C   | scope chain + parent-aware nested unary path                | outer join、CTE DAG        |
| D1  | static HAVING/function metadata/Max1/Mark                   | Window/SetOp              |
| D2a | Join handlers（按 join type逐项启用）                              | SetOp/Window/TopN         |
| D2b | SetOp ALL/DISTINCT handlers                                 | Window/TopN/CTE           |
| D2c | Window 与常量 TopN/OFFSET handlers                             | CTE/recursive             |
| E1  | forced internal domain CTE + CTE context/inliner contract   | user CTE DAG              |
| E2  | user CTE DAG tree-cut 与 consumer layout                     | recursive CTE             |
| E3  | recursive seed/step/work-table bindings                     | row identity              |
| E4  | full-join condition multi-input singleton mini-design       | row identity              |
| E5  | `ONCE_PER_OUTER_ROW` row identity                           | physical Apply（独立评估）      |
| F   | 完整候选 alternatives、统计/分布 cost、默认开关                           | 新 SQL 语义                  |


每个 PR 的 analyzer relaxation 与 handler/test 同批提交；不得先接受一种 SQL shape、再依赖未来 PR
补正确 rewrite。

### 10.8 灰度与可观测性

`enable_holistic_subquery_unnesting` 初始默认 `false`。开发/CI 可另用非用户合同的 shadow hook只运行
preflight/index并比较诊断，不构造替换计划；正式 ON 模式只能接受完整 `Success`。

`EXPLAIN VERBOSE` 或 optimizer trace 应输出：

```text
apply id, provider scope ids, lexical depth, outer-ref count
access-site count and stop node
simple | exact-domain | row-identity | substitution strategy
domain key ExprIds and materialization policy
static-aggregate / single / mark semantic contracts
legacy-safe decline or unsupported reason
holistic planning time and generated plan-node delta
```

FE metrics按 reason 聚合，不记录 SQL literal、列值或完整 query text。默认开启门槛至少包括：已支持 shape
连续通过 differential/regression，shadow 中无 invariant violation，planning P99 与 plan-node 增量在预算内，
且出现问题可仅关闭新 pass 回到未改动的 legacy 路径。legacy rules 要等默认开启后的覆盖率和稳定周期达标
再删除，不能与首次 default-on 同一 PR 完成。

## 11. 测试方案



### 11.1 FE 单测

- `OuterRefKey/OuterReferenceSlot`：最近作用域遮蔽、父/祖父层、同名列、同一 `ExprId` 不同 owner、
同一 query block派生 Scope复用 ID、sibling subquery共享 provider但 ApplyId不同、join conjunct；并验证
`getInputSlots/getInputSlotExprIds`、visitor、`ExpressionUtils.replace`、所有
`withXxx` 与 deep copy 都不丢 provider；
- plan index：local/subtree access、ancestor/LCA、nested parent、context-sensitive CTE cache；
- replacement graph：传递 resolve、插入顺序、branch clone/merge、冲突、环、boundary output missing；
- condition ledger：RHS correlated predicate、boundary theta、IN semantic compare在 simple/OR/non-equi/
nested parent rewrite后各出现一次，状态迁移无 duplicate/missing；
- scoped equivalence：普通 `=`/`<=>`、null rejection、outer-join scope、project rename；
- `DomainSpec`：exact source、fresh/disjoint `ExprId`、NULL composite key；nullable unique + 两个 NULL
必须保留 Aggregate，non-null/nulls-not-distinct unique才允许 Project shortcut；
- domain key semantics：GROUP BY/`NullSafeEqual`/hash 对 NULL、NaN、collation一致；不可比较复杂类型稳定拒绝；
- 每个 handler 的 capability/preflight、输入/输出 map 和 negative path；
- static aggregate zero-tuple evaluator、HAVING 和 match marker；
- per-binding scalar 0/1/>1 assert 与 Max1 elimination proof；
- semantic barrier：上层 Project 不输出 assert helper时，`NoneMovableFunction` 仍保留并实际触发错误；
- atomic result state和 final validator 的每条 invariant。



### 11.2 Regression

在 `query_p0/subquery` 下按阶段新增 suite，例如：

```text
holistic_unnesting_domain.groovy
holistic_unnesting_nested.groovy
holistic_unnesting_semantics.groovy
holistic_unnesting_operators.groovy
holistic_unnesting_cte.groovy
```

最小交叉矩阵如下：


| 类别                  | 必测数据/shape                                                     | 断言                                           |
| ------------------- | -------------------------------------------------------------- | -------------------------------------------- |
| 2015 Q1             | EQ scalar MIN；nullable sid                                     | 旧快路径等价；普通 `=` 的 null rejection 未丢            |
| 2015 Q2             | OR + AND；id/year/major；non-equi                                | 无需拆 OR；生成 exact D；结果正确                       |
| Multiplicity        | L 同 binding 1/2/100 行；R 重复                                     | D 一行；join-back 恢复 L bag                      |
| NULL identity       | 单/复合 outer key NULL；inner NULL                                 | D NULL binding不丢；原 predicate 仍 3VL           |
| Hidden output       | 多层 Alias/Project/Distinct                                      | binding 到最后 consumer前可见，最终被裁掉                |
| Empty aggregate     | COUNT(*), COUNT(nullable), SUM/MIN/MAX/AVG × inner 0/1/many    | 每 binding default 与 HAVING 正确                |
| Empty vs NULL row   | RHS 无行；RHS 一行业务值 NULL                                          | match marker能区分两者                            |
| Scalar              | 每 binding 0/1/2 行；两行相同/不同/全 NULL                               | 0→NULL、1→值、2→error                           |
| Grouped scalar      | 普通 group 0/1/2 groups；global `()`                              | Max1 不因追加 D key 消失；global empty正确恢复          |
| Mixed grouping sets | mixed/duplicate sets；GROUPING_ID                               | 初版 preflight 稳定拒绝；专用 handler 后再转结果测试         |
| Window              | rank/row_number/frame；各 binding 数据量不同                          | partition完全隔离                                |
| TopN                | limit 0/1/MAX、多行、offset近上界、tie、NULL order                      | checked upper不溢出；未支持 variant 稳定报错            |
| SetOp               | UNION/INTERSECT/EXCEPT ALL/DISTINCT                            | 每 binding bag/set count 正确                   |
| Union constant rows | `constantExprsList` × NULL/duplicate binding                   | 首版稳定拒绝或 normalization 后每 binding复制且 layout正确 |
| Join                | inner单/双侧 access；left/right/full matched/unmatched             | branch `<=>` 和 representative 正确             |
| Exists Mark         | EXISTS/NOT EXISTS；RHS predicate UNKNOWN/empty；OR/value context | 始终 TRUE/FALSE，missing marker归 FALSE          |
| IN Mark             | IN/NOT IN；outer/inner NULL；RHS empty；OR/value context          | 完整三值 truth table                             |
| Nested              | 2～6 层；inner 同时引用 parent/grandparent                            | 无独立域乘积；最终无 outer ref                         |
| CTE                 | 双 consumer；不同 context；materialize true/false × threshold 0/大值  | producer一次；internal force不受用户变量；layout对齐     |
| Recursive           | seed/step/scan 各读取 binding                                     | 迭代不跨 binding，schema 同构                       |
| Repeatability       | random/volatile/side-effect stub；多 tablet/并行 instance          | source-wide row-id无碰撞，或 atomic unsupported   |
| Candidate failure   | unsupported operator位于深层                                       | 原计划未半改写；legacy-safe/unsupported 分类正确         |


每个结果 query 用 `order_qt` 或 SQL `ORDER BY`；预期错误用 `test { sql; exception }`；测试表在 suite
开头 drop/create，结束时保留；`.out` 只能由 `run-regression-test.sh` 生成。运行时使用目录和 suite
同时限定：

```bash
./run-regression-test.sh --run -d query_p0/subquery -s holistic_unnesting_domain
```

计划断言至少检查：holistic success 后无 Apply/OuterReference/Domain placeholder；Phase 2 的 nested
aggregate group key是 reachable compound binding；Phase 4 的 internal CTE producer没有被 inline。

### 11.3 Differential、property 和 fuzz

- 生成深度 1～6、有界 operator 集的 correlated SQL，小表高权重注入 NULL/duplicate/empty；
- 同一 seed 同时执行 nested SQL 与由 generator 独立产生的 ordinary join/window 参考 SQL；
- 以 `GROUP BY all_columns, COUNT(*)` 或排序后的完整 bag 比较 multiplicity，不能只比较 DISTINCT result；
- scalar 的“多行报错”也是 oracle result，不只比较成功 rows；
- DuckDB/PostgreSQL 可做第二 oracle，但只对双方支持且语义一致的 shape；不能用一个实现覆盖论文未证明的
Doris Mark/side-effect 合同；
- 每次同时运行结构 validator，防止“结果在小数据碰巧正确但仍有 domain cross product”。



### 11.4 性能基准

- 2025 §2.3 的三层 minimal pathology，按每层 NDV 扩大；
- 六层 `crash.sql`/procbench UDF18 形态，但使用可公开重建的等价结构，不依赖未提供原 SQL；
- TPC-H/TPC-DS correlated 变体；
- 外表强过滤后 D 很小、D 接近 L、substitution selective/unselective 两组；
- Doris 特有的 bucket/shuffle/broadcast 和 runtime filter 组合。

记录：

- planning time；
- peak FE/BE memory；
- `D` NDV 和实际行数；
- inner scan rows；
- pre-aggregate input rows；
- network bytes；
- runtime filter selectivity；
- end-to-end latency。

验收以增长曲线和 plan/profile 结构为主：inner aggregate input 应随真实 reachable bindings增长，而不是
各层单列 NDV 的乘积。论文中的 120GB/33ms 等数字只说明病理量级，不作为 Doris 的硬时间阈值；共享
机器上的 wall-clock 容易波动，必须同时记录 rows、bytes、peak memory 和 exchange。

## 12. 风险与控制


| 风险                                    | 控制                                                                               |
| ------------------------------------- | -------------------------------------------------------------------------------- |
| bag/NULL 语义回归                         | 把 NULL-safe、mark、static aggregate 写成全局 invariant；高权重 fuzz                        |
| analyzer 先放开、handler 尚未支持             | capability registry 与 analyzer relaxation 同 PR；unsupported shape 保持稳定错误          |
| rewrite 与 join reorder 互相破坏           | holistic pass 完成并验证无 free ref 后才进入普通 join reorder                                |
| plan identity 在 immutable rewrite 中失效 | pass 内临时 node id；所有位置查询经统一 index                                                 |
| replacement 串错列或形成环                   | owner-aware key；显式 old/new boundary；graph 冲突/环 check                             |
| 备选计划指数增长                              | 只在 Apply 边界生成完整等价候选；statement-level alternative budget                           |
| `D` 导致额外 shuffle                      | composite NDV、distribution 和 runtime-filter 收益共同 costing                         |
| duplicate-tree 重复昂贵 outer             | deterministic Phase 1 白名单；尽快切 forced internal CTE；profile plan size              |
| internal CTE 被重新 inline               | 两条 CTE inliner 都识别 force-materialize；validator 检查 producer/consumer              |
| internal CTE 物化大 L 占用资源               | 统计/内存/落盘成本；pure path可比较 duplicate；row-id path评估 physical delim                   |
| user CTE producer 被重复转换               | context-aware tree-cut；producer/context 只处理一次，consumer 用 canonical slots         |
| recursive CTE schema 错位               | seed/worktable/recursive term 的 canonical binding slots 同构校验                     |
| static aggregate/Max1 漏语义             | 独立 semantic contract；match marker；zero-tuple 和 per-binding cardinality matrix    |
| 非确定表达式求值次数改变                          | repeatability contract；row identity或拒绝/physical Apply                            |
| 新旧规则混合                                | immutable candidate；仅 `DeclinedLegacySafe` 回旧路径；success 后不再运行 legacy Apply rules |
| 参考实现许可证污染                             | Materialize 只做概念对照；实现 hunk追溯论文/Apache/Doris clean-room 推导                        |




## 13. 建议的代码落点

概念边界如下；文件只在对应 phase 创建，不要求第一批 PR 一次铺满：

```text
rules/analysis/
  BindOwnerAwareOuterReference.java       # Phase 2 完整 scope chain
  NormalizeSubqueryApply.java             # 保留 Mark/Single/empty contract

trees/expressions/
  OuterReferenceSlot.java

trees/plans/logical/
  LogicalApply.java                       # 增加 CorrelationSpec；仍为 planning-only
  LogicalDomainJoin.java                  # 可选临时节点，lowering 后必须消失

rules/rewrite/subquery/
  HolisticApplyEliminator.java             # orchestration + atomic result
  CorrelationPlanIndex.java                # input topology/access/purity
  CorrelationCapabilityRegistry.java       # analyzer/preflight 共用
  model/
    ScopeId.java
    ApplyId.java
    PredicateId.java
    CorrelationSpec.java
    OuterRefKey.java
    DomainSpec.java
    UnnestingInfo.java
    UnnestingState.java
    UnnestingRewriteResult.java
    SemanticContract.java
    BindingReplacementGraph.java
    ScopedEquivalenceGraph.java
    ConditionLedger.java
    AccessSet.java
  domain/
    DomainLowering.java                    # duplicate tree / internal CTE
    UnnestingAlternativeBuilder.java       # Phase 5 完整 Apply 边界 alternatives
  semantics/
    StaticAggregateLowering.java
    ScalarSingleLowering.java
    MarkApplyLowering.java
  operator/
    UnnestFilter.java
    UnnestProject.java
    UnnestAggregate.java
    UnnestJoin.java
    UnnestSetOperation.java
    UnnestWindow.java
    UnnestTopN.java
  HolisticUnnestingValidator.java

jobs/executor/Rewriter.java                # 新旧路径互斥调度
qe/SessionVariable.java                    # feature flag / diagnostics
```

除新增类外，各 phase 必须显式修改/验证现有入口，而不是假定框架会自动识别新 identity：


| Phase | 现有入口                                                                              | 必要改动                                                   |
| ----- | --------------------------------------------------------------------------------- | ------------------------------------------------------ |
| 0/2   | `analyzer/Scope.java`、`StatementScopeIdGenerator.java`/`StatementContext`         | 生成并传播独立 `ScopeId/ApplyId`                              |
| 1/2   | `ExpressionAnalyzer.java`、`SubExprAnalyzer.java`、`SubqueryToApply.java`           | owner-aware bind、capability-gated validator、Apply spec |
| 0/2   | `ExpressionVisitor.java`、`ExpressionDeepCopier.java`、expression replace utilities | 识别/复制/替换 `OuterReferenceSlot` 且保留 owner                |
| 1     | `LogicalApply.java`、`Rewriter.java`、`RuleType.java`                               | 新旧路径互斥、planning-only post-condition、trace              |
| 4     | `CTEInline.java`、`CTEInliner.java`、`RewriteCteChildren`/optimizer CTE context     | internal force、consumer maps、context refresh           |


ID 可以由 `StatementScopeIdGenerator` 增加新的强类型 generator，也可由 `StatementContext` 持有计数器；
无论选择哪处，不能复用 `ExprId/ObjectId/CTEId` 的整数并靠类型外约定区分。

不要让每个 operator rule 自己寻找 ancestor、复制 domain 或猜 provider。它们只消费
`CorrelationPlanIndex + UnnestingState`，返回 `UnnestingRewriteResult`。Phase 4 的 internal CTE 还会对
`CTEInline/CTEInliner` 做一项最小兼容修改：两者共同尊重 force-materialize；这项改动与首次使用 internal
CTE 同 PR，不能提前做通用 CTE refactor。

Phase 0/1 的最小实际修改面应限制为：`LogicalApply`/owner adapter、index/model、三个 unary handlers、
domain lowerer、validator、`Rewriter` feature-flag wiring 和对应 FE/regression tests。Join/SetOp/CTE 文件
不能因为“以后会用”而先做空框架或顺手重构。

## 14. 最终建议

实现顺序必须是：

```text
owner/access foundation（shadow）
  -> 单层 Q2 explicit-D vertical slice
  -> minimal global static-empty restorer
  -> 完整 scope chain + parent-aware nested traversal
  -> 补全 HAVING/Single/Mark 与 operator 覆盖
  -> CTE/DAG/recursive/repeatability
  -> 完整边界的 D/substitution/WinMagic alternatives
  -> MPP cost、共享与默认开启
```

如果跳过前两步，直接在现有 `UnCorrelatedApplyAggregateFilter` 周围增加 `D`，只能得到一个新的
bottom-up 局部算法：既无法支持论文的祖先引用，又可能复现 2015 的独立域组合问题。

因此，终局架构的最小不变量不是“新增一条 domain join rule”，而是：

- owner-aware lexical binding；
- 统一 dependent-join IR；
- plan/access index；
- 带 parent state 的 top-down rewrite；
- 显式 output/replacement graph；
- bag/NULL/empty/Single/Mark 不变量；
- 原子 candidate + final validator；
- 最后才是 cost-based 的 `D`、substitution、sharing 与 WinMagic 选择。

第一条行为 PR 可以只覆盖 Q2 unary shape，但它的数据结构和 pass boundary 必须已经容纳上述终局状态；
这使后续增加 handler 是扩展能力，而不是推翻单层 ScalarApply 特例。

这条路线既能补齐 Doris 的 SQL 能力，也能把选择性绑定尽早送进 MPP scan/aggregate，从而将
2025 算法的主要性能收益转化为 Doris 现有执行器可消费的普通计划。

## 参考资料

输入 HTML 的 `../paper/*.pdf` 在本 checkout 中不存在，以下均使用可达的一手链接；二手阅读目录只作
导航，不作为算法或源码事实的唯一证据。

### 论文

- [Neumann, Kemper: Unnesting Arbitrary Queries, BTW 2015，GI 官方页](https://dl.gi.de/handle/20.500.12116/2418)
（LNI P-241, pp.383–402；核心 domain/push-down：论文 PDF pp.6–9 / LNI pp.388–391）
- [Neumann: Improving Unnesting of Complex Queries, BTW 2025，GI 官方页](https://dl.gi.de/handle/20.500.12116/45881)
与 [DOI 10.18420/BTW2025-01](https://doi.org/10.18420/BTW2025-01)
（LNI P-361, pp.25–47；主算法 Fig.3–6：PDF pp.9–11；operator §3.3：pp.15–16；
复杂结构 §4：pp.16–20）
- [Neumann: A Formalization of Top-Down Unnesting, arXiv:2412.04294](https://arxiv.org/abs/2412.04294)
（BTW 2025 引用 2024 v1；本文核验当前 2026 v2；Theorem 4.1/Eq.17：PDF pp.5–6；
fresh/disjoint schema：§5 pp.12–13）
- [Fent, Moerkotte, Neumann: Asymptotically Better Query Optimization Using Indexed Algebra,
PVLDB 16(11), 2023](https://www.vldb.org/pvldb/vol16/p3018-fent.pdf)
- [Neumann, Leis, Kemper: The Complete Story of Joins (in HyPer), BTW 2017](https://dl.gi.de/handle/20.500.12116/922)
（Single Join/Max1 语义补充）



### 开源实现与调研入口

- [Spark DecorrelateInnerQuery（固定提交）](https://github.com/apache/spark/blob/eb327b68ab8571d425ed08d820ae8ccbafabf32f/sql/catalyst/src/main/scala/org/apache/spark/sql/catalyst/optimizer/DecorrelateInnerQuery.scala)、
[DomainJoin](https://github.com/apache/spark/blob/eb327b68ab8571d425ed08d820ae8ccbafabf32f/sql/catalyst/src/main/scala/org/apache/spark/sql/catalyst/plans/logical/basicLogicalOperators.scala#L2475-L2496)、
[optimizer integration](https://github.com/apache/spark/blob/eb327b68ab8571d425ed08d820ae8ccbafabf32f/sql/catalyst/src/main/scala/org/apache/spark/sql/catalyst/optimizer/subquery.scala)
- [DuckDB holistic flatten（固定提交）](https://github.com/duckdb/duckdb/blob/6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d/src/planner/subquery/flatten_dependent_join.cpp)、
[PR #17294](https://github.com/duckdb/duckdb/pull/17294)、
[binding graph PR #22162](https://github.com/duckdb/duckdb/pull/22162)、
[Delim-to-CTE](https://github.com/duckdb/duckdb/blob/6a3a26ffa866fcfccb74bbc1a9780b00a9ba082d/src/planner/subquery/delim_join_cte_rewriter.cpp)
- [Calcite TopDownGeneralDecorrelator（固定提交）](https://github.com/apache/calcite/blob/2ceaf277b8eb16c6dbdce0e6129a8ad7c186a390/core/src/main/java/org/apache/calcite/sql2rel/TopDownGeneralDecorrelator.java)、
[Programs opt-in integration](https://github.com/apache/calcite/blob/2ceaf277b8eb16c6dbdce0e6129a8ad7c186a390/core/src/main/java/org/apache/calcite/tools/Programs.java)、
[CALCITE-7031](https://issues.apache.org/jira/browse/CALCITE-7031)
- [Materialize lowering（仅 source-available 对照）](https://github.com/MaterializeInc/materialize/blob/cb4e73fb79ed2d533baba040fbc2e5cd15e938b6/src/sql/src/plan/lowering.rs)、
[BSL 1.1 license](https://github.com/MaterializeInc/materialize/blob/cb4e73fb79ed2d533baba040fbc2e5cd15e938b6/LICENSE)
- [awesome-db-optimizer/unnset 阅读目录](https://github.com/0AyanamiRei/awesome-db-optimizer/tree/main/unnset)

