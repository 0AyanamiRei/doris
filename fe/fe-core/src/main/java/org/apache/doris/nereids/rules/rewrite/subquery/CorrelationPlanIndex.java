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

import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.logical.LogicalApply;

import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableSet;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;

/**
 * Immutable access index used by the holistic unnesting pass.
 *
 * <p>The first implementation indexes a plan tree. CTE and recursive edges are deliberately not traversed as
 * graph edges yet. The important 2025 contract already lives here: each dependent join is associated with its
 * parent dependent join and with every expression node in its RHS that accesses one of its outer slots.</p>
 */
public class CorrelationPlanIndex {
    private final Map<Integer, ApplyInfo> applyInfos;

    private CorrelationPlanIndex(Map<Integer, ApplyInfo> applyInfos) {
        this.applyInfos = applyInfos;
    }

    /** Build an index without changing the input plan. */
    public static CorrelationPlanIndex build(Plan root) {
        Map<Integer, MutableApplyInfo> mutableInfos = new HashMap<>();
        index(root, new ArrayList<>(), mutableInfos);
        Map<Integer, ApplyInfo> infos = new HashMap<>();
        for (Map.Entry<Integer, MutableApplyInfo> entry : mutableInfos.entrySet()) {
            MutableApplyInfo info = entry.getValue();
            infos.put(entry.getKey(), new ApplyInfo(info.applyId, info.parentApplyId,
                    info.outerReferences, ImmutableList.copyOf(info.accessSites)));
        }
        return new CorrelationPlanIndex(infos);
    }

    public Optional<ApplyInfo> getApplyInfo(LogicalApply<?, ?> apply) {
        return Optional.ofNullable(applyInfos.get(apply.getId()));
    }

    private static void index(Plan plan, List<LogicalApply<?, ?>> activeApplies,
            Map<Integer, MutableApplyInfo> infos) {
        registerAccess(plan, activeApplies, infos);
        if (plan instanceof LogicalApply) {
            LogicalApply<?, ?> apply = (LogicalApply<?, ?>) plan;
            Optional<Integer> parentApplyId = activeApplies.isEmpty()
                    ? Optional.empty() : Optional.of(activeApplies.get(activeApplies.size() - 1).getId());
            infos.put(apply.getId(), new MutableApplyInfo(apply.getId(), parentApplyId,
                    ImmutableSet.copyOf(apply.getCorrelationSlot())));

            index(apply.left(), activeApplies, infos);
            activeApplies.add(apply);
            index(apply.right(), activeApplies, infos);
            activeApplies.remove(activeApplies.size() - 1);
            return;
        }
        for (Plan child : plan.children()) {
            index(child, activeApplies, infos);
        }
    }

    private static void registerAccess(Plan plan, List<LogicalApply<?, ?>> activeApplies,
            Map<Integer, MutableApplyInfo> infos) {
        if (activeApplies.isEmpty()) {
            return;
        }
        Set<Slot> inputSlots = new LinkedHashSet<>();
        for (Expression expression : plan.getExpressions()) {
            inputSlots.addAll(expression.getInputSlots());
        }
        if (inputSlots.isEmpty()) {
            return;
        }
        for (LogicalApply<?, ?> apply : activeApplies) {
            MutableApplyInfo info = infos.get(apply.getId());
            if (info != null && info.outerReferences.stream().anyMatch(inputSlots::contains)) {
                info.accessSites.add(plan);
            }
        }
    }

    /** Indexed information for one LogicalApply. */
    public static class ApplyInfo {
        private final int applyId;
        private final Optional<Integer> parentApplyId;
        private final Set<Slot> outerReferences;
        private final List<Plan> accessSites;

        private ApplyInfo(int applyId, Optional<Integer> parentApplyId,
                Set<Slot> outerReferences, List<Plan> accessSites) {
            this.applyId = applyId;
            this.parentApplyId = parentApplyId;
            this.outerReferences = outerReferences;
            this.accessSites = accessSites;
        }

        public int getApplyId() {
            return applyId;
        }

        public Optional<Integer> getParentApplyId() {
            return parentApplyId;
        }

        public Set<Slot> getOuterReferences() {
            return outerReferences;
        }

        public List<Plan> getAccessSites() {
            return accessSites;
        }
    }

    private static class MutableApplyInfo {
        private final int applyId;
        private final Optional<Integer> parentApplyId;
        private final Set<Slot> outerReferences;
        private final Set<Plan> accessSites = new LinkedHashSet<>();

        private MutableApplyInfo(int applyId, Optional<Integer> parentApplyId, Set<Slot> outerReferences) {
            this.applyId = applyId;
            this.parentApplyId = parentApplyId;
            this.outerReferences = outerReferences;
        }
    }
}
