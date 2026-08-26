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

import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.logical.LogicalPlan;

import com.google.common.collect.ImmutableMap;

import java.util.Map;

/** Rewritten fragment plus the binding representation visible in its output. */
public class UnnestingRewriteResult {
    private final LogicalPlan plan;
    private final Map<Slot, Slot> replacements;

    public UnnestingRewriteResult(LogicalPlan plan, Map<Slot, Slot> replacements) {
        this.plan = plan;
        this.replacements = ImmutableMap.copyOf(replacements);
    }

    public LogicalPlan getPlan() {
        return plan;
    }

    public Map<Slot, Slot> getReplacements() {
        return replacements;
    }
}
