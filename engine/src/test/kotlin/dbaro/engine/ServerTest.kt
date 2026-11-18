package dbaro.engine

import dbaro.engine.driver.DriverRegistry
import dbaro.engine.ipc.Cbor
import dbaro.engine.ipc.Framing
import dbaro.engine.ipc.Server
import java.io.DataInputStream
import java.io.DataOutputStream
import java.nio.channels.Channels
import java.nio.channels.Pipe
import java.nio.file.Files
import java.util.concurrent.atomic.AtomicInteger
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail

/** 실제 Server를 파이프로 띄우고 H2 내장 DB로 프로토콜 전체 흐름을 확인한다. */
class ServerTest {
    companion object {
        private val databases = AtomicInteger(1)
    }

    private lateinit var toServer: DataOutputStream
    private lateinit var fromServer: DataInputStream
    private lateinit var serverThread: Thread
    private val responses = HashMap<Long, Map<String, Any?>>()
    private var nextId = 1L

    @BeforeTest
    fun start() {
        val requests = Pipe.open()
        val replies = Pipe.open()
        toServer = DataOutputStream(Channels.newOutputStream(requests.sink()))
        fromServer = DataInputStream(Channels.newInputStream(replies.source()))
        val server = Server(
            Channels.newInputStream(requests.source()),
            Channels.newOutputStream(replies.sink()),
            DriverRegistry(Files.createTempDirectory("dbaro-drivers")),
        )
        serverThread = Thread { server.run() }.apply {
            isDaemon = true
            start()
        }
    }

    @AfterTest
    fun stop() {
        toServer.close()
        serverThread.join(5_000)
    }

    private fun send(method: String, params: Map<String, Any?> = emptyMap()): Long {
        val id = nextId++
        Framing.write(toServer, Cbor.encode(mapOf("id" to id, "method" to method, "params" to params)))
        return id
    }

    @Suppress("UNCHECKED_CAST")
    private fun await(id: Long): Map<String, Any?> {
        while (true) {
            responses.remove(id)?.let { return it }
            val frame = Framing.read(fromServer) ?: fail("The server closed the connection")
            val message = Cbor.decode(frame) as Map<String, Any?>
            responses[(message["id"] as Number).toLong()] = message
        }
    }

    @Suppress("UNCHECKED_CAST")
    private fun call(method: String, params: Map<String, Any?> = emptyMap()): Map<String, Any?> {
        val response = await(send(method, params))
        response["error"]?.let { fail("$method failed: $it") }
        return response["result"] as Map<String, Any?>
    }

    @Suppress("UNCHECKED_CAST")
    private fun callError(method: String, params: Map<String, Any?>): Map<String, Any?> {
        val response = await(send(method, params))
        return response["error"] as? Map<String, Any?> ?: fail("$method should have failed: ${response["result"]}")
    }

    private fun connect(autoCommit: Boolean = true): String = call(
        "connect",
        mapOf(
            "type" to "generic",
            "url" to "jdbc:h2:mem:db${databases.getAndIncrement()};DB_CLOSE_DELAY=-1",
            "driverClass" to "org.h2.Driver",
            "user" to "sa",
            "password" to "",
            "autoCommit" to autoCommit,
        ),
    )["sessionId"] as String

    private fun execute(session: String, sql: String, pageSize: Int = 500) =
        call("execute", mapOf("sessionId" to session, "sql" to sql, "pageSize" to pageSize))

    @Test
    fun hello() {
        val result = call("hello")
        assertEquals(ENGINE_VERSION, result["version"])
    }

    @Test
    fun pagesThroughLargeResults() {
        val session = connect()
        execute(session, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20))")
        val insert = execute(session, "INSERT INTO t SELECT X, 'name' || X FROM SYSTEM_RANGE(1, 1200)")
        assertEquals("update", insert["kind"])
        assertEquals(1200L, insert["updateCount"])

        val first = execute(session, "SELECT id, name FROM t ORDER BY id")
        assertEquals("resultSet", first["kind"])
        assertEquals(500, (first["rows"] as List<*>).size)
        assertEquals(true, first["hasMore"])
        val cursor = first["cursorId"] as String
        val columns = first["columns"] as List<*>
        assertEquals("ID", (columns[0] as Map<*, *>)["name"])
        assertEquals(true, (columns[0] as Map<*, *>)["numeric"])

        val second = call("fetch", mapOf("sessionId" to session, "cursorId" to cursor, "count" to 500))
        assertEquals(500, (second["rows"] as List<*>).size)
        assertEquals(true, second["hasMore"])
        val third = call("fetch", mapOf("sessionId" to session, "cursorId" to cursor, "count" to 500))
        val lastRows = third["rows"] as List<*>
        assertEquals(200, lastRows.size)
        assertEquals(false, third["hasMore"])
        assertEquals(listOf(1200L, "name1200"), lastRows.last())

        val gone = callError("fetch", mapOf("sessionId" to session, "cursorId" to cursor, "count" to 500))
        assertTrue((gone["message"] as String).contains("no longer available"))
    }

    @Test
    fun convertsValueTypes() {
        val session = connect()
        val result = execute(
            session,
            "SELECT 1 AS i, CAST(1.5 AS DOUBLE PRECISION) AS d, CAST(12.30 AS DECIMAL(10,2)) AS n, 'abc' AS s, " +
                "NULL AS z, TRUE AS b, DATE '2024-01-02' AS dt, TIMESTAMP '2024-01-02 03:04:05' AS ts, " +
                "TIMESTAMP '2024-01-02 03:04:05.25' AS tf, X'0102' AS bin",
        )
        val row = (result["rows"] as List<*>).single() as List<*>
        assertEquals(1L, row[0])
        assertEquals(1.5, row[1])
        assertEquals("12.30", row[2])
        assertEquals("abc", row[3])
        assertNull(row[4])
        assertEquals(true, row[5])
        assertEquals("2024-01-02", row[6])
        assertEquals("2024-01-02 03:04:05", row[7])
        assertEquals("2024-01-02 03:04:05.25", row[8])
        assertTrue((row[9] as ByteArray).contentEquals(byteArrayOf(1, 2)))
        assertEquals(false, result["hasMore"])
        assertNull(result["cursorId"])
    }

    @Test
    fun reportsSqlErrors() {
        val session = connect()
        val error = callError("execute", mapOf("sessionId" to session, "sql" to "SELECT * FROM missing_table"))
        assertTrue((error["message"] as String).contains("MISSING_TABLE", ignoreCase = true))
        assertNotNull(error["sqlState"])
        // 오류 뒤에도 세션은 계속 쓸 수 있다
        assertEquals("resultSet", execute(session, "SELECT 1")["kind"])
    }

    @Test
    fun commitsAndRollsBack() {
        val session = connect(autoCommit = false)
        execute(session, "CREATE TABLE tx (id INT)")
        execute(session, "INSERT INTO tx VALUES (1)")
        call("rollback", mapOf("sessionId" to session))
        assertEquals(0L, countRows(session))
        execute(session, "INSERT INTO tx VALUES (2)")
        call("commit", mapOf("sessionId" to session))
        call("setAutoCommit", mapOf("sessionId" to session, "autoCommit" to true))
        assertEquals(1L, countRows(session))
    }

    private fun countRows(session: String): Long =
        ((execute(session, "SELECT COUNT(*) FROM tx")["rows"] as List<*>).single() as List<*>)[0] as Long

    @Test
    fun cancelsRunningStatement() {
        val session = connect()
        val id = send(
            "execute",
            mapOf(
                "sessionId" to session,
                "sql" to "SELECT COUNT(*) FROM SYSTEM_RANGE(1, 100000) a, SYSTEM_RANGE(1, 100000) b WHERE a.X + b.X < 0",
            ),
        )
        Thread.sleep(500)
        val started = System.nanoTime()
        call("cancel", mapOf("sessionId" to session))
        val response = await(id)
        assertNotNull(response["error"], "The long query should end with a cancel error")
        assertTrue((System.nanoTime() - started) / 1_000_000 < 10_000, "Cancel should stop the query quickly")
    }

    @Test
    fun disconnectsAndRejectsUnknownSessions() {
        val session = connect()
        call("disconnect", mapOf("sessionId" to session))
        val error = callError("execute", mapOf("sessionId" to session, "sql" to "SELECT 1"))
        assertTrue((error["message"] as String).contains("not connected"))
        assertTrue((callError("noSuchMethod", emptyMap())["message"] as String).contains("Unknown method"))
    }

    @Test
    fun testsConnection() {
        val result = call(
            "test",
            mapOf("type" to "generic", "url" to "jdbc:h2:mem:probe", "driverClass" to "org.h2.Driver", "user" to "sa", "password" to ""),
        )
        assertEquals("H2", result["product"])
    }

    @Test
    fun readsMetadata() {
        val session = connect()
        execute(session, "CREATE TABLE emp_2 (id INT PRIMARY KEY, name VARCHAR(30) NOT NULL, salary DECIMAL(10,2) DEFAULT 0)")
        execute(session, "COMMENT ON TABLE emp_2 IS '사원'")
        execute(session, "COMMENT ON COLUMN emp_2.name IS '이름'")
        execute(session, "CREATE VIEW emp_view AS SELECT id, name FROM emp_2")

        val schemas = call("metaSchemas", mapOf("sessionId" to session))
        assertTrue((schemas["names"] as List<*>).contains("PUBLIC"))
        assertEquals("PUBLIC", schemas["current"])

        @Suppress("UNCHECKED_CAST")
        val objects = call("metaObjects", mapOf("sessionId" to session, "schema" to "PUBLIC"))["objects"] as List<Map<String, Any?>>
        val table = objects.single { it["name"] == "EMP_2" }
        assertTrue((table["type"] as String).contains("TABLE"))
        assertEquals("사원", table["remarks"])
        assertTrue((objects.single { it["name"] == "EMP_VIEW" }["type"] as String).contains("VIEW"))

        @Suppress("UNCHECKED_CAST")
        val columns = call("metaColumns", mapOf("sessionId" to session, "schema" to "PUBLIC", "table" to "EMP_2"))["columns"] as List<Map<String, Any?>>
        assertEquals(listOf("ID", "NAME", "SALARY"), columns.map { it["name"] })
        assertEquals(true, columns[0]["primaryKey"])
        assertEquals(false, columns[1]["nullable"])
        assertEquals("이름", columns[1]["remarks"])
        assertTrue((columns[1]["type"] as String).endsWith("(30)"), "type: ${columns[1]["type"]}")
        assertTrue((columns[2]["type"] as String).endsWith("(10,2)"), "type: ${columns[2]["type"]}")
    }

    @Test
    fun reportsExecuteAndFetchTimes() {
        val session = connect()
        val first = execute(session, "SELECT X FROM SYSTEM_RANGE(1, 30)", pageSize = 10)
        val executeMs = first["executeMs"] as Number
        val elapsedMs = first["elapsedMs"] as Number
        assertTrue(executeMs.toLong() in 0..elapsedMs.toLong(), "execute $executeMs ms, total $elapsedMs ms")
        val next = call("fetch", mapOf("sessionId" to session, "cursorId" to first["cursorId"], "count" to 10))
        assertNotNull(next["elapsedMs"])
    }

    @Test
    fun explainsWithoutExecuting() {
        val session = connect()
        execute(session, "CREATE TABLE plan_t (id INT PRIMARY KEY, name VARCHAR(20))")
        val plan = call("explain", mapOf("sessionId" to session, "sql" to "DELETE FROM plan_t WHERE id = 1"))
        assertTrue((plan["text"] as String).contains("PLAN_T", ignoreCase = true), "plan: ${plan["text"]}")
        assertNotNull(plan["elapsedMs"])
        // 계획만 보았으므로 아무것도 지워지지 않는다 (애초에 행도 없지만 오류도 없다)
        execute(session, "INSERT INTO plan_t VALUES (1, 'a')")
        call("explain", mapOf("sessionId" to session, "sql" to "DELETE FROM plan_t"))
        assertEquals(1L, ((execute(session, "SELECT COUNT(*) FROM plan_t")["rows"] as List<*>).single() as List<*>)[0])
    }

    @Test
    fun generatesDdlWithCommentsIndexesSynonymsAndGrants() {
        val session = connect()
        execute(session, "CREATE TABLE emp_ddl (id INT PRIMARY KEY, name VARCHAR(30) NOT NULL, salary DECIMAL(10,2) DEFAULT 0)")
        execute(session, "COMMENT ON TABLE emp_ddl IS '사원'")
        execute(session, "COMMENT ON COLUMN emp_ddl.name IS '이름'")
        execute(session, "CREATE INDEX emp_ddl_name_ix ON emp_ddl (name)")
        execute(session, "CREATE USER reporter PASSWORD 'x'")
        execute(session, "GRANT SELECT, UPDATE ON emp_ddl TO reporter")
        execute(session, "CREATE SYNONYM emp_syn FOR emp_ddl")
        execute(session, "CREATE VIEW emp_ddl_v AS SELECT id, name FROM emp_ddl")

        val table = call("ddl", mapOf("sessionId" to session, "name" to "emp_ddl"))
        val script = table["script"] as String
        assertEquals("EMP_DDL", table["name"])
        for (expected in listOf(
            "CREATE TABLE \"PUBLIC\".\"EMP_DDL\" (",
            "\"NAME\" CHARACTER VARYING(30) NOT NULL",
            "(10,2) DEFAULT 0",
            "PRIMARY KEY (\"ID\")",
            "-- Comments",
            "COMMENT ON TABLE \"PUBLIC\".\"EMP_DDL\" IS '사원';",
            "COMMENT ON COLUMN \"PUBLIC\".\"EMP_DDL\".\"NAME\" IS '이름';",
            "-- Indexes",
            "CREATE INDEX \"PUBLIC\".\"EMP_DDL_NAME_IX\" ON \"PUBLIC\".\"EMP_DDL\" (\"NAME\");",
            "-- Synonyms",
            "SYNONYM \"PUBLIC\".\"EMP_SYN\" FOR \"PUBLIC\".\"EMP_DDL\";",
            "-- Grants",
            "ON \"PUBLIC\".\"EMP_DDL\" TO \"REPORTER\";",
        )) {
            assertTrue(script.contains(expected), "missing <$expected> in:\n$script")
        }
        assertTrue(Regex("GRANT (SELECT, UPDATE|UPDATE, SELECT) ON").containsMatchIn(script), script)

        // 스키마로 한정한 이름, 뷰
        val view = call("ddl", mapOf("sessionId" to session, "schema" to "public", "name" to "Emp_Ddl_V"))
        assertEquals("VIEW", view["type"])
        assertTrue((view["script"] as String).contains("CREATE VIEW \"PUBLIC\".\"EMP_DDL_V\" AS"), view["script"].toString())

        val missing = callError("ddl", mapOf("sessionId" to session, "name" to "no_such_object"))
        assertTrue((missing["message"] as String).contains("Object not found"), missing.toString())
    }

    @Test
    fun explainsMissingTiberoDriver() {
        val error = callError("test", mapOf("type" to "tibero", "url" to "jdbc:tibero:thin:@localhost:8629:tibero"))
        assertTrue((error["message"] as String).contains("Tibero JDBC driver"))
    }
}
