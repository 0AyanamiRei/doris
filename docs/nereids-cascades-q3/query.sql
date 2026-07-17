SELECT
    c.c_custkey,
    o.o_orderkey,
    l.l_linenumber,
    o.o_orderdate
FROM customer c
JOIN orders o
  ON c.c_custkey = o.o_custkey
JOIN lineitem l
  ON o.o_orderkey = l.l_orderkey
WHERE c.c_mktsegment = 'BUILDING'
  AND o.o_orderdate < DATE '1995-03-15'
  AND l.l_shipdate > DATE '1995-03-15'
ORDER BY o.o_orderdate, o.o_orderkey, l.l_linenumber;
