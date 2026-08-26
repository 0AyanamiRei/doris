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

import org.apache.doris.nereids.trees.copier.DeepCopierContext;
import org.apache.doris.nereids.trees.copier.LogicalPlanDeepCopier;
import org.apache.doris.nereids.trees.expressions.ExprId;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.NamedExpression;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.logical.LogicalAggregate;
import org.apache.doris.nereids.trees.plans.logical.LogicalPlan;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Lazy description of a duplicate-free outer binding domain. */
public class DomainSpec {
    private final LogicalPlan source;
    private final Map<Slot, Slot> outerToSourceRepresentative;

    public DomainSpec(LogicalPlan source, Map<Slot, Slot> outerToSourceRepresentative) {
        this.source = source;
        this.outerToSourceRepresentative = ImmutableMap.copyOf(outerToSourceRepresentative);
        Preconditions.checkArgument(!outerToSourceRepresentative.isEmpty(), "Domain keys must not be empty");
    }

    /**
     * Materialize the domain as a DISTINCT aggregate over a fresh deep copy of the source.
     *
     * <p>The fresh copy prevents ExprId aliasing when the original source remains on the Apply left side.</p>
     */
    public DomainInstance materialize() {
        DeepCopierContext copierContext = new DeepCopierContext();
        LogicalPlan copiedSource = LogicalPlanDeepCopier.INSTANCE.deepCopy(source, copierContext);
        Map<Slot, Slot> outerToDomain = new LinkedHashMap<>();
        ImmutableList.Builder<Expression> groupBy = ImmutableList.builder();
        ImmutableList.Builder<NamedExpression> outputs = ImmutableList.builder();
        for (Map.Entry<Slot, Slot> entry : outerToSourceRepresentative.entrySet()) {
            ExprId copiedExprId = copierContext.exprIdReplaceMap.get(entry.getValue().getExprId());
            Preconditions.checkState(copiedExprId != null,
                    "Missing copied ExprId for domain source slot %s", entry.getValue());
            Slot copiedSlot = findOutput(copiedSource.getOutput(), copiedExprId);
            groupBy.add(copiedSlot);
            outputs.add(copiedSlot);
            outerToDomain.put(entry.getKey(), copiedSlot);
        }
        LogicalAggregate<LogicalPlan> domain = new LogicalAggregate<>(
                groupBy.build(), outputs.build(), copiedSource);
        return new DomainInstance(domain, outerToDomain);
    }

    private static Slot findOutput(List<Slot> outputs, ExprId exprId) {
        for (Slot output : outputs) {
            if (output.getExprId().equals(exprId)) {
                return output;
            }
        }
        throw new IllegalStateException("Copied domain source does not output ExprId " + exprId);
    }

    /** Materialized domain and the fresh slot representing every original outer key. */
    public static class DomainInstance {
        private final LogicalPlan plan;
        private final Map<Slot, Slot> outerToDomain;

        private DomainInstance(LogicalPlan plan, Map<Slot, Slot> outerToDomain) {
            this.plan = plan;
            this.outerToDomain = ImmutableMap.copyOf(outerToDomain);
        }

        public LogicalPlan getPlan() {
            return plan;
        }

        public Map<Slot, Slot> getOuterToDomain() {
            return outerToDomain;
        }
    }
}
