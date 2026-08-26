// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.rules.rewrite.subquery;

import org.apache.doris.nereids.hint.DistributeHint;
import org.apache.doris.nereids.jobs.JobContext;
import org.apache.doris.nereids.trees.expressions.ExprId;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.NamedExpression;
import org.apache.doris.nereids.trees.expressions.NullSafeEqual;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.expressions.functions.NoneMovableFunction;
import org.apache.doris.nereids.trees.expressions.functions.agg.AggregateFunction;
import org.apache.doris.nereids.trees.expressions.functions.agg.Count;
import org.apache.doris.nereids.trees.expressions.functions.agg.Max;
import org.apache.doris.nereids.trees.expressions.functions.agg.Min;
import org.apache.doris.nereids.trees.plans.DistributeType;
import org.apache.doris.nereids.trees.plans.JoinType;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.logical.LogicalAggregate;
import org.apache.doris.nereids.trees.plans.logical.LogicalApply;
import org.apache.doris.nereids.trees.plans.logical.LogicalFilter;
import org.apache.doris.nereids.trees.plans.logical.LogicalJoin;
import org.apache.doris.nereids.trees.plans.logical.LogicalOneRowRelation;
import org.apache.doris.nereids.trees.plans.logical.LogicalPlan;
import org.apache.doris.nereids.trees.plans.logical.LogicalProject;
import org.apache.doris.nereids.trees.plans.logical.LogicalRelation;
import org.apache.doris.nereids.trees.plans.logical.LogicalSubQueryAlias;
import org.apache.doris.nereids.trees.plans.visitor.CustomRewriter;
import org.apache.doris.nereids.util.ExpressionUtils;

import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableSet;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;

/**
 * Domain-based correlated scalar-subquery elimination.
 *
 * <p>This is the first production slice of the 2015 algorithm and the orchestration skeleton of the 2025
 * algorithm. It supports Filter/Project/global Aggregate over an independent relational subtree, including an
 * inner join, and MAX/MIN/COUNT. Nested Apply is processed root first: the parent domain is attached to the nested
 * left input before the child domain is derived, so the child domain contains reachable compound bindings instead
 * of a product of independent domains.</p>
 */
public class HolisticApplyEliminator implements CustomRewriter {

    @Override
    public Plan rewriteRoot(Plan plan, JobContext jobContext) {
        if (jobContext == null || jobContext.getCascadesContext().getConnectContext() == null
                || !jobContext.getCascadesContext().getConnectContext().getSessionVariable()
                        .isEnableHolisticSubqueryUnnesting()
                || !plan.containsType(LogicalApply.class)) {
            return plan;
        }
        CorrelationPlanIndex index = CorrelationPlanIndex.build(plan);
        return rewritePlan(plan, index);
    }

    private Plan rewritePlan(Plan plan, CorrelationPlanIndex index) {
        if (plan instanceof LogicalApply) {
            LogicalApply<?, ?> apply = (LogicalApply<?, ?>) plan;
            Plan rewrittenLeft = rewritePlan(apply.left(), index);
            LogicalApply<?, ?> candidate = rewrittenLeft == apply.left()
                    ? apply : (LogicalApply<?, ?>) apply.withChildren(
                            ImmutableList.of(rewrittenLeft, apply.right()));
            return tryRewriteRootApply(candidate, index).map(UnnestingRewriteResult::getPlan).orElse(candidate);
        }

        ImmutableList.Builder<Plan> children = ImmutableList.builderWithExpectedSize(plan.arity());
        boolean changed = false;
        for (Plan child : plan.children()) {
            Plan rewrittenChild = rewritePlan(child, index);
            children.add(rewrittenChild);
            changed |= rewrittenChild != child;
        }
        return changed ? plan.withChildren(children.build()) : plan;
    }

    private Optional<UnnestingRewriteResult> tryRewriteRootApply(
            LogicalApply<?, ?> apply, CorrelationPlanIndex index) {
        if (!isEligibleApply(apply, index) || !(apply.left() instanceof LogicalPlan)
                || !(apply.right() instanceof LogicalPlan)
                || !isSupportedIndependentSubtree((LogicalPlan) apply.left())) {
            return Optional.empty();
        }
        Map<Slot, Slot> sourceRepresentatives = findSourceRepresentatives(
                apply.getCorrelationSlot(), (LogicalPlan) apply.left(), Collections.emptyMap());
        if (sourceRepresentatives.size() != apply.getCorrelationSlot().size()) {
            return Optional.empty();
        }

        DomainSpec domain = new DomainSpec((LogicalPlan) apply.left(), sourceRepresentatives);
        UnnestingState state = new UnnestingState(apply, domain, Optional.empty());
        Optional<UnnestingRewriteResult> right = rewriteWithBinding((LogicalPlan) apply.right(), state, index);
        if (!right.isPresent()) {
            return Optional.empty();
        }
        return finishApply(apply, (LogicalPlan) apply.left(), sourceRepresentatives,
                right.get(), Collections.emptyMap());
    }

    private Optional<UnnestingRewriteResult> rewriteWithBinding(
            LogicalPlan plan, UnnestingState state, CorrelationPlanIndex index) {
        if (plan instanceof LogicalApply) {
            return rewriteNestedApply((LogicalApply<?, ?>) plan, state, index);
        }
        if (!containsOuterReference(plan, state.getOuterReferences())
                && !plan.containsType(LogicalApply.class)) {
            if (!isSupportedIndependentSubtree(plan)) {
                return Optional.empty();
            }
            DomainSpec.DomainInstance domain = state.getDomain().materialize();
            LogicalJoin<LogicalPlan, LogicalPlan> attached = new LogicalJoin<>(JoinType.CROSS_JOIN,
                    ExpressionUtils.EMPTY_CONDITION, ExpressionUtils.EMPTY_CONDITION,
                    new DistributeHint(DistributeType.NONE), domain.getPlan(), plan, null);
            return Optional.of(new UnnestingRewriteResult(attached, domain.getOuterToDomain()));
        }
        if (plan instanceof LogicalFilter) {
            return rewriteFilter((LogicalFilter<?>) plan, state, index);
        }
        if (plan instanceof LogicalProject) {
            return rewriteProject((LogicalProject<?>) plan, state, index);
        }
        if (plan instanceof LogicalAggregate) {
            return rewriteAggregate((LogicalAggregate<?>) plan, state, index);
        }
        return Optional.empty();
    }

    private Optional<UnnestingRewriteResult> rewriteFilter(
            LogicalFilter<?> filter, UnnestingState state, CorrelationPlanIndex index) {
        if (!expressionsAreRepeatable(filter)) {
            return Optional.empty();
        }
        LogicalPlan childPlan = (LogicalPlan) filter.child();
        Set<Expression> remainingConjuncts = new LinkedHashSet<>(filter.getConjuncts());
        List<LogicalPlan> unaryNodesAboveApply = new ArrayList<>();
        LogicalPlan nestedCandidate = childPlan;
        while ((nestedCandidate instanceof LogicalFilter
                || nestedCandidate instanceof LogicalSubQueryAlias
                || (nestedCandidate instanceof LogicalProject
                    && !((LogicalProject<?>) nestedCandidate).isDistinct()))
                && expressionsAreRepeatable(nestedCandidate)) {
            unaryNodesAboveApply.add(nestedCandidate);
            nestedCandidate = (LogicalPlan) nestedCandidate.child(0);
        }
        if (nestedCandidate instanceof LogicalApply) {
            LogicalApply<?, ?> nestedApply = (LogicalApply<?, ?>) nestedCandidate;
            Set<Slot> availableOnNestedLeft = new LinkedHashSet<>(nestedApply.left().getOutput());
            availableOnNestedLeft.addAll(state.getOuterReferences());
            Set<Expression> pushableConjuncts = new LinkedHashSet<>();
            for (Expression conjunct : filter.getConjuncts()) {
                Set<Slot> inputSlots = conjunct.getInputSlots();
                boolean referencesParent = state.getOuterReferences().stream().anyMatch(inputSlots::contains);
                if (referencesParent && availableOnNestedLeft.containsAll(inputSlots)
                        && !conjunct.containsNondeterministic()
                        && !conjunct.containsType(NoneMovableFunction.class)) {
                    pushableConjuncts.add(conjunct);
                }
            }
            if (!pushableConjuncts.isEmpty()) {
                LogicalFilter<Plan> pushedFilter = new LogicalFilter<>(
                        ImmutableSet.copyOf(pushableConjuncts), (LogicalPlan) nestedApply.left());
                LogicalPlan rebuiltChild = (LogicalPlan) nestedApply.withChildren(
                        ImmutableList.of(pushedFilter, nestedApply.right()));
                for (int i = unaryNodesAboveApply.size() - 1; i >= 0; i--) {
                    rebuiltChild = (LogicalPlan) unaryNodesAboveApply.get(i).withChildren(
                            ImmutableList.of(rebuiltChild));
                }
                childPlan = rebuiltChild;
                remainingConjuncts.removeAll(pushableConjuncts);
            }
        }
        Optional<UnnestingRewriteResult> child = rewriteWithBinding(
                childPlan, state, index);
        if (!child.isPresent()) {
            return Optional.empty();
        }
        if (remainingConjuncts.isEmpty()) {
            return child;
        }
        Map<Expression, Expression> replacements = expressionReplacements(child.get().getReplacements());
        Set<Expression> conjuncts = new LinkedHashSet<>();
        for (Expression conjunct : remainingConjuncts) {
            conjuncts.add(ExpressionUtils.replace(conjunct, replacements));
        }
        LogicalFilter<Plan> rewritten = filter.withConjunctsAndChild(
                ImmutableSet.copyOf(conjuncts), child.get().getPlan());
        return Optional.of(new UnnestingRewriteResult(rewritten, child.get().getReplacements()));
    }

    private Optional<UnnestingRewriteResult> rewriteProject(
            LogicalProject<?> project, UnnestingState state, CorrelationPlanIndex index) {
        if (project.isDistinct() || !expressionsAreRepeatable(project)) {
            return Optional.empty();
        }
        Optional<UnnestingRewriteResult> child = rewriteWithBinding(
                (LogicalPlan) project.child(), state, index);
        if (!child.isPresent()) {
            return Optional.empty();
        }
        Map<Expression, Expression> replacements = expressionReplacements(child.get().getReplacements());
        List<NamedExpression> projects = new ArrayList<>();
        for (NamedExpression expression : project.getProjects()) {
            projects.add((NamedExpression) ExpressionUtils.replace(expression, replacements));
        }
        appendBindingOutputs(projects, state, child.get().getReplacements());
        LogicalProject<Plan> rewritten = project.withProjectsAndChild(projects, child.get().getPlan());
        return Optional.of(new UnnestingRewriteResult(rewritten,
                remapToOutputs(child.get().getReplacements(), rewritten)));
    }

    private Optional<UnnestingRewriteResult> rewriteAggregate(
            LogicalAggregate<?> aggregate, UnnestingState state, CorrelationPlanIndex index) {
        if (!isSupportedGlobalAggregate(aggregate) || !expressionsAreRepeatable(aggregate)) {
            return Optional.empty();
        }
        Optional<UnnestingRewriteResult> child = rewriteWithBinding(
                (LogicalPlan) aggregate.child(), state, index);
        if (!child.isPresent()) {
            return Optional.empty();
        }
        Map<Expression, Expression> replacements = expressionReplacements(child.get().getReplacements());
        List<Expression> groupBy = new ArrayList<>();
        for (Expression expression : aggregate.getGroupByExpressions()) {
            groupBy.add(ExpressionUtils.replace(expression, replacements));
        }
        List<NamedExpression> outputs = new ArrayList<>();
        for (NamedExpression expression : aggregate.getOutputExpressions()) {
            outputs.add((NamedExpression) ExpressionUtils.replace(expression, replacements));
        }
        appendBindingExpressions(groupBy, state, child.get().getReplacements());
        appendBindingOutputs(outputs, state, child.get().getReplacements());
        LogicalAggregate<Plan> rewritten = aggregate.withChildGroupByAndOutput(
                groupBy, outputs, child.get().getPlan());
        return Optional.of(new UnnestingRewriteResult(rewritten,
                remapToOutputs(child.get().getReplacements(), rewritten)));
    }

    private Optional<UnnestingRewriteResult> rewriteNestedApply(
            LogicalApply<?, ?> apply, UnnestingState parentState, CorrelationPlanIndex index) {
        Optional<CorrelationPlanIndex.ApplyInfo> applyInfo = index.getApplyInfo(apply);
        if (!applyInfo.isPresent() || !applyInfo.get().getParentApplyId().isPresent()
                || applyInfo.get().getParentApplyId().get() != parentState.getApply().getId()
                || !isEligibleApply(apply, index)) {
            return Optional.empty();
        }

        Optional<UnnestingRewriteResult> left = rewriteWithBinding(
                (LogicalPlan) apply.left(), parentState, index);
        if (!left.isPresent() || !isSupportedIndependentSubtree(left.get().getPlan())) {
            return Optional.empty();
        }
        Map<Slot, Slot> sourceRepresentatives = findSourceRepresentatives(
                apply.getCorrelationSlot(), left.get().getPlan(), left.get().getReplacements());
        if (sourceRepresentatives.size() != apply.getCorrelationSlot().size()) {
            return Optional.empty();
        }

        DomainSpec childDomain = new DomainSpec(left.get().getPlan(), sourceRepresentatives);
        UnnestingState childState = new UnnestingState(apply, childDomain, Optional.of(parentState));
        Optional<UnnestingRewriteResult> right = rewriteWithBinding(
                (LogicalPlan) apply.right(), childState, index);
        if (!right.isPresent()) {
            return Optional.empty();
        }
        return finishApply(apply, left.get().getPlan(), sourceRepresentatives,
                right.get(), left.get().getReplacements());
    }

    private Optional<UnnestingRewriteResult> finishApply(LogicalApply<?, ?> apply,
            LogicalPlan left, Map<Slot, Slot> sourceRepresentatives,
            UnnestingRewriteResult right, Map<Slot, Slot> parentReplacements) {
        if (right.getPlan().containsType(LogicalApply.class)) {
            return Optional.empty();
        }
        List<Expression> identity = new ArrayList<>();
        for (Slot outerReference : apply.getCorrelationSlot()) {
            Slot leftRepresentative = sourceRepresentatives.get(outerReference);
            Slot rightRepresentative = right.getReplacements().get(outerReference);
            if (leftRepresentative == null || rightRepresentative == null) {
                return Optional.empty();
            }
            identity.add(new NullSafeEqual(leftRepresentative, rightRepresentative));
        }
        LogicalJoin<LogicalPlan, LogicalPlan> join = new LogicalJoin<>(JoinType.LEFT_OUTER_JOIN,
                identity, ExpressionUtils.EMPTY_CONDITION, new DistributeHint(DistributeType.NONE),
                left, right.getPlan(), null);

        List<NamedExpression> boundaryOutputs = new ArrayList<>();
        for (Slot output : apply.getOutput()) {
            Slot rewrittenOutput = findOutput(join.getOutput(), output.getExprId());
            if (rewrittenOutput == null) {
                return Optional.empty();
            }
            boundaryOutputs.add(rewrittenOutput);
        }
        for (Slot parentOutput : parentReplacements.values()) {
            appendIfAbsent(boundaryOutputs, parentOutput);
        }
        LogicalProject<Plan> boundary = new LogicalProject<>(boundaryOutputs, join);
        return Optional.of(new UnnestingRewriteResult(boundary,
                remapToOutputs(parentReplacements, boundary)));
    }

    private boolean isEligibleApply(LogicalApply<?, ?> apply, CorrelationPlanIndex index) {
        Optional<CorrelationPlanIndex.ApplyInfo> info = index.getApplyInfo(apply);
        return apply.isScalar() && apply.isCorrelated() && !apply.isMarkJoin()
                && !apply.alreadyExecutedEliminateFilter()
                && info.isPresent() && !info.get().getAccessSites().isEmpty()
                && hasSupportedGlobalAggregateAtTop((LogicalPlan) apply.right());
    }

    private boolean hasSupportedGlobalAggregateAtTop(LogicalPlan plan) {
        while (plan instanceof LogicalProject) {
            if (((LogicalProject<?>) plan).isDistinct()) {
                return false;
            }
            plan = (LogicalPlan) plan.child(0);
        }
        return plan instanceof LogicalAggregate && isSupportedGlobalAggregate((LogicalAggregate<?>) plan);
    }

    private boolean isSupportedGlobalAggregate(LogicalAggregate<?> aggregate) {
        if (!aggregate.getGroupByExpressions().isEmpty() || aggregate.getSourceRepeat().isPresent()) {
            return false;
        }
        boolean foundAggregate = false;
        for (NamedExpression output : aggregate.getOutputExpressions()) {
            for (AggregateFunction function
                    : output.<AggregateFunction>collectToList(AggregateFunction.class::isInstance)) {
                foundAggregate = true;
                if (!(function instanceof Max || function instanceof Min || function instanceof Count)
                        || (function instanceof Count && function.isDistinct())) {
                    return false;
                }
            }
        }
        return foundAggregate;
    }

    private Map<Slot, Slot> findSourceRepresentatives(List<Slot> outerReferences,
            LogicalPlan source, Map<Slot, Slot> inheritedReplacements) {
        Map<Slot, Slot> representatives = new LinkedHashMap<>();
        for (Slot outerReference : outerReferences) {
            Slot sourceOutput = findOutput(source.getOutput(), outerReference.getExprId());
            if (sourceOutput != null) {
                representatives.put(outerReference, sourceOutput);
            } else if (inheritedReplacements.containsKey(outerReference)) {
                representatives.put(outerReference, inheritedReplacements.get(outerReference));
            }
        }
        return representatives;
    }

    private boolean containsOuterReference(LogicalPlan plan, Set<Slot> outerReferences) {
        for (Expression expression : plan.getExpressions()) {
            if (expression.anyMatch(expr -> expr instanceof Slot && outerReferences.contains(expr))) {
                return true;
            }
        }
        for (Plan child : plan.children()) {
            if (containsOuterReference((LogicalPlan) child, outerReferences)) {
                return true;
            }
        }
        return false;
    }

    private boolean isSupportedIndependentSubtree(LogicalPlan plan) {
        if (!expressionsAreRepeatable(plan)) {
            return false;
        }
        if (plan instanceof LogicalRelation || plan instanceof LogicalOneRowRelation) {
            return true;
        }
        if (plan instanceof LogicalProject || plan instanceof LogicalFilter
                || plan instanceof LogicalSubQueryAlias || plan instanceof LogicalAggregate) {
            return plan.arity() == 1 && isSupportedIndependentSubtree((LogicalPlan) plan.child(0));
        }
        if (plan instanceof LogicalJoin) {
            LogicalJoin<?, ?> join = (LogicalJoin<?, ?>) plan;
            return !join.isMarkJoin() && (join.getJoinType().isInnerJoin() || join.getJoinType().isCrossJoin())
                    && isSupportedIndependentSubtree((LogicalPlan) join.left())
                    && isSupportedIndependentSubtree((LogicalPlan) join.right());
        }
        return false;
    }

    private boolean expressionsAreRepeatable(Plan plan) {
        for (Expression expression : plan.getExpressions()) {
            if (expression.containsNondeterministic()
                    || expression.containsType(NoneMovableFunction.class)) {
                return false;
            }
        }
        return true;
    }

    private Map<Expression, Expression> expressionReplacements(Map<Slot, Slot> replacements) {
        Map<Expression, Expression> expressionMap = new HashMap<>();
        expressionMap.putAll(replacements);
        return expressionMap;
    }

    private void appendBindingExpressions(List<Expression> expressions, UnnestingState state,
            Map<Slot, Slot> replacements) {
        Set<ExprId> outputExprIds = new LinkedHashSet<>();
        for (Expression expression : expressions) {
            if (expression instanceof NamedExpression) {
                outputExprIds.add(((NamedExpression) expression).getExprId());
            }
        }
        for (Slot outerReference : state.getApply().getCorrelationSlot()) {
            Slot replacement = replacements.get(outerReference);
            if (replacement != null && outputExprIds.add(replacement.getExprId())) {
                expressions.add(replacement);
            }
        }
    }

    private void appendBindingOutputs(List<NamedExpression> outputs, UnnestingState state,
            Map<Slot, Slot> replacements) {
        for (Slot outerReference : state.getApply().getCorrelationSlot()) {
            Slot replacement = replacements.get(outerReference);
            if (replacement != null) {
                appendIfAbsent(outputs, replacement);
            }
        }
    }

    private void appendIfAbsent(List<NamedExpression> outputs, Slot slot) {
        for (NamedExpression output : outputs) {
            if (output.getExprId().equals(slot.getExprId())) {
                return;
            }
        }
        outputs.add(slot);
    }

    private Map<Slot, Slot> remapToOutputs(Map<Slot, Slot> replacements, LogicalPlan plan) {
        Map<Slot, Slot> remapped = new LinkedHashMap<>();
        for (Map.Entry<Slot, Slot> entry : replacements.entrySet()) {
            Slot output = findOutput(plan.getOutput(), entry.getValue().getExprId());
            if (output != null) {
                remapped.put(entry.getKey(), output);
            }
        }
        return remapped;
    }

    private Slot findOutput(List<Slot> outputs, ExprId exprId) {
        for (Slot output : outputs) {
            if (output.getExprId().equals(exprId)) {
                return output;
            }
        }
        return null;
    }

}
