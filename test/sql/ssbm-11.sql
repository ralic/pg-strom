SET search_path = pgstrom_regress,public;
SET pg_strom.debug_kernel_source = off;
-- Q1_1
SET pg_strom.enabled = off;
select sum(lo_extendedprice*lo_discount) as revenue
from lineorder,date1
where lo_orderdate = d_datekey
and d_year = 1993
and lo_discount between 1 and 3
and lo_quantity < 25;

SET pg_strom.enabled = on;
select sum(lo_extendedprice*lo_discount) as revenue
from lineorder,date1
where lo_orderdate = d_datekey
and d_year = 1993
and lo_discount between 1 and 3
and lo_quantity < 25;
