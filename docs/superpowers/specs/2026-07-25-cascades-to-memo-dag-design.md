# Cascades Initial Memo DAG Design

## Goal

Add a companion SVG for the three-table teaching query that previews the Memo
immediately after `CascadesContext.toMemo()` initializes it from the rewritten
logical plan. Keep the existing final-Memo diagram unchanged.

## Semantic Boundary

The diagram represents the state after `new Memo(connectContext, plan)` has
recursively inserted the rewritten plan and before statistics derivation,
DPHyp/Cascades exploration, physical implementation, property enforcement,
costing, best-plan selection, or physical-plan post-processing.

For the captured query there are no pre-materialized-view alternatives to copy
into the root Group. The initial Memo therefore contains 15 Groups, `@0`
through root `@14`, with one logical GroupExpression per Group:

- `@0`-`@2`: lineitem scan, filter, and projection
- `@3`-`@5`: orders scan, filter, and projection
- `@6`-`@8`: customer scan, filter, and projection
- `@9`: orders/customer join
- `@10`: projection above the orders/customer join
- `@11`: lineitem joined with the orders/customer result
- `@12`: final output projection
- `@13`: logical sort
- `@14`: result sink

The figure must not show PhysicalExpressions, Enforcers, statistics, costs,
lowest-plan entries, chosen markers, or the later alternative Groups
`@15`-`@19`.

## Visual Design

Create
`docs/nereids-cascades-q3/memo/cascades-to-memo-dag.svg` using the visual
language of `cascades-dag.svg`:

- light slate background, white Group cards, and a highlighted root card;
- monospace Group and expression text;
- arrows from each parent GroupExpression to its child Groups;
- three aligned relation branches that converge through the two joins;
- a compact legend distinguishing the initial logical expression from
  deliberately absent later Memo content;
- a subtitle that states the exact `toMemo()` snapshot boundary.

Use a tree-like layout because the initial Memo has no equivalent logical
alternatives and therefore no shared or cyclic dependency edges. Retain the
term “DAG” because the objects and edges are Memo Groups and GroupExpressions,
and the figure is intended as the before-state companion to the final Memo DAG.

## Source of Truth

- Rewritten input tree:
  `docs/nereids-cascades-q3/logical/result.txt`
- Final Group numbering and initial logical expressions:
  `docs/nereids-cascades-q3/memo/cascades.txt`
- Memo initialization order:
  `Memo.init`, which recursively initializes children before allocating the
  parent Group
- Existing visual conventions:
  `docs/nereids-cascades-q3/memo/cascades-dag.svg`

## Validation

- Validate the SVG as XML with `xmllint --noout`.
- Verify exactly 15 Group cards covering every Group from `@0` through `@14`.
- Verify exactly 14 cross-Group child edges.
- Verify the SVG contains no PhysicalExpression, Enforcer, cost, statistics,
  chosen-marker, or `@15`-`@19` content.
- Compare every displayed child reference with the rewritten logical tree and
  the initial logical expressions retained in the final Memo capture.
- Render or visually inspect the SVG when a local renderer is available.
