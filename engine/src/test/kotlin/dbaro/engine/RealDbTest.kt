package dbaro.engine

import dbaro.engine.driver.DriverRegistry
import dbaro.engine.exec.Connector
import dbaro.engine.exec.Ddl
import dbaro.engine.exec.PlanObject
import dbaro.engine.exec.Session
import org.junit.jupiter.api.Assumptions.assumeTrue
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertFailsWith
import kotlin.test.assertNotNull
import kotlin.test.assertTrue
import kotlin.test.fail

/*
 * 실제 DB(dev/docker-compose.yml + dev/seed)로 엔진 기능을 확인한다.
 * dev/.env 가 없거나 그 DB가 localhost에서 돌고 있지 않으면 건너뛴다 (보통의 gradlew test는 그대로 통과).
 * DBARO_REAL_DB=off 면 항상 건너뛴다. 드라이버는 앱과 같은 ~/.dbaro/drivers 를 쓴다 (없으면 내려받는다).
 * 결과는 build/real-db/<type>.txt 에도 남겨 눈으로 확인할 수 있게 한다.
 */

class OracleRealDbTest : RealDbTest("oracle", 1521, "FREEPDB1", hrSchema = "DBARO_HR")
class PostgresRealDbTest : RealDbTest("postgres", 5432, "dbaro", hrSchema = "hr")
class MySqlRealDbTest : RealDbTest("mysql", 3307, "dbaro", hrSchema = "dbaro_hr")
class MariaDbRealDbTest : RealDbTest("mariadb", 3306, "dbaro", hrSchema = "dbaro_hr")

@Suppress("UNCHECKED_CAST")
abstract class RealDbTest(
    private val type: String,
    private val port: Int,
    private val database: String,
    private val hrSchema: String,
) {
    companion object {
        private val env: Map<String, String>? by lazy { loadEnv() }
        private val drivers by lazy { DriverRegistry(Paths.get(System.getProperty("user.home"), ".dbaro", "drivers")) }

        private fun loadEnv(): Map<String, String>? {
            if (System.getenv("DBARO_REAL_DB").equals("off", ignoreCase = true)) return null
            var dir: Path? = Paths.get(System.getProperty("user.dir")).toAbsolutePath()
            while (dir != null) {
                val file = dir.resolve("dev").resolve(".env")
                if (Files.isRegularFile(file)) {
                    return Files.readAllLines(file).map { it.trim() }
                        .filter { it.isNotEmpty() && !it.startsWith("#") && it.contains('=') }
                        .associate { it.substringBefore('=').trim() to it.substringAfter('=').trim() }
                }
                dir = dir.parent
            }
            return null
        }

        private fun reachable(port: Int): Boolean = runCatching {
            Socket().use { it.connect(InetSocketAddress("localhost", port), 500) }
        }.isSuccess
    }

    private val oracle = type == "oracle"
    private val catalogs = type == "mysql" || type == "mariadb"
    private lateinit var session: Session
    private lateinit var schema: String
    private val report = StringBuilder()

    /** 이름 그대로 쓰는 DB(Oracle은 대문자로 저장) */
    private fun n(name: String) = if (oracle) name.uppercase() else name

    @BeforeTest
    fun open() {
        val env = env
        assumeTrue(env != null, "dev/.env not found (or DBARO_REAL_DB=off)")
        assumeTrue(reachable(port), "$type is not running on localhost:$port")
        session = connect(autoCommit = true)
        schema = if (oracle) env!!.getValue("DB_USER").uppercase() else if (catalogs) database else "public"
    }

    @AfterTest
    fun close() {
        if (::session.isInitialized) session.close()
        if (report.isNotEmpty()) {
            val file = Paths.get("build", "real-db", "$type.txt")
            Files.createDirectories(file.parent)
            Files.writeString(file, report.toString(), Charsets.UTF_8,
                java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.APPEND)
        }
    }

    private fun connect(autoCommit: Boolean): Session {
        val env = env!!
        val url = when (type) {
            "oracle" -> "jdbc:oracle:thin:@//localhost:$port/$database"
            "postgres" -> "jdbc:postgresql://localhost:$port/$database"
            else -> "jdbc:$type://localhost:$port/$database"
        }
        return Connector.open(
            drivers,
            mapOf("type" to type, "url" to url, "user" to env["DB_USER"], "password" to env["DB_PASSWORD"], "autoCommit" to autoCommit),
            "real-$type-${System.nanoTime()}",
        )
    }

    private fun dump(title: String, value: Any?) {
        report.append("===== ").append(title).append('\n')
        report.append(if (value is String) value else pretty(value)).append("\n\n")
    }

    private fun pretty(value: Any?, indent: String = ""): String = when (value) {
        is Map<*, *> -> value.entries.joinToString("\n") { (k, v) ->
            if (v is Map<*, *> || (v is List<*> && v.any { it is Map<*, *> || it is List<*> })) "$indent$k:\n" + pretty(v, "$indent  ")
            else "$indent$k: ${short(v)}"
        }
        is List<*> -> value.joinToString("\n") { v ->
            if (v is Map<*, *>) "$indent-\n" + pretty(v, "$indent  ") else "$indent- ${short(v)}"
        }
        else -> indent + short(value)
    }

    private fun short(v: Any?): String = v.toString().let { if (it.length > 400) it.take(400) + "...(${it.length})" else it }

    private fun query(sql: String): List<List<Any?>> = session.execute(sql, 1000, 0)["rows"] as List<List<Any?>>

    private fun scalar(sql: String): Any? = query(sql).first().first()

    /** 숫자 값 (NUMERIC·DECIMAL·Oracle NUMBER는 자릿수를 지키려고 문자열로 온다) */
    private fun num(value: Any?): Double = value.toString().toDouble()

    private fun request(name: String, schema: String? = null) = Ddl.Request(schema, n(name), false, false, null)

    // ---------------------------------------------------------------------

    @Test
    fun metadata() {
        val schemas = session.schemas()
        dump("schemas", schemas)
        val names = schemas["names"] as List<String>
        assertContains(names, schema)
        assertContains(names, hrSchema)

        val objects = (session.objects(schema)["objects"] as List<Map<String, Any?>>)
        dump("objects", objects)
        val byName = objects.associate { (it["name"] as String) to (it["type"] as String) }
        for (table in listOf("dept", "emp", "orders", "order_item", "shipment", "sales_hist")) {
            assertTrue(byName[n(table)]?.contains("TABLE") == true, "$table in $byName")
        }
        assertTrue(byName[n("emp_dept_v")]?.contains("VIEW") == true, "view: $byName")
        assertEquals("FUNCTION", byName[n("emp_count")], "function: $byName")
        assertEquals("PROCEDURE", byName[n("raise_salary")], "procedure: $byName")
        assertEquals("사원", objects.first { it["name"] == n("emp") }["remarks"])

        val columns = session.columns(schema, n("emp"))["columns"] as List<Map<String, Any?>>
        dump("columns emp", columns)
        assertEquals(n("emp_id"), columns.first()["name"])
        assertEquals(true, columns.first()["primaryKey"])
        assertEquals("사원 이름", columns.first { it["name"] == n("emp_name") }["remarks"])
        assertEquals(false, columns.first { it["name"] == n("emp_name") }["nullable"])

        val hrColumns = session.columns(hrSchema, n("region"))["columns"] as List<Map<String, Any?>>
        assertEquals(listOf(n("region_id"), n("region_name")), hrColumns.map { it["name"] })

        // PostgreSQL만 데이터베이스 목록을 준다 (템플릿 제외). 다른 DB는 빈 목록
        val databases = session.databases()
        dump("databases", databases)
        val databaseNames = databases["names"] as List<String>
        if (type == "postgres") {
            assertContains(databaseNames, database)
            assertTrue(databaseNames.none { it.startsWith("template") }, "templates: $databaseNames")
            assertEquals(database, databases["current"])
        } else {
            assertTrue(databaseNames.isEmpty(), "$type: $databaseNames")
        }
    }

    @Test
    fun executeFetchAndValues() {
        val first = session.execute("SELECT order_id, status FROM orders ORDER BY order_id", 100, 0)
        assertEquals(100, (first["rows"] as List<*>).size)
        assertEquals(true, first["hasMore"])
        val next = session.fetch(first["cursorId"] as String, 100)
        assertEquals(101.0, num((next["rows"] as List<List<Any?>>).first().first()))

        val types = session.execute("SELECT * FROM data_types ORDER BY id", 100, 0)
        dump("data_types columns", types["columns"])
        dump("data_types rows", types["rows"])
        val rows = types["rows"] as List<List<Any?>>
        assertEquals(3, rows.size)
        assertEquals("가나다 abc", rows[0][1])
        assertTrue(rows[1].drop(1).all { it == null }, "row 2 is all NULL: ${rows[1]}")
        val long = rows[2].filterIsInstance<String>().maxBy { it.length }
        assertEquals(8000, long.length, "long text is read whole")

        val names = (types["columns"] as List<Map<String, Any?>>).map { (it["name"] as String).lowercase() }
        fun value(column: String) = rows[0][names.indexOf(column)]
        if (catalogs) {
            assertEquals("18446744073709551615", value("c_bigint"), "BIGINT UNSIGNED beyond Long")
            assertEquals("10100101", value("c_bit"), "BIT(8)")
            assertEquals(2024L, value("c_year"), "YEAR")
        }
        if (oracle) {
            assertEquals(true, value("c_boolean"))
            assertContains(value("c_json").toString(), "DBaro")
        }

        val korean = query("SELECT dept_name FROM dept WHERE dept_id = 20")
        assertEquals("개발", korean.first().first())
    }

    @Test
    fun bindsAndBatch() {
        val result = session.execute(
            "SELECT emp_name FROM emp WHERE emp_id = ? AND job = ?", 10, 0,
            listOf(Session.Bind("7", "NUMBER", false), Session.Bind("STAFF", "VARCHAR", false)),
        )
        assertEquals("사원007", (result["rows"] as List<List<Any?>>).first().first())

        val batch = session.executeBatch(
            listOf(
                "CREATE TABLE batch_probe (id INT PRIMARY KEY)",
                "INSERT INTO batch_probe VALUES (1)",
                "INSERT INTO batch_probe VALUES (1)",
                "DROP TABLE batch_probe",
            ),
            stopOnError = false,
        )
        dump("batch", batch)
        assertEquals(3, (batch["executed"] as Number).toInt())
        assertEquals(1, (batch["failed"] as Number).toInt())
    }

    @Test
    fun cancelsALongQuery() {
        val worker = Executors.newSingleThreadExecutor()
        try {
            val started = System.nanoTime()
            val future = worker.submit<Map<String, Any?>> {
                session.execute("SELECT COUNT(*) FROM order_item a CROSS JOIN order_item b WHERE a.qty + b.qty > 0", 100, 0)
            }
            Thread.sleep(1500)
            session.cancel()
            val error = runCatching { future.get(20, TimeUnit.SECONDS) }.exceptionOrNull()
            val seconds = (System.nanoTime() - started) / 1e9
            dump("cancel", "after %.1f s: %s".format(seconds, error?.cause ?: error))
            assertNotNull(error, "the query was stopped")
            assertTrue(seconds < 15, "stopped quickly ($seconds s)")
        } finally {
            worker.shutdownNow()
        }
        // 세션은 계속 쓸 수 있다
        assertEquals(5, num(scalar("SELECT COUNT(*) FROM dept")).toInt())
    }

    @Test
    fun describe() {
        val result = session.execute("DESC emp", 100, 0)
        dump("desc emp", result["columns"].toString() + "\n" + pretty(result["rows"]))
        val rows = result["rows"] as List<List<Any?>>
        assertTrue(rows.size >= 9, "rows: $rows")
        assertTrue(rows.any { row -> row.any { it.toString().equals("emp_name", ignoreCase = true) } })
    }

    @Test
    fun estimatedPlan() {
        val plan = session.explain("SELECT e.emp_name, o.amount FROM emp e JOIN orders o ON o.emp_id = e.emp_id WHERE o.status = 'NEW'")
        dump("estimated plan", plan)
        val nodes = plan["nodes"] as List<Map<String, Any?>>
        assertTrue(nodes.isNotEmpty())
        val objects = nodes.mapNotNull { it["object"] as Map<String, Any?>? }
        assertTrue(objects.any { (it["name"] as String).equals("orders", ignoreCase = true) }, "objects: $objects")
    }

    @Test
    fun actualPlanOfAQuery() {
        val plan = session.explain("SELECT * FROM orders WHERE status = 'NEW'", actual = true)
        dump("actual plan (status = NEW)", plan)
        assertEquals(true, plan["actual"])
        assertEquals(false, plan["rolledBack"])
        assertEquals(100L, (plan["resultRows"] as Number?)?.toLong() ?: 100L)
        val nodes = plan["nodes"] as List<Map<String, Any?>>
        assertTrue(nodes.isNotEmpty(), "nodes")
        val rows = nodes.mapNotNull { it["rows"] as Map<String, Any?>? }
        assertTrue(rows.any { (it["actual"] as Number?)?.toDouble() == 100.0 }, "some step returned the 100 rows: $rows")
        if (oracle) {
            // 히스토그램 없이 모은 통계라 예상은 약 3만 행 → 크게 빗나간 것으로 칠해져야 한다
            assertTrue(rows.any { r ->
                val e = (r["estimated"] as Number?)?.toDouble() ?: return@any false
                val a = (r["actual"] as Number?)?.toDouble() ?: return@any false
                e >= a * 10 && e - a >= 100
            }, "misestimate: $rows")
            val text = plan["text"] as String
            for (section in listOf("Query Block Name", "Outline Data", "Predicate Information", "Column Projection Information", "A-Rows")) {
                assertContains(text, section)
            }
            assertEquals(true, plan["textFirst"])
        }
    }

    @Test
    fun actualPlanWithMemory() {
        // 해시 조인·정렬로 작업 메모리를 쓰는 계획 (Oracle은 OMem·1Mem·Used-Mem)
        val plan = session.explain(
            "SELECT o.status, COUNT(*), SUM(i.qty * i.price) FROM orders o JOIN order_item i ON i.order_id = o.order_id " +
                "GROUP BY o.status ORDER BY 2 DESC",
            actual = true,
        )
        dump("actual plan (join + sort)", plan)
        val nodes = plan["nodes"] as List<Map<String, Any?>>
        assertTrue(nodes.isNotEmpty())
        if (oracle) {
            // 표의 작업 영역 크기가 DBMS_XPLAN 글의 HASH JOIN 줄과 같다 (단위: 바이트를 K·M으로)
            val columns = plan["columns"] as List<String>
            val join = nodes.first { (it["cells"] as List<*>)[0] == "HASH JOIN" }["cells"] as List<String>
            val line = (plan["text"] as String).lines().first { it.contains("HASH JOIN") }
            for (column in listOf("OMem", "1Mem")) assertContains(line, join[columns.indexOf(column)] + "|")
            assertContains(line, join[columns.indexOf("Used-Mem")].substringBefore(" ("))
        }
    }

    @Test
    fun actualPlanRollsBackDml() {
        val before = num(scalar("SELECT SUM(salary) FROM emp WHERE dept_id = 10"))
        val plan = session.explain("UPDATE emp SET salary = 0 WHERE dept_id = 10", actual = true)
        dump("actual plan (update)", plan)
        assertEquals(true, plan["rolledBack"])
        assertEquals(before, num(scalar("SELECT SUM(salary) FROM emp WHERE dept_id = 10")))

        // 수동 커밋: 앞서 바꾼 것은 남고 분석한 문장만 되돌린다
        session.setAutoCommit(false)
        session.execute("UPDATE dept SET location = 'X' WHERE dept_id = 50", 10, 0)
        session.explain("DELETE FROM shipment WHERE shipment_id < 100", actual = true)
        assertEquals("X", scalar("SELECT location FROM dept WHERE dept_id = 50"))
        assertTrue(num(scalar("SELECT COUNT(*) FROM shipment WHERE shipment_id < 100")).toInt() > 0)
        session.rollback()
        assertEquals("인천", scalar("SELECT location FROM dept WHERE dept_id = 50"))
    }

    @Test
    fun ddlScripts() {
        val targets = mutableListOf("emp", "order_item", "shipment", "sales_hist", "emp_dept_v", "emp_count", "raise_salary")
        if (oracle) targets += listOf("order_seq", "order_api", "emp_touch_trg")
        if (type == "postgres") targets += listOf("order_seq", "emp_touch")
        for (name in targets) {
            val result = runCatching { session.ddl(request(name)) }
            result.onFailure { dump("ddl $name FAILED", it.toString()) }
            val script = result.getOrElse { fail("DDL of $name: $it") }["script"] as String
            dump("ddl $name", script)
            assertTrue(script.contains(n(name), ignoreCase = true), "$name in its DDL")
        }
        val emp = session.ddl(request("emp"))["script"] as String
        assertContains(emp, "emp_mgr_fk", ignoreCase = true)
        assertContains(emp, "사원")
        val item = session.ddl(request("order_item"))["script"] as String
        assertContains(item, "PRIMARY KEY", ignoreCase = true)
        // 스크립트를 그대로 다시 실행할 수 있게: 분할 정의와 파티션, 제약이 쓰는 인덱스는 따로 만들지 않음
        val sales = session.ddl(request("sales_hist"))["script"] as String
        assertContains(sales, "PARTITION BY RANGE", ignoreCase = true)
        if (type == "postgres") {
            assertContains(sales, "PARTITION OF public.sales_hist FOR VALUES FROM ('2023-01-01')")
            assertEquals("PROCEDURE", session.ddl(request("raise_salary"))["type"])
            val partition = session.ddl(request("sales_hist_2023"))["script"] as String
            dump("ddl sales_hist_2023", partition)
            assertFalse(partition.contains("CREATE INDEX"), "the partition's index comes from the parent's")
            assertContains(partition, "CREATE TABLE public.sales_hist_2023 PARTITION OF public.sales_hist\nFOR VALUES FROM")
        }
        if (oracle) {
            assertFalse(Regex("CREATE UNIQUE INDEX \"\\w+\".\"(EMP_PK|EMP_EMAIL_UK)\"").containsMatchIn(emp), emp)
            assertContains(emp, "CREATE INDEX \"$schema\".\"EMP_DEPT_IX\"")
            assertFalse(emp.contains("\n   COMMENT"), "statements start at the line start")
        }
    }

    @Test
    fun tableInfo() {
        val info = session.tableInfo(request("order_item"))
        dump("tableInfo order_item", info)
        assertEquals(listOf(n("order_id"), n("line_no")), info["primaryKey"].let { pk ->
            (pk as? Map<String, Any?>)?.get("columns") ?: pk
        })
        val foreignKeys = info["foreignKeys"] as List<Map<String, Any?>>
        assertEquals(setOf(n("orders"), n("product")), foreignKeys.map { it["refTable"] }.toSet())
        val referencedBy = info["referencedBy"] as List<Map<String, Any?>>
        val shipment = referencedBy.single { it["table"] == n("shipment") }
        assertEquals(listOf(n("order_id"), n("line_no")), shipment["columns"])

        val emp = session.tableInfo(request("emp"))
        val self = (emp["foreignKeys"] as List<Map<String, Any?>>).single { it["refTable"] == n("emp") }
        assertEquals(listOf(n("mgr_id")), self["columns"])
        assertEquals("사원", emp["remarks"])
    }

    @Test
    fun planObject() {
        val result = session.planObject(PlanObject.Request(null, n("orders_status_ix"), "INDEX", n("orders_status_ix"), null))
        dump("planObject orders_status_ix", result)
        val table = result["table"] as Map<String, Any?>
        assertEquals(n("orders"), table["name"])
    }

    @Test
    fun switchesSchema() {
        val result = session.setSchema(hrSchema)
        dump("setSchema", result)
        assertEquals(hrSchema, result["schema"])
        assertEquals(3, num(scalar("SELECT COUNT(*) FROM region")).toInt())
        val info = session.tableInfo(Ddl.Request(null, n("office"), false, false, hrSchema))
        assertEquals(n("region"), (info["foreignKeys"] as List<Map<String, Any?>>).single()["refTable"])
        session.setSchema(schema)
        assertEquals(schema, session.currentSchema())
    }

    @Test
    fun serverOutputAndErrors() {
        if (oracle) {
            val result = session.execute("BEGIN DBMS_OUTPUT.PUT_LINE('안녕 DBaro'); END;", 10, 0)
            assertEquals(listOf("안녕 DBaro"), result["output"])
        }
        val error = assertFailsWith<java.sql.SQLException> { session.execute("SELECT no_such_column FROM emp", 10, 0) }
        dump("error", error.toString())
    }
}
