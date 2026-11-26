package dbaro.engine

import dbaro.engine.exec.Connector
import dbaro.engine.exec.Explain
import java.math.BigDecimal
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/** 실제 Oracle·PostgreSQL 서버 없이 실행 계획 변환과 접속 속성을 확인한다. */
class ExplainTest {
    @Test
    fun oraclePlanTableBecomesTree() {
        // Oracle JDBC는 NUMBER를 BigDecimal로 준다.
        val rows = listOf(
            mapOf("ID" to BigDecimal(0), "PARENT_ID" to null, "OPERATION" to "SELECT STATEMENT", "OPTIONS" to null,
                "COST" to BigDecimal(3), "CARDINALITY" to BigDecimal(1), "BYTES" to BigDecimal(39), "TIME" to BigDecimal(1)),
            mapOf("ID" to BigDecimal(1), "PARENT_ID" to BigDecimal(0), "OPERATION" to "TABLE ACCESS", "OPTIONS" to "BY INDEX ROWID",
                "OBJECT_OWNER" to "HR", "OBJECT_NAME" to "EMPLOYEES", "COST" to BigDecimal(3), "CARDINALITY" to BigDecimal(1),
                "BYTES" to BigDecimal(39), "TIME" to BigDecimal(3725)),
            mapOf("ID" to BigDecimal(2), "PARENT_ID" to BigDecimal(1), "OPERATION" to "INDEX", "OPTIONS" to "UNIQUE SCAN",
                "OBJECT_OWNER" to "HR", "OBJECT_NAME" to "EMP_EMP_ID_PK", "COST" to BigDecimal(1), "CARDINALITY" to BigDecimal(1),
                "ACCESS_PREDICATES" to "\"EMPLOYEE_ID\"=100"),
        )
        val plan = Explain.oraclePlan(rows, "Plan hash value: 1")
        assertEquals(listOf("Operation", "Object", "Rows", "Bytes", "Cost", "Time", "Predicates"), plan["columns"])
        @Suppress("UNCHECKED_CAST")
        val nodes = plan["nodes"] as List<Map<String, Any?>>
        assertEquals(listOf(-1L, 0L, 1L), nodes.map { it["parent"] })
        assertEquals(listOf("TABLE ACCESS BY INDEX ROWID", "HR.EMPLOYEES", "1", "39", "3", "01:02:05", ""), nodes[1]["cells"])
        assertEquals("access(\"EMPLOYEE_ID\"=100)", (nodes[2]["cells"] as List<*>).last())
        assertEquals(mapOf("schema" to "HR", "name" to "EMPLOYEES", "type" to "TABLE"), nodes[1]["object"])
        assertEquals("INDEX", (nodes[2]["object"] as Map<*, *>)["type"])
        assertEquals("Plan hash value: 1", plan["text"])
    }

    @Test
    fun postgresJsonBecomesTree() {
        val json = """
            [{"Plan": {"Node Type": "Hash Join", "Join Type": "Left", "Startup Cost": 1.5, "Total Cost": 35.25,
              "Plan Rows": 120, "Plan Width": 44, "Hash Cond": "(e.dept_id = d.id)",
              "Plans": [
                {"Node Type": "Seq Scan", "Parent Relationship": "Outer", "Relation Name": "employees", "Schema": "public",
                 "Alias": "e", "Startup Cost": 0.0, "Total Cost": 22.0, "Plan Rows": 120, "Plan Width": 40,
                 "Filter": "(salary > 1000::numeric)"},
                {"Node Type": "Index Scan", "Relation Name": "departments", "Alias": "departments",
                 "Index Name": "departments_pkey", "Startup Cost": 0.15, "Total Cost": 8.17, "Plan Rows": 1, "Plan Width": 8,
                 "Sort Key": ["d.name", "d.id"]}
              ]}}]
        """.trimIndent()
        val plan = Explain.postgresPlan(json, "Hash Left Join  (cost=1.50..35.25 rows=120 width=44)")
        assertEquals(listOf("Operation", "Object", "Rows", "Width", "Cost", "Details"), plan["columns"])
        @Suppress("UNCHECKED_CAST")
        val nodes = plan["nodes"] as List<Map<String, Any?>>
        assertEquals(3, nodes.size)
        assertEquals(listOf("Hash Join (Left)", "", "120", "44", "1.50..35.25", "Hash Cond: (e.dept_id = d.id)"), nodes[0]["cells"])
        assertEquals(listOf("Seq Scan", "public.employees e", "120", "40", "0.00..22.00", "Filter: (salary > 1000::numeric)"), nodes[1]["cells"])
        assertEquals("departments using departments_pkey", (nodes[2]["cells"] as List<*>)[1])
        assertEquals("departments_pkey", (nodes[2]["object"] as Map<*, *>)["index"])
        assertEquals(null, nodes[0]["object"])
        assertEquals("Sort Key: d.name, d.id", (nodes[2]["cells"] as List<*>)[5])
        assertEquals(listOf(-1L, 0L, 0L), nodes.map { it["parent"] })
    }

    @Test
    fun oracleSessionShowsDbaroAsProgram() {
        val properties = Connector.connectionProperties("oracle", mapOf("user" to "hr", "password" to "x"), 10)
        assertEquals("DBaro", properties.getProperty("v\$session.program"))
        assertEquals("hr", properties.getProperty("user"))
        assertFalse(Connector.connectionProperties("postgres", emptyMap(), 10).containsKey("v\$session.program"))
        // PostgreSQL은 pg_stat_activity.application_name
        assertEquals("DBaro", Connector.connectionProperties("postgres", emptyMap(), 10).getProperty("ApplicationName"))
        assertTrue(Connector.connectionProperties("oracle", emptyMap(), 10).getProperty("remarksReporting") == "true")
    }
    @Test
    fun mysqlAliasesBecomeTableNames() {
        val aliases = Explain.tableAliases(
            "SELECT e.emp_name, o.amount /* FROM x y */ FROM emp e JOIN `orders` AS o ON o.emp_id = e.emp_id " +
                "LEFT JOIN dbaro_hr.region r ON r.region_id = 1, product p " +
                "WHERE o.status = 'FROM z w' AND EXISTS (SELECT 1 FROM order_item i WHERE i.order_id = o.order_id)",
        )
        assertEquals("emp", aliases["e"]?.second)
        assertEquals("orders", aliases["o"]?.second)
        assertEquals("dbaro_hr" to "region", aliases["r"])
        assertEquals("dbaro_hr" to "region", aliases["region"])
        assertEquals("product", aliases["p"]?.second)
        assertEquals("order_item", aliases["i"]?.second)
        assertFalse("y" in aliases || "w" in aliases || "amount" in aliases, aliases.toString())

        val plan = Explain.withTableNames(
            "SELECT * FROM orders o",
            mapOf("nodes" to listOf(mapOf("object" to mapOf("schema" to null, "name" to "o", "type" to "TABLE", "index" to "orders_status_ix")))),
        )
        @Suppress("UNCHECKED_CAST")
        val target = (plan["nodes"] as List<Map<String, Any?>>).single()["object"] as Map<String, Any?>
        assertEquals("orders", target["name"])
        assertEquals("orders_status_ix", target["index"])
    }
}
