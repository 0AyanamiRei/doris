# Doris 去相关子查询现状与 2025 Holistic Unnesting 设计

## 1. 结论

本文基于 Apache Doris 本仓库提交
`8f6bd0cb680afa8a457f6878768a155b9124d655`、题目给出的
[awesome-db-optimizer/unnset](https://github.com/0AyanamiRei/awesome-db-optimizer/tree/main/unnset)
资料，以及 2015、2023、2024、2025 年的原始论文。

结论是：

1. Doris 已能把常见的 `IN/NOT IN`、`EXISTS/NOT EXISTS` 和标量相关子查询改写成
   semi/anti/mark/outer join，且已经处理三值逻辑、标量子查询行数校验、空输入上的
   `COUNT` 等重要语义。
2. 当前实现是“分析期限制 + bottom-up 局部 pattern rule”，不是完整的关系代数去相关算法。
   它没有显式绑定域 `D`，没有跨多层 `Apply` 的统一状态，也不能表示祖父层及更外层引用。
3. 这意味着当前最主要的问题是**能力不完备和错失选择性下推**。不能直接说 Doris 已经遭遇
   2015 Domain-D 算法的笛卡尔积缺陷；论文中的典型深层案例在 Doris 中通常会更早被拒绝。
4. 不建议继续以单条规则方式修补。建议增加一个 feature-flag 控制的、top-down 的
   `HolisticSubqueryUnnesting` pass：先建立完整作用域绑定和 dependent-join IR，再以
   `accessing + parent + D + repr + cclasses` 状态一次处理整棵相关子查询树。
5. 显式 `D` 和等价列替换不应在 rewrite 阶段硬编码二选一。二者应产生相同输出
   `ExprId` 的等价逻辑表达式，进入同一个 Memo group，由现有 CBO 根据 NDV、分布、
   shuffle 和 runtime filter 收益选择。

目标不是照抄某个单机数据库实现，而是保留 2025 算法的语义不变量，并让生成的普通
aggregate/join/window 计划继续进入 Doris 现有的 Cascades、MPP 分布和执行实现。

## 2. 算法基线

### 2.1 2015：以绑定域 `D` 消除 dependent join

[Unnesting Arbitrary Queries（2015）](https://db.in.tum.de/teaching/ws2223/foundationsde/unnesting.pdf)
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
L depjoin R
  ≡
L ⋈null-safe (D depjoin R)
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

[Improving Unnesting of Complex Queries（BTW 2025）](https://15799.courses.cs.cmu.edu/spring2025/papers/11-unnesting/neumann-btw2025.pdf)
把算法改成 top-down，并统一处理所有 dependent join。核心不是一组额外 pattern，而是三个部分：

1. **识别访问关系。** 对每个 dependent join，标出右子树中哪些 operator 访问它提供的外部列。
   论文以 producer、accessing operator 的最近公共祖先为基础，并使用
   [Indexed Algebra（CIDR 2023）](https://www-db.cs.tum.edu/~fent/papers/p2755-fent.pdf)
   高效回答树位置问题。
2. **尽量做 simple unnesting。** 线性路径上的 filter 可并入 join，map 可上移。所有访问都被
   消掉时，dependent join 直接变成普通 join，不必创建 `D`。
3. **需要 `D` 时 top-down 传播统一状态。** 遇到嵌套 dependent join，内层继承外层状态；
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

2024 的形式化技术报告补充了实现时必须保留的条件：

- 使用 bag/multiset 语义证明，而不是默认 set 语义；
- `D` 必须无重复，并覆盖左输入的绑定投影；
- `D` 的属性命名空间必须与被推入的子树分离，冲突时显式 rename；
- group-by 增加 `D` 列后，aggregate 本身不能错误地消费这些辅助列；
- 遇到嵌套 dependent join 时，外层 `D` 只进入内层左侧；内层 join 再把表示传给右侧；
- 每个 operator 至多访问一次，最终不再残留 dependent join。

### 2.4 2025 覆盖的复杂 operator

2025 论文不只解决深层 filter/aggregate，还给出以下规则：

- left/right/full outer join：分别维护两侧表示，以 NULL-safe equality 重连；full join 的代表列
  使用 `COALESCE(left_repr, right_repr)`；
- window：把绑定表示加入 `PARTITION BY`；
- `ORDER BY ... LIMIT/OFFSET`：改写为按绑定分区的 `ROW_NUMBER`，再过滤行号；
- set operation：把绑定列传入所有 child，并保持各自 bag/set 语义；
- scalar aggregate：为空输入的每个绑定保留一行，不能只做普通 inner join；
- shared CTE DAG：把 DAG 视为 producer tree 加 consumer proxy tree，producer 只转换一次；
- recursive CTE：把绑定列同时穿过 seed、recursive term 和 work table；
- full join condition 中同时引用 join 两侧的相关子查询。

## 3. Doris 当前实现

### 3.1 当前主链路

分析期在
[Analyzer.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/jobs/executor/Analyzer.java#L210-L218)
bottom-up 执行 `SubqueryToApply`。它把子查询变成
[LogicalApply.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/trees/plans/logical/LogicalApply.java#L40-L114)，
节点保存：

- `IN_SUBQUERY`、`EXITS_SUBQUERY`、`SCALAR_SUBQUERY` 类型；
- `List<Slot> correlationSlot`；
- 可选 `correlationFilter`、compare expression 和 mark slot。

rewrite 期的
[Rewriter.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/jobs/executor/Rewriter.java#L226-L255)
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

### 3.2 已有能力和正确性处理

当前实现已有下列有价值的基础，不能在重构中回退：

| 能力 | 当前处理 |
|---|---|
| `IN/NOT IN`、`EXISTS/NOT EXISTS`、scalar | 转成 semi/anti/mark/outer join |
| OR 等复杂布尔上下文 | mark join 保存布尔结果 |
| `NOT IN` + NULL | uncorrelated nullable 场景使用 NULL-aware left anti，相关场景显式补 NULL 条件 |
| scalar 行数 | 非聚合 scalar 增加 `count + any_value + assert_true` |
| 空输入 `COUNT` | left join 后以 `NVL` 修复应返回 0 的语义，见 [SubqueryToApply.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/analysis/SubqueryToApply.java#L393-L447) |
| 简单相关 aggregate | 把内侧相关表达式加入 group key |
| 特定 aggregate scalar | `AggScalarSubQueryToWindowFunction` 可用窗口避免重复扫描 |
| 后续优化 | Apply 最终变成普通 join，可继续参加 join reorder、分布规划和 CBO |

### 3.3 明确的分析期和 rewrite 限制

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
把同时依赖两侧的情况归为 `UnSupported`。

scalar 的最后转换还要求所有相关 predicate 都是 `EqualTo`，见
[ScalarApplyToJoin.java](../fe/fe-core/src/main/java/org/apache/doris/nereids/rules/rewrite/ScalarApplyToJoin.java#L77-L97)。
仓库回归测试明确断言这些错误，见
[test_subquery_conjunct.groovy](../regression-test/suites/nereids_syntax_p0/test_subquery_conjunct.groovy#L42-L68)。

### 3.4 能力矩阵

| 场景 | 当前 Doris | 2025 目标 | 差距性质 |
|---|---|---|---|
| 单层等值 correlated EXISTS/IN | 支持 | 支持 | 已具备 |
| 单层 scalar aggregate | 部分支持 | 支持 | operator/predicate 受限 |
| scalar 非等值相关谓词 | 拒绝 | 支持 | rewrite 不完备 |
| 父层 + 祖父层引用 | binder 无法表达 | 支持 | IR/作用域缺失 |
| 两处访问同一外层绑定 | 拒绝 | 支持 | 全局状态缺失 |
| correlated TopN/OFFSET | 拒绝 | 分区 ROW_NUMBER | operator rule 缺失 |
| correlated window | 拒绝 | 扩展 partition key | operator rule 缺失 |
| correlated set operation | 拒绝 | 各 child 传播绑定 | operator rule 缺失 |
| outer join 内相关访问 | 多数拒绝 | 按 preserved side 处理 | operator rule 缺失 |
| full join condition 同时依赖两侧 | 拒绝 | 支持 | join condition IR 缺失 |
| shared correlated CTE | 无 holistic DAG 处理 | producer 一次转换 | DAG 索引缺失 |
| correlated recursive CTE | 不支持；测试中被注释 | 绑定穿过迭代 | recursive rule 缺失 |
| 选择性绑定域下推 | 无显式 `D` | 支持 | 主要性能缺口 |
| `D` 与列替换 cost choice | 无 | Memo 等价表达式 | CBO 集成缺失 |
| unsupported fallback | 规划失败 | 可选物理 Apply | 执行能力缺失 |

## 4. Doris 的核心缺陷

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

2025 算法在访问消失处有两个等价选择：

1. 显式把 `D` 与当前子树 join；
2. 若 `cclasses` 已证明存在局部等价列，以该列替换外部引用。

前者可能提供强 runtime filter，后者省去一次 join。当前 Doris 没有这个逻辑选择，也没有绑定域
组合 NDV 和分布成本。

### P2：没有执行期 Apply 兜底

去相关失败时只能报错。完整的物理 Apply/nested-loop fallback 不是 2025 rewrite 的前置条件，
但它能把“暂未覆盖”从功能错误降级为性能较慢。该 fallback 必须有明确的相关执行协议、批量绑定
和资源限制，不能退化为 FE 逐行发起查询。

## 5. 目标架构

### 5.1 阶段位置

建议的新主链路：

```text
BindSubqueryScopes
        |
NormalizeSubqueryToDependentJoin
        |
BuildCorrelationPlanIndex
        |
HolisticSubqueryUnnesting      <-- top-down，一次完成
        |
CheckNoFreeOuterReference
        |
普通 Nereids rewrite / Memo / implementation
```

初期置于当前 “Subquery unnesting” topic 内，替换
`CorrelateApplyToUnCorrelateApply + ApplyToJoin`，并在 session variable
`enable_holistic_subquery_unnesting` 下灰度。

旧链路只能在新 pass **尚未开始改变计划且明确返回 DECLINED** 时回退。不能部分转换后再交给旧规则，
否则两套相关列语义会混合。

### 5.2 完整作用域绑定

引入不可歧义的外部引用：

```java
record CorrelationId(long value) {}

record OuterReference(
        CorrelationId provider,
        int lexicalDepth,
        Slot slot) {}
```

改造 `ExpressionAnalyzer.visitUnboundSlot`：

1. 当前 scope 未命中时沿完整 outer-scope chain 查找，不再只看 previous level；
2. 将命中的 provider scope 和 slot 记录为 `OuterReference`；
3. 在每个 subquery 边界分配稳定 `CorrelationId`；
4. 保留原 slot 的 type、nullable、qualifier 和 `ExprId` 来源，但外部引用本身不能只靠
   `ExprId` 推断层级；
5. JOIN condition 不在 binder 阶段拒绝，由后续 IR 表达其访问集合。

如果同名列在多个作用域可见，继续遵循 SQL 最近作用域规则；不要把多层搜索误实现为多候选匹配。

### 5.3 统一 dependent-join IR

可重构 `LogicalApply`，也可先引入新节点：

```java
enum DependentJoinKind {
    INNER, LEFT, SINGLE, SEMI, ANTI, NULL_AWARE_ANTI, MARK
}

record CorrelationSpec(
        CorrelationId id,
        ImmutableSet<OuterReference> outerRefs,
        Optional<Expression> compareExpr,
        Optional<MarkJoinSlotReference> markSlot,
        ScalarCardinality scalarCardinality) {}
```

`SINGLE` 表示 scalar 语义，不要过早拆成 `any_value/count/assert`；在 aggregate/outer join 规则完成后，
统一插入 cardinality check。现有 mark/null-aware 逻辑应复用。

规范化后，子查询中的外部列仍以 `OuterReference` 存在。只有 holistic pass 结束后，所有表达式才必须
只引用 child output。

### 5.4 计划索引与访问标注

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

- node 到 parent/depth/Euler interval；
- `ExprId`/relation output 的 producer；
- 每个 `OuterReference` 的 consumer operator；
- 每个 dependent join 的所有 accessing operator；
- CTE consumer 到 producer 的边。

当前 `AbstractPlan` 已有 `ObjectId` 和 `getId()`，但需要审计所有 `withChildren/withXxx` 是否稳定继承
ID；不能未经验证就把它当作 rewrite 全程稳定身份。第一版可以由本 pass 为输入树分配临时连续
`nodeId`，避免把全局 ID 语义扩散到其他规则。

对普通树，用 Euler interval 判断祖先关系，以 binary lifting 或离线 LCA 求最近公共祖先。
第一版即使使用一次 O(n) parent walk，也应封装在 index 后面；不能在每条 operator rule 中反复
`contains` 扫描子树。

CTE 不能简单把 producer 内联多次。索引层将 DAG 分解为：

- producer 为独立转换树；
- 每个 consumer 是访问代理叶子；
- 汇总所有 consumer 的 accessing 信息后，只触发一次 producer rewrite；
- consumer 通过稳定的 canonical binding output 对接转换后的 producer。

### 5.5 Top-down 状态

建议数据结构：

```java
record UnnestingInfo(
        CorrelationSpec correlation,
        ImmutableSet<OuterReference> outerRefs,
        LogicalPlan domain,
        Optional<UnnestingInfo> parent) {}

final class UnnestingState {
    UnnestingInfo info;
    ImmutableEqualSet.Builder<Slot> cclasses;
    Map<OuterReference, Slot> repr;
    Set<CorrelationId> accessing;
}
```

Doris 已有
[ImmutableEqualSet](../fe/fe-core/src/main/java/org/apache/doris/nereids/util/ImmutableEqualSet.java#L32-L112)
可作为 `cclasses` 的基础，也已有
[NullSafeEqual](../fe/fe-core/src/main/java/org/apache/doris/nereids/trees/expressions/NullSafeEqual.java#L28-L62)。
需要为 equal-set 增加 clone/merge/project 等适合分支状态的操作，但不另造不同语义的等价类实现。

每个 binding 创建 canonical、重命名后的 `SlotReference`。必须保证：

```text
domain output ExprId ∩ target subtree output ExprId = ∅
```

不要依赖 qualifier 避免冲突，关系代数正确性依赖属性身份分离。

### 5.6 主算法

伪代码：

```text
unnest(plan):
  index = buildIndex(plan)
  return rewriteTopDown(plan, emptyState)

rewriteTopDown(node, state):
  if node is DependentJoin J:
    info = makeInfo(J, state.info)
    if accessing(J).isEmpty():
      return ordinaryJoin(J)

    left = rewriteTopDown(J.left, state inherited from parent)
    D = distinct(project required bindings from left)
    childState = state.push(info, D)
    right = rewriteTopDown(J.right, childState)
    return finishDependentJoin(J, left, right, childState)

  state = consumeOuterRefsInExpressions(node, state)
  result = applyOperatorRule(node, state)

  if accessing(current correlation) becomes empty:
    return chooseDomainJoinOrSubstitution(result, state)
  return result
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

## 6. 各 operator 的转换规则

| Operator | 规则 | 关键正确性点 |
|---|---|---|
| Filter | 用 `repr` 重写外部列；从 `EqualTo`/`NullSafeEqual` 更新 `cclasses` | 普通 `=` 不能被错误提升成 NULL-safe 语义 |
| Project/Map | 携带仍被访问的代表列；重写表达式；更新 alias 映射 | 不得丢失后续 operator 所需 `ExprId` |
| Aggregate | 把 binding repr 加入 group key/grouping sets，输出辅助列 | aggregate function 不消费辅助列；保持 bag 语义 |
| Scalar Aggregate | 对每个绑定保持一行，可用 domain LEFT JOIN/group join | 空输入 `COUNT=0`、其他 aggregate 的 NULL 语义 |
| Window | binding repr 加入所有相关 window 的 `PARTITION BY` | 不改变每个绑定内部 order/frame |
| Inner/Semi/Anti Join | 按 accessing 将状态推到一侧或两侧 | mark/anti 的 UNKNOWN 语义沿用现有实现 |
| Left Join | 优先使用 preserved left repr；必要时两侧 NULL-safe 重连 | unmatched right row 不能丢 binding |
| Right Join | 与 left 对称 | preserved right |
| Full Join | 两侧分别传播，输出 `COALESCE(left_repr,right_repr)` | 两侧 unmatched row 都保留 |
| Union All | 每个 child 追加同构 binding output | 保持重复 |
| Union/Intersect/Except | 每个 child 带绑定列执行原 set 语义 | binding 成为集合比较的一部分 |
| Sort | 在同一 binding 内排序；只有语义无关时才删除 | 不能把全局排序当作每绑定排序 |
| Limit/TopN | `ROW_NUMBER() OVER (PARTITION BY binding ORDER BY keys)`，过滤 `offset < rn <= offset+limit` | tie、NULL ordering、offset 与原语义一致 |
| CTE Producer/Consumer | producer 转换一次，consumer 传递 canonical binding slots | DAG 全部访问点一致 |
| Recursive Union | binding 穿过 seed、recursive term、work table | 迭代 schema、distinct/union-all 和终止条件不变 |
| Generate/Lateral | 将 binding 作为 table-function 输入的一部分 | 对非确定/有副作用函数设置 barrier |

对 volatile、non-deterministic 或有副作用表达式，domain batching 可能改变求值次数。第一版应在明确
判定此类表达式时报告不可去相关，或走物理 Apply；不能把它们当作普通纯函数移动。

## 7. `D` 的构造与 MPP 执行

### 7.1 逻辑构造

```text
D = LogicalAggregate(
      groupBy = canonicalized outerRefs,
      output  = canonicalized outerRefs,
      child   = filtered left input)
```

`D` 具有唯一性 DataTrait。若左输入已有覆盖这些列的 unique trait，可省略 aggregate。

最终重连：

```text
left
  JOIN
rewritten-right
  ON left.k <=> rewritten-right.D_k
```

多列绑定必须逐列 `NullSafeEqual`，不能只对 nullable 列特殊处理；这使规则与统计信息是否准确解耦。

### 7.2 分布选择

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

### 7.3 显式 `D` 与 substitution 的 Memo 选择

在访问消失处生成相同 logical properties 和相同 canonical output `ExprId` 的两个等价表达式：

```text
Alternative A:
  D <=>-JOIN T
  -> Project(canonical binding slots, T outputs)

Alternative B:
  Project(localEquivalent AS canonical binding slots, T outputs)
```

Alternative B 只在 `cclasses` 证明每个 binding 都有局部表示时合法。

建议用 exploration rule 把两个普通逻辑计划插入同一个 Memo group，而不是增加必须物理实现的
`LogicalCorrelationDomainChoice` 节点。若需要 wrapper 表达 rewrite 中间态，该节点必须在 Memo
实现阶段前完全消失。

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

## 8. 正确性不变量

新 pass 完成时以 `DORIS_CHECK` 或专用 validator 验证：

1. 计划中不存在 `OuterReference` 或 free correlated slot；
2. 不存在 `LogicalDependentJoin`/待处理 `LogicalApply`；
3. 每个 `D` 对 binding tuple 唯一，并覆盖对应左输入绑定投影；
4. domain 和被推入子树的辅助 `ExprId` 命名空间不相交；
5. 每个 operator 输出所有下游仍需的 `repr`；
6. domain 重连全部使用 `NullSafeEqual`；
7. scalar cardinality check 未被绕过；
8. static aggregate 为每个输入绑定返回一行；
9. mark/NOT IN 保持 SQL 三值逻辑；
10. outer join 的 preserved/unmatched 行都有正确绑定表示；
11. grouping sets、set operation、window 和 TopN 保持 bag 语义；
12. 每个 plan operator 在一次 holistic pass 中最多被主算法访问一次。

这些是确定性前置条件，不应写成“条件不满足则继续生成计划”的防御分支。发现破坏不变量应在 FE
直接报告内部错误；只有在 pass 开始前判定某个 SQL 特性尚未实现时才允许 DECLINED/fallback。

## 9. 分阶段实现

### Phase 0：语义基线和观测

- 建立 current capability SQL matrix；
- 给 Apply 增加 explain/debug 信息：correlation id、provider depth、consumer；
- 增加 rewrite 前后 plan invariant 测试；
- 记录 planning time、Apply 数量、相关深度和报错类型。

交付标准：不改变用户行为，能稳定复现所有当前支持/拒绝场景。

### Phase 1：多层 binder + 核心 holistic pass

- 完整 outer scope chain 和 `OuterReference`；
- dependent-join IR；
- 普通树的 plan index/accessing；
- filter/project/inner join/aggregate；
- 显式 `D`，先不做 cost choice；
- 嵌套 2～6 层父/祖父引用；
- scalar cardinality、NULL-safe rejoin、空 scalar aggregate。

交付标准：论文中的深层 pathological query 不报错、不产生独立域笛卡尔积，pass 后无 free reference。

### Phase 2：完整关系算子

- left/right/full join；
- set operation 和 grouping sets；
- window；
- per-binding TopN/LIMIT/OFFSET；
- non-equality predicate；
- mark、NOT IN 和复杂布尔组合的统一验证。

交付标准：这些场景从显式“不支持”测试迁移为有序结果测试。

### Phase 3：DAG 和递归

- shared CTE producer/consumer index；
- correlated recursive CTE；
- full join condition 同时依赖两侧；
- volatile/side-effect barrier 与可选 physical Apply fallback。

交付标准：producer 只转换一次，递归 binding schema 在 seed/worktable/recursive term 一致。

### Phase 4：Memo 和 MPP cost

- `D join` / substitution 等价表达式；
- composite NDV 和 binding unique trait；
- broadcast/shuffle/runtime-filter costing；
- WinMagic 作为另一个等价表达式，而不是 holistic pass 前的特例；
- 灰度默认开启，最终删除重叠的 legacy rules。

交付标准：正确性不依赖 cost；关闭统计信息也能产生正确计划，有统计信息时避免明显的全内表聚合。

## 10. 测试方案

### 10.1 FE 单测

- scope chain：最近作用域遮蔽、父层、祖父层、相关 join conjunct；
- plan index：ancestor/LCA/accessing、稳定 node identity；
- `cclasses/repr`：分支 clone、merge、project、rename；
- 每个 operator 的转换输入/输出；
- pass 后 no-free-reference validator；
- `D` unique/coverage 和 disjoint `ExprId`。

### 10.2 Regression

新增例如：

```text
regression-test/suites/query_p0/subquery/holistic_unnesting/
```

覆盖：

1. 2～6 层嵌套，内层同时引用父层和祖父层；
2. 外层强过滤，验证 `EXPLAIN` 中 `D` 在 aggregate 前；
3. 重复绑定和 NULL binding；
4. empty input 的 `COUNT/SUM/AVG`；
5. scalar 0/1/>1 行；
6. correlated non-equality；
7. grouping sets、union/intersect/except；
8. window frame；
9. TopN + offset + NULL ordering；
10. left/right/full join 的 matched/unmatched 行；
11. full join condition 同时依赖两侧；
12. shared CTE 被多个 consumer 使用；
13. recursive CTE；
14. mark/OR/NOT IN 三值真值表；
15. volatile expression 明确拒绝或 fallback。

所有结果用 `order_qt` 或显式 `ORDER BY`，错误用 `test { sql; exception }`，`.out` 必须由
`run-regression-test.sh` 自动生成。

### 10.3 Differential 和 fuzz

- 生成有界深度的相关子查询；
- 与逐层手写 ordinary-join/window 等价 SQL 比较 multiset result；
- 对 NULL、重复值、空表和 outer join 提高采样权重；
- 可再与支持该语义的 PostgreSQL/DuckDB 结果交叉检查；
- 对 rewrite 后计划做结构 invariant 检查，而不只比较结果。

### 10.4 性能基准

- 2025 论文的多层 correlated aggregate pathological query；
- 2015/2025 资料中的 `crash.sql`；
- paper 的 procedural/UDF 深层案例；
- TPC-H/TPC-DS correlated 变体；
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

不把某一个查询的加速比作为唯一验收条件。正确性覆盖、深度增长曲线和峰值内存更能发现
2015 式组合爆炸。

## 11. 风险与控制

| 风险 | 控制 |
|---|---|
| bag/NULL 语义回归 | 把 NULL-safe、mark、static aggregate 写成全局 invariant；高权重 fuzz |
| rewrite 与 join reorder 互相破坏 | holistic pass 完成并验证无 free ref 后才进入普通 join reorder |
| plan identity 在 immutable rewrite 中失效 | pass 内临时 node id；所有位置查询经统一 index |
| 备选计划指数增长 | 只在访问停止点向同一 Memo group 增加局部等价表达式 |
| `D` 导致额外 shuffle | composite NDV、distribution 和 runtime-filter 收益共同 costing |
| CTE producer 被重复展开 | producer 独立树，只转换一次，consumer 用 proxy |
| recursive CTE schema 错位 | seed/worktable/recursive term 的 canonical binding slots 同构校验 |
| 非确定表达式求值次数改变 | purity barrier；未证明安全时拒绝或 physical Apply |
| 新旧规则混合 | 新 pass 原子成功或在变更前 DECLINED |

## 12. 建议的代码落点

第一阶段可按以下边界拆分：

```text
rules/analysis/
  BindOuterReference.java
  NormalizeSubqueryToDependentJoin.java

trees/plans/logical/
  LogicalDependentJoin.java
  LogicalCorrelationDomain.java       # 仅当中间态确有必要

rules/rewrite/subquery/
  CorrelationPlanIndex.java
  CorrelationSpec.java
  UnnestingInfo.java
  UnnestingState.java
  HolisticSubqueryUnnesting.java
  HolisticUnnestingValidator.java
  operator/
    UnnestFilter.java
    UnnestProject.java
    UnnestAggregate.java
    UnnestJoin.java
    UnnestSetOperation.java
    UnnestWindow.java
    UnnestTopN.java

rules/exploration/
  CorrelationDomainJoinToSubstitution.java
```

不要让每个 operator rule 自己寻找 ancestor、复制 domain 或猜 provider。它们只消费
`CorrelationPlanIndex + UnnestingState`，返回新 plan 和更新后的状态。

## 13. 最终建议

实现顺序必须是：

```text
多层引用能被准确表示
  -> dependent join 有完整访问关系
  -> top-down 算法保证正确性
  -> 扩展 operator 覆盖
  -> CBO 选择 D/substitution/WinMagic
  -> MPP cost 与性能调优
```

如果跳过前两步，直接在现有 `UnCorrelatedApplyAggregateFilter` 周围增加 `D`，只能得到一个新的
bottom-up 局部算法：既无法支持论文的祖先引用，又可能复现 2015 的独立域组合问题。

因此，2025 版优化的最小正确实现不是“新增一条 domain join rule”，而是：

- 完整 lexical binding；
- 统一 dependent-join IR；
- plan/access index；
- 带 parent state 的 top-down rewrite；
- bag/NULL/outer-join/scalar 不变量；
- 最后才是 cost-based 的 `D` 与 substitution 选择。

这条路线既能补齐 Doris 的 SQL 能力，也能把选择性绑定尽早送进 MPP scan/aggregate，从而将
2025 算法的主要性能收益转化为 Doris 现有执行器可消费的普通计划。

## 参考资料

- [awesome-db-optimizer：unnset 阅读目录](https://github.com/0AyanamiRei/awesome-db-optimizer/tree/main/unnset)
- [Unnesting Arbitrary Queries, BTW 2015](https://db.in.tum.de/teaching/ws2223/foundationsde/unnesting.pdf)
- [Indexed Algebra, CIDR 2023](https://www-db.cs.tum.edu/~fent/papers/p2755-fent.pdf)
- [A Formalization of Top-Down Unnesting, arXiv 2024](https://arxiv.org/abs/2412.04294)
- [Improving Unnesting of Complex Queries, BTW 2025](https://15799.courses.cs.cmu.edu/spring2025/papers/11-unnesting/neumann-btw2025.pdf)
- [BTW 2025 论文元数据与 DOI](https://portal.fis.tum.de/en/publications/improving-unnesting-of-complex-queries/)
