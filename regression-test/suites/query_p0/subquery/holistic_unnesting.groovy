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

suite("holistic_unnesting") {
    sql "set enable_holistic_subquery_unnesting = true"

    sql "drop table if exists hu_outer"
    sql "drop table if exists hu_inner"
    sql "drop table if exists hu_dim"
    sql "drop table if exists hu_deep"

    sql """
        create table hu_outer (
            id int,
            k1 int,
            bound_v int,
            flag int
        ) distributed by hash(id) buckets 1
        properties("replication_num" = "1")
    """
    sql """
        create table hu_inner (
            k1 int,
            k2 int,
            v int,
            dim_id int,
            flag int
        ) distributed by hash(k1) buckets 1
        properties("replication_num" = "1")
    """
    sql """
        create table hu_dim (
            dim_id int,
            threshold_v int
        ) distributed by hash(dim_id) buckets 1
        properties("replication_num" = "1")
    """
    sql """
        create table hu_deep (
            k1 int,
            k2 int,
            v int
        ) distributed by hash(k1) buckets 1
        properties("replication_num" = "1")
    """

    sql "insert into hu_outer values (1, 1, 10, 0), (2, 1, 4, 1), (3, 2, 100, 0), (4, 3, 5, 1), (5, null, 10, 0)"
    sql "insert into hu_inner values (1, 10, 3, 100, 0), (1, 10, 7, 101, 1), (1, 20, 12, 100, 0), (2, 10, 20, 100, 0), (2, 30, null, 102, 0), (null, 40, 8, 100, 0)"
    sql "insert into hu_dim values (100, 2), (101, 6), (102, 1)"
    sql "insert into hu_deep values (1, 10, 5), (1, 10, 9), (1, 20, 15), (2, 10, 25), (2, 30, 0)"

    order_qt_max_non_equality """
        select o.id, (select max(i.v) from hu_inner i
            where i.k1 = o.k1 and i.v < o.bound_v) as result
        from hu_outer o
        order by o.id
    """

    order_qt_min_or_predicate """
        select o.id, (select min(i.v) from hu_inner i
            where i.k1 = o.k1 and (i.v < o.bound_v or i.flag = o.flag)) as result
        from hu_outer o
        order by o.id
    """

    order_qt_count_empty_binding """
        select o.id, (select count(*) from hu_inner i
            where i.k1 = o.k1 and i.v < o.bound_v) as result
        from hu_outer o
        order by o.id
    """

    order_qt_inner_equal_join """
        select o.id, (select max(i.v) from hu_inner i
            inner join hu_dim d on i.dim_id = d.dim_id
            where i.k1 = o.k1 and d.threshold_v < o.bound_v) as result
        from hu_outer o
        order by o.id
    """

    order_qt_nested_reachable_domain """
        select o.id, (select max((select max(d.v) from hu_deep d
                where d.k1 = o.k1 and d.k2 = i.k2))
            from hu_inner i where i.k1 = o.k1) as result
        from hu_outer o
        order by o.id
    """
}
