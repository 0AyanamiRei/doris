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

package org.apache.doris.nereids.rules.rewrite;

import org.apache.doris.nereids.trees.expressions.NullSafeEqual;
import org.apache.doris.nereids.trees.expressions.StatementScopeIdGenerator;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.logical.LogicalAggregate;
import org.apache.doris.nereids.trees.plans.logical.LogicalApply;
import org.apache.doris.nereids.trees.plans.logical.LogicalJoin;
import org.apache.doris.nereids.util.PlanChecker;
import org.apache.doris.utframe.TestWithFeService;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.List;

class HolisticApplyEliminatorTest extends TestWithFeService {

    @Override
    protected void runBeforeAll() throws Exception {
        createDatabase("holistic_unnesting_test");
        connectContext.setDatabase("holistic_unnesting_test");
        connectContext.getSessionVariable().setDisableNereidsRules("PRUNE_EMPTY_PARTITION");
        createTables(
                "CREATE TABLE hu_outer (id INT, k1 INT, bound_v INT, flag INT) "
                        + "DISTRIBUTED BY HASH(id) BUCKETS 1 PROPERTIES('replication_num'='1');",
                "CREATE TABLE hu_inner (k1 INT, k2 INT, v INT, dim_id INT, flag INT) "
                        + "DISTRIBUTED BY HASH(k1) BUCKETS 1 PROPERTIES('replication_num'='1');",
                "CREATE TABLE hu_dim (dim_id INT, threshold_v INT) "
                        + "DISTRIBUTED BY HASH(dim_id) BUCKETS 1 PROPERTIES('replication_num'='1');",
                "CREATE TABLE hu_deep (k1 INT, k2 INT, v INT) "
                        + "DISTRIBUTED BY HASH(k1) BUCKETS 1 PROPERTIES('replication_num'='1');"
        );
    }

    @Override
    protected void runBeforeEach() throws Exception {
        StatementScopeIdGenerator.clear();
        connectContext.getSessionVariable().setEnableHolisticSubqueryUnnesting(true);
    }

    @Test
    void testMaxMinCountWithGeneralCorrelatedPredicates() {
        assertDomainPlan("SELECT o.id, (SELECT MAX(i.v) FROM hu_inner i "
                + "WHERE i.k1 = o.k1 AND i.v < o.bound_v) FROM hu_outer o");
        assertDomainPlan("SELECT o.id, (SELECT MIN(i.v) FROM hu_inner i "
                + "WHERE i.k1 = o.k1 AND (i.v < o.bound_v OR i.flag = o.flag)) FROM hu_outer o");
        assertDomainPlan("SELECT o.id, (SELECT COUNT(*) FROM hu_inner i "
                + "WHERE i.k1 = o.k1 AND i.v < o.bound_v) FROM hu_outer o");
    }

    @Test
    void testIndependentInnerEqualJoinBelowCorrelatedFilter() {
        Plan plan = rewrite("SELECT o.id, (SELECT MAX(i.v) FROM hu_inner i "
                + "INNER JOIN hu_dim d ON i.dim_id = d.dim_id "
                + "WHERE i.k1 = o.k1 AND d.threshold_v < o.bound_v) FROM hu_outer o");
        assertNoApplyAndNullSafeJoinBack(plan);
        Assertions.assertTrue(plan.<LogicalJoin<?, ?>>collectToList(LogicalJoin.class::isInstance).stream()
                .anyMatch(join -> join.getJoinType().isInnerJoin()), plan.treeString());
    }

    @Test
    void testReachableCompoundDomainForNestedApply() {
        Plan plan = rewrite("SELECT o.id, (SELECT MAX((SELECT MAX(d.v) FROM hu_deep d "
                + "WHERE d.k1 = o.k1 AND d.k2 = i.k2)) FROM hu_inner i "
                + "WHERE i.k1 = o.k1) FROM hu_outer o");
        assertNoApplyAndNullSafeJoinBack(plan);
        List<LogicalAggregate<?>> aggregates = plan
                .<LogicalAggregate<?>>collectToList(LogicalAggregate.class::isInstance);
        Assertions.assertTrue(aggregates.stream()
                .anyMatch(aggregate -> aggregate.getGroupByExpressions().size() >= 2), plan.treeString());
        Assertions.assertTrue(plan.<LogicalJoin<?, ?>>collectToList(LogicalJoin.class::isInstance).stream()
                .anyMatch(join -> join.getHashJoinConjuncts().stream()
                        .filter(NullSafeEqual.class::isInstance).count() >= 2), plan.treeString());
        Assertions.assertFalse(plan.<LogicalJoin<?, ?>>collectToList(LogicalJoin.class::isInstance).stream()
                .anyMatch(join -> join.getJoinType().isCrossJoin()), plan.treeString());
    }

    private void assertDomainPlan(String sql) {
        Plan plan = rewrite(sql);
        assertNoApplyAndNullSafeJoinBack(plan);
        long aggregates = plan.<LogicalAggregate<?>>collectToList(LogicalAggregate.class::isInstance).size();
        Assertions.assertTrue(aggregates >= 2, plan.treeString());
    }

    private void assertNoApplyAndNullSafeJoinBack(Plan plan) {
        Assertions.assertFalse(plan.containsType(LogicalApply.class), plan.treeString());
        Assertions.assertTrue(plan.<LogicalJoin<?, ?>>collectToList(LogicalJoin.class::isInstance).stream()
                .flatMap(join -> join.getHashJoinConjuncts().stream())
                .anyMatch(NullSafeEqual.class::isInstance), plan.treeString());
    }

    private Plan rewrite(String sql) {
        return PlanChecker.from(connectContext)
                .analyze(sql)
                .rewrite()
                .getPlan();
    }
}
