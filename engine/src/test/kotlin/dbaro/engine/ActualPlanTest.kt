package dbaro.engine

import dbaro.engine.exec.ActualPlan
import dbaro.engine.exec.Session
import java.math.BigDecimal
import java.sql.Connection
import java.sql.DriverManager
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/** 실제 실행 계획: DBMS별 결과 변환(서버 없이)과, H2로 실행·되돌리기 */
class ActualPlanTest {
    private lateinit var connection: Connection
    private lateinit var session: Session

    @BeforeTest
    fun setUp() {
        connection = DriverManager.getConnection("jdbc:h2:mem:actual${System.nanoTime()}", "sa", "")
        connection.createStatement().use { s ->
            s.execute("CREATE TABLE EMP (ID INT PRIMARY KEY, NAME VARCHAR(40), SALARY INT)")
            s.execute("INSERT INTO EMP VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30)")
        }
        session = Session("t1", "generic", connection)
    }

    @AfterTest
    fun tearDown() {
        session.close()
    }

    private fun count(where: String = "1=1"): Int =
        connection.createStatement().use { s -> s.executeQuery("SELECT COUNT(*) FROM EMP WHERE $where").use { it.next(); it.getInt(1) } }

    @Suppress("UNCHECKED_CAST")
    private fun nodes(plan: Map<String, Any?>) = plan["nodes"] as List<Map<String, Any?>>

    @Test
    fun runsTheQueryAndKeepsTheData() {
        val plan = session.explain("SELECT * FROM EMP WHERE SALARY > 15", actual = true)
        assertEquals(true, plan["actual"])
        assertEquals(false, plan["rolledBack"])
        assertTrue((plan["text"] as String).contains("scanCount"), plan["text"].toString())
        assertTrue(plan["executionMs"] is Double)
        assertTrue(connection.autoCommit)
    }

    @Test
    fun changesAreRolledBack() {
        val plan = session.explain("UPDATE EMP SET SALARY = 0", actual = true)
        assertEquals(true, plan["rolledBack"])
        assertEquals(0, count("SALARY = 0"))
        session.explain("/* comment */ DELETE FROM EMP", actual = true)
        assertEquals(3, count())
        assertTrue(connection.autoCommit, "auto-commit comes back")
    }

    @Test
    fun manualCommitKeepsEarlierChanges() {
        session.setAutoCommit(false)
        connection.createStatement().use { it.execute("INSERT INTO EMP VALUES (4, 'd', 40)") }
        session.explain("DELETE FROM EMP", actual = true)
        assertEquals(4, count(), "only the analyzed statement is rolled back")
        connection.rollback()
        assertEquals(3, count())
        assertEquals(false, connection.autoCommit)
    }

    @Test
    fun onlyQueriesAndDml() {
        assertFailsWith<IllegalArgumentException> { session.explain("CREATE TABLE X (A INT)", actual = true) }
        assertFailsWith<IllegalArgumentException> { session.explain("BEGIN NULL; END;", actual = true) }
        assertEquals("SELECT", ActualPlan.statementKind("-- note\n (( select 1))"))
        assertEquals("WITH", ActualPlan.statementKind("/* a */with x as (select 1) select * from x"))
        assertFailsWith<UnsupportedOperationException> {
            ActualPlan.explain("tibero", connection, "SELECT 1 FROM DUAL", 0) {}
        }
    }

    @Test
    fun oracleStatisticsBecomeTree() {
        // V$SQL_PLAN_STATISTICS_ALL. Oracle JDBC는 NUMBER를 BigDecimal로 준다. LAST_ELAPSED_TIME은 마이크로초
        fun n(value: Number) = BigDecimal(value.toLong())
        val rows = listOf(
            mapOf("ID" to n(0), "PARENT_ID" to null, "OPERATION" to "SELECT STATEMENT", "COST" to n(5),
                "LAST_STARTS" to n(1), "LAST_OUTPUT_ROWS" to n(14000), "LAST_ELAPSED_TIME" to n(15250),
                "LAST_CR_BUFFER_GETS" to n(120), "LAST_CU_BUFFER_GETS" to n(0), "LAST_DISK_READS" to n(4)),
            mapOf("ID" to n(1), "PARENT_ID" to n(0), "OPERATION" to "NESTED LOOPS", "CARDINALITY" to n(1), "COST" to n(5),
                "LAST_STARTS" to n(1), "LAST_OUTPUT_ROWS" to n(14000), "LAST_ELAPSED_TIME" to n(15000),
                "LAST_CR_BUFFER_GETS" to n(120), "LAST_CU_BUFFER_GETS" to n(0), "LAST_DISK_READS" to n(4),
                // 작업 영역(바이트): 메모리 안에서 끝남
                "ESTIMATED_OPTIMAL_SIZE" to n(1452L * 1024), "ESTIMATED_ONEPASS_SIZE" to n(1452L * 1024), "LAST_MEMORY_USED" to n(1290L * 1024),
                "LAST_EXECUTION" to "OPTIMAL"),
            mapOf("ID" to n(2), "PARENT_ID" to n(1), "OPERATION" to "TABLE ACCESS", "OPTIONS" to "FULL", "OBJECT_OWNER" to "HR",
                "OBJECT_NAME" to "EMPLOYEES", "OBJECT_TYPE" to "TABLE", "CARDINALITY" to n(107), "COST" to n(3),
                "LAST_STARTS" to n(1), "LAST_OUTPUT_ROWS" to n(107), "LAST_ELAPSED_TIME" to n(300),
                "LAST_CR_BUFFER_GETS" to n(6), "LAST_CU_BUFFER_GETS" to n(0), "LAST_DISK_READS" to n(4),
                "FILTER_PREDICATES" to "\"SALARY\">1000"),
            mapOf("ID" to n(3), "PARENT_ID" to n(1), "OPERATION" to "INDEX", "OPTIONS" to "RANGE SCAN", "OBJECT_OWNER" to "HR",
                "OBJECT_NAME" to "EMP_DEPT_IX", "OBJECT_TYPE" to "INDEX", "CARDINALITY" to n(1), "COST" to n(1),
                "LAST_STARTS" to n(107), "LAST_OUTPUT_ROWS" to n(14000), "LAST_ELAPSED_TIME" to n(9000),
                "LAST_CR_BUFFER_GETS" to n(114), "LAST_CU_BUFFER_GETS" to n(0), "LAST_DISK_READS" to n(0),
                "ACCESS_PREDICATES" to "\"DEPT_ID\"=10",
                // 디스크를 한 번 거침: 임시 세그먼트(바이트)
                "LAST_MEMORY_USED" to n(2048L * 1024), "LAST_EXECUTION" to "1 PASS", "LAST_TEMPSEG_SIZE" to n(20 * 1024 * 1024)),
        )
        val plan = ActualPlan.oraclePlan(rows, "SQL_ID  abc, child number 0")
        // ALLSTATS LAST의 컬럼 (쓴 적 없는 Writes, Pstart·Pstop은 빠진다)
        assertEquals(
            listOf("Operation", "Object", "Starts", "E-Rows", "A-Rows", "A-Time (ms)", "Buffers", "Reads",
                "OMem", "1Mem", "Used-Mem", "Used-Tmp", "Cost", "Predicates"),
            plan["columns"],
        )
        val nodes = nodes(plan)
        assertEquals(listOf(-1L, 0L, 1L, 1L), nodes.map { it["parent"] })
        assertEquals(
            listOf("TABLE ACCESS FULL", "HR.EMPLOYEES", "1", "107", "107", "0.300", "6", "4", "", "", "", "", "3", "filter(\"SALARY\">1000)"),
            nodes[2]["cells"],
        )
        assertEquals(listOf("1452K", "1452K", "1290K (0)", ""), (nodes[1]["cells"] as List<*>).subList(8, 12))
        assertEquals(listOf("", "", "2048K (1)", "20M"), (nodes[3]["cells"] as List<*>).subList(8, 12))
        assertEquals(listOf("2048", "1161K", "73M", "3072M", "20G"),
            listOf(2048L, 1188864L, 73L * 1024 * 1024, 3L * 1024 * 1024 * 1024, 20L * 1024 * 1024 * 1024).map(ActualPlan::size))
        assertEquals("15.250", (nodes[0]["cells"] as List<*>)[5])
        assertEquals(mapOf("estimated" to 1L, "actual" to 14000L), nodes[1]["rows"])
        // E-Rows는 한 번 시작할 때의 예상: Starts를 곱한다
        assertEquals(mapOf("estimated" to 107L, "actual" to 14000L), nodes[3]["rows"])
        assertEquals(null, nodes[0]["rows"], "no estimate for the top step")
        assertEquals("INDEX", (nodes[3]["object"] as Map<*, *>)["type"])
        assertEquals("A-Rows", plan["actualRowsColumn"])
        assertEquals("SQL_ID  abc, child number 0", plan["text"])
    }

    @Test
    fun postgresAnalyzeBecomesTree() {
        val json = """
            [{"Plan": {"Node Type": "Hash Join", "Join Type": "Inner", "Startup Cost": 1.5, "Total Cost": 35.25,
              "Plan Rows": 5, "Plan Width": 44, "Actual Startup Time": 0.05, "Actual Total Time": 1.25,
              "Actual Rows": 1200, "Actual Loops": 1, "Hash Cond": "(e.dept_id = d.id)",
              "Shared Hit Blocks": 12, "Shared Read Blocks": 3, "Shared Dirtied Blocks": 0,
              "Plans": [
                {"Node Type": "Seq Scan", "Relation Name": "employees", "Schema": "public", "Alias": "e",
                 "Startup Cost": 0.0, "Total Cost": 22.0, "Plan Rows": 120, "Plan Width": 40,
                 "Actual Startup Time": 0.01, "Actual Total Time": 0.5, "Actual Rows": 1200, "Actual Loops": 1,
                 "Filter": "(salary > 1000)", "Rows Removed by Filter": 30, "Shared Hit Blocks": 10},
                {"Node Type": "Index Scan", "Relation Name": "departments", "Alias": "departments",
                 "Index Name": "departments_pkey", "Startup Cost": 0.15, "Total Cost": 8.17, "Plan Rows": 1, "Plan Width": 8,
                 "Actual Startup Time": 0.002, "Actual Total Time": 0.003, "Actual Rows": 1, "Actual Loops": 4},
                {"Node Type": "Seq Scan", "Relation Name": "x", "Alias": "x", "Startup Cost": 0.0, "Total Cost": 1.0,
                 "Plan Rows": 1, "Plan Width": 4, "Actual Startup Time": 0.0, "Actual Total Time": 0.0,
                 "Actual Rows": 0, "Actual Loops": 0}
              ]},
              "Planning Time": 0.321, "Triggers": [], "Execution Time": 1.5}]
        """.trimIndent()
        val plan = dbaro.engine.exec.Explain.postgresPlan(json, null, actual = true)
        assertEquals(
            listOf("Operation", "Object", "Rows", "Actual Rows", "Loops", "Time (ms)", "Cost", "Buffers", "Details"),
            plan["columns"],
        )
        val nodes = nodes(plan)
        assertEquals(
            listOf("Hash Join", "", "5", "1200", "1", "1.250", "1.50..35.25", "shared hit=12 read=3", "Hash Cond: (e.dept_id = d.id)"),
            nodes[0]["cells"],
        )
        assertEquals(
            listOf("Seq Scan", "public.employees e", "120", "1200", "1", "0.500", "0.00..22.00", "shared hit=10",
                "Filter: (salary > 1000); Rows Removed by Filter: 30"),
            nodes[1]["cells"],
        )
        // 반복 4번: 행 수와 시간은 전체
        assertEquals(listOf("1", "4", "4", "0.012"), (nodes[2]["cells"] as List<*>).subList(2, 6))
        assertEquals("never executed", (nodes[3]["cells"] as List<*>)[3])
        assertEquals(mapOf("estimated" to 5L, "actual" to 1200L), nodes[0]["rows"])
        assertEquals(mapOf("estimated" to 4L, "actual" to 4L), nodes[2]["rows"])
        assertEquals(null, nodes[3]["rows"])
        assertEquals(0.321, plan["planningMs"])
        assertEquals(1.5, plan["executionMs"])
        assertEquals(1200L, plan["resultRows"])
        assertEquals("Actual Rows", plan["actualRowsColumn"])
    }

    @Test
    fun mysqlAnalyzeTextBecomesTree() {
        val text = """
            -> Nested loop inner join  (cost=4.95 rows=9) (actual time=0.153..0.2 rows=9 loops=1)
                -> Filter: (e.dept_id is not null)  (cost=1.15 rows=9) (actual time=0.084..0.1 rows=9 loops=1)
                    -> Table scan on e  (cost=1.15 rows=9) (actual time=0.081..0.093 rows=9 loops=1)
                -> Single-row index lookup on d using PRIMARY (dept_id=e.dept_id)  (cost=0.31 rows=1) (actual time=0.008..0.01 rows=1 loops=9)
                -> Index lookup on x using `x_ix` (a=e.a)  (cost=0.25 rows=1) (never executed)
        """.trimIndent()
        val plan = ActualPlan.mysqlPlan(text)
        assertEquals(listOf("Operation", "Rows", "Actual Rows", "Loops", "Time (ms)", "Cost"), plan["columns"])
        val nodes = nodes(plan)
        assertEquals(listOf(-1L, 0L, 1L, 0L, 0L), nodes.map { it["parent"] })
        assertEquals(listOf("Nested loop inner join", "9", "9", "1", "0.200", "4.95"), nodes[0]["cells"])
        assertEquals(
            listOf("Single-row index lookup on d using PRIMARY (dept_id=e.dept_id)", "1", "9", "9", "0.090", "0.31"),
            nodes[3]["cells"],
        )
        assertEquals(mapOf("schema" to null, "name" to "d", "type" to "TABLE", "index" to "PRIMARY"), nodes[3]["object"])
        assertEquals(mapOf("schema" to null, "name" to "e", "type" to "TABLE", "index" to null), nodes[2]["object"])
        assertEquals(null, nodes[1]["object"], "a filter reads no table")
        assertEquals("x_ix", (nodes[4]["object"] as Map<*, *>)["index"])
        assertEquals("never executed", (nodes[4]["cells"] as List<*>)[2])
        assertEquals(mapOf("estimated" to 9L, "actual" to 9L), nodes[3]["rows"])
        assertEquals(9L, plan["resultRows"])
        assertEquals(text, plan["text"])
    }

    @Test
    fun mariadbAnalyzeKeepsTheTable() {
        val columns = listOf("id", "select_type", "table", "type", "key", "rows", "r_rows", "filtered", "r_filtered", "Extra")
        val rows = listOf(
            listOf("1", "SIMPLE", "e", "ALL", null, "10", "5000.00", "100.00", "2.00", "Using where"),
            listOf("1", "SIMPLE", "d", "eq_ref", "PRIMARY", "1", "1.00", "100.00", "100.00", null),
        )
        val plan = ActualPlan.mariadbPlan(columns, rows)
        assertEquals(columns, plan["columns"])
        val nodes = nodes(plan)
        assertEquals(mapOf("estimated" to 10L, "actual" to 5000L), nodes[0]["rows"])
        assertEquals("PRIMARY", (nodes[1]["object"] as Map<*, *>)["index"])
        assertEquals("r_rows", plan["actualRowsColumn"])
    }
}
