package dbaro.engine.exec

import dbaro.engine.Log
import dbaro.engine.daemonThreads
import java.sql.CallableStatement
import java.sql.Connection
import java.sql.PreparedStatement
import java.sql.ResultSet
import java.sql.Statement
import java.sql.Types
import java.util.concurrent.Executors

/**
 * 연결 하나. 앱의 쿼리 탭 하나와 짝을 이룬다.
 * 모든 작업은 세션 전용 스레드에서 순서대로 실행되고, cancel만 다른 스레드에서 바로 불린다.
 * 결과는 페이지 단위로 보낸다. 남은 행이 있으면 커서를 열어 두고 fetch로 이어서 가져온다.
 */
class Session(val id: String, private val type: String, private val connection: Connection) : AutoCloseable {
    companion object {
        private const val MAX_OBJECTS = 20_000
    }

    private class Cursor(val statement: Statement, val resultSet: ResultSet, val types: IntArray) {
        fun close() {
            runCatching { resultSet.close() }
            runCatching { statement.close() }
        }
    }

    private class Page(val rows: List<List<Any?>>, val hasMore: Boolean)

    private val executor = Executors.newSingleThreadExecutor(daemonThreads("session-$id"))
    private val cursors = HashMap<String, Cursor>()
    private var cursorSeq = 0

    @Volatile
    private var running: Statement? = null

    @Volatile
    private var cancelRequested = false

    // Oracle 계열은 DBMS_OUTPUT을 켜 두고, 실행이 끝날 때마다 쌓인 줄을 가져온다.
    private val readsServerOutput = (type == "oracle" || type == "tibero") && enableServerOutput()

    fun submit(task: () -> Unit) {
        executor.execute { task() }
    }

    fun info(): Map<String, Any?> {
        val meta = connection.metaData
        return mapOf(
            "sessionId" to id,
            "product" to meta.databaseProductName,
            "productVersion" to meta.databaseProductVersion,
            "driverName" to meta.driverName,
            "driverVersion" to meta.driverVersion,
            "autoCommit" to connection.autoCommit,
        )
    }

    /**
     * 위치 바인드 (?) 하나. [out]이면 CallableStatement의 OUT 파라미터로 등록하고 실행한 뒤 값을 돌려준다.
     * [type]: VARCHAR, NUMERIC, CLOB (CLI의 VARIABLE)
     */
    class Bind(val value: Any?, val type: String, val out: Boolean)

    fun execute(sql: String, pageSize: Int, queryTimeoutSec: Int, binds: List<Bind> = emptyList()): Map<String, Any?> {
        // DESC는 SQL*Plus·psql의 명령이라 Oracle·Tibero·PostgreSQL 서버가 모르므로 딕셔너리로 직접 답한다.
        if (binds.isEmpty() && Describe.supports(type)) Describe.parse(sql)?.let { return describe(it) }

        val callable = binds.any { it.out }
        val statement: Statement = when {
            binds.isEmpty() -> connection.createStatement()
            callable -> connection.prepareCall(sql)
            else -> connection.prepareStatement(sql)
        }
        var keepOpen = false
        val start = System.nanoTime()
        try {
            if (queryTimeoutSec > 0) statement.queryTimeout = queryTimeoutSec
            statement.fetchSize = pageSize
            if (statement is PreparedStatement) bindAll(statement, binds)

            running = statement
            val hasResultSet = try {
                if (statement is PreparedStatement) statement.execute() else statement.execute(sql)
            } finally {
                running = null
            }
            val executedAt = System.nanoTime()

            val result = linkedMapOf<String, Any?>()
            // OUT 파라미터가 있는 호출(PL/SQL 블록)은 결과 표 없이 OUT 값만 돌려준다.
            // 드라이버에 따라(H2) OUT 값이 결과 집합의 첫 행에 묶여 있으면 그 행으로 옮겨 읽는다.
            if (statement is CallableStatement) {
                val outs = runCatching { readOuts(statement, binds) }.getOrElse { error ->
                    if (!hasResultSet || statement.resultSet?.next() != true) throw error
                    readOuts(statement, binds)
                }
                result["kind"] = "update"
                result["updateCount"] = -1L
                result["outBinds"] = outs
            } else if (hasResultSet) {
                val resultSet = statement.resultSet
                val meta = resultSet.metaData
                val types = IntArray(meta.columnCount) { meta.getColumnType(it + 1) }
                result["kind"] = "resultSet"
                result["columns"] = (1..meta.columnCount).map { column ->
                    mapOf(
                        "name" to meta.getColumnLabel(column),
                        "typeName" to meta.getColumnTypeName(column),
                        "numeric" to Values.isNumeric(types[column - 1]),
                    )
                }
                val page = readPage(resultSet, types, pageSize)
                result["rows"] = page.rows
                result["hasMore"] = page.hasMore
                if (page.hasMore) {
                    val cursorId = "c${++cursorSeq}"
                    cursors[cursorId] = Cursor(statement, resultSet, types)
                    result["cursorId"] = cursorId
                    keepOpen = true
                }
            } else {
                result["kind"] = "update"
                result["updateCount"] = statement.updateCount.toLong()
            }
            val finishedAt = System.nanoTime()
            result["elapsedMs"] = (finishedAt - start) / 1_000_000
            result["executeMs"] = (executedAt - start) / 1_000_000 // 나머지는 첫 페이지를 읽은 시간
            result["warnings"] = warnings(statement)
            result["output"] = readServerOutput()
            return result
        } finally {
            if (!keepOpen) runCatching { statement.close() }
        }
    }

    private fun jdbcType(type: String): Int = when (type.uppercase()) {
        "NUMERIC", "NUMBER" -> Types.NUMERIC
        "CLOB" -> Types.CLOB
        else -> Types.VARCHAR
    }

    private fun bindAll(statement: PreparedStatement, binds: List<Bind>) {
        binds.forEachIndexed { i, bind ->
            val index = i + 1
            val jdbc = jdbcType(bind.type)
            when {
                bind.out -> (statement as CallableStatement).registerOutParameter(index, jdbc)
                bind.value == null -> statement.setNull(index, jdbc)
                jdbc == Types.NUMERIC -> {
                    val number = bind.value.toString().trim().toBigDecimalOrNull()
                    if (number != null) statement.setBigDecimal(index, number) else statement.setString(index, bind.value.toString())
                }
                else -> statement.setString(index, bind.value.toString())
            }
        }
    }

    private fun readOuts(statement: CallableStatement, binds: List<Bind>): List<Any?> =
        binds.mapIndexed { index, bind -> if (bind.out) outValue(statement.getObject(index + 1)) else null }

    /** OUT 파라미터 값 (결과 표와 같은 규칙: 큰 숫자는 문자열, CLOB은 64K 글자까지) */
    private fun outValue(value: Any?): Any? = when (value) {
        null -> null
        is java.sql.Clob -> value.getSubString(1, minOf(value.length(), 65_536L).toInt())
        is java.math.BigDecimal -> value.toPlainString()
        is Int, is Long, is Short, is Byte -> (value as Number).toLong()
        is Double, is Float -> (value as Number).toDouble()
        is Boolean -> value
        else -> value.toString()
    }

    /** DESC 결과. 행이 적으므로 한 번에 모두 보낸다. */
    private fun describe(target: Describe.Target): Map<String, Any?> {
        val start = System.nanoTime()
        // PostgreSQL은 트랜잭션 안에서 문장이 하나라도 실패하면 트랜잭션 전체가 막힌다.
        // 사용자의 열린 트랜잭션을 지키려고 세이브포인트 안에서 조회한다.
        val savepoint = if (type == "postgres" && !connection.autoCommit) connection.setSavepoint() else null
        val table = try {
            Describe.describe(type, connection, target)
        } catch (e: Exception) {
            savepoint?.let { runCatching { connection.rollback(it) } }
            throw e
        }
        savepoint?.let { runCatching { connection.releaseSavepoint(it) } }
        val elapsedMs = (System.nanoTime() - start) / 1_000_000
        return linkedMapOf(
            "kind" to "resultSet",
            "columns" to table.columns.map { mapOf("name" to it, "typeName" to "VARCHAR2", "numeric" to false) },
            "rows" to table.rows,
            "hasMore" to false,
            "elapsedMs" to elapsedMs,
            "executeMs" to elapsedMs,
            "warnings" to emptyList<String>(),
            "output" to emptyList<String>(),
        )
    }

    fun fetch(cursorId: String, count: Int): Map<String, Any?> {
        val cursor = cursors[cursorId] ?: throw IllegalStateException("The result is no longer available.")
        val start = System.nanoTime()
        val page = try {
            readPage(cursor.resultSet, cursor.types, count)
        } catch (e: Exception) {
            cursors.remove(cursorId)?.close()
            throw e
        }
        if (!page.hasMore) cursors.remove(cursorId)?.close()
        return mapOf("rows" to page.rows, "hasMore" to page.hasMore, "elapsedMs" to (System.nanoTime() - start) / 1_000_000)
    }

    /**
     * 실행 계획. 보통은 예상 계획이라 문장을 실행하지 않는다.
     * [actual]이면 문장을 실행해서 얻은 실제 계획이고, 바꾼 데이터는 되돌린다 ([ActualPlan]). 실행 중에는 cancel로 멈출 수 있다.
     */
    fun explain(sql: String, actual: Boolean = false, queryTimeoutSec: Int = 0): Map<String, Any?> {
        val start = System.nanoTime()
        val plan = if (!actual) {
            Explain.explain(type, connection, sql)
        } else {
            cancelRequested = false
            ActualPlan.explain(type, connection, sql, queryTimeoutSec) { running = it }
        }
        return plan + ("elapsedMs" to (System.nanoTime() - start) / 1_000_000)
    }

    /** 객체 하나의 DDL 스크립트 (코멘트, 인덱스, 트리거, 시노님, 권한 포함) */
    fun ddl(request: Ddl.Request): Map<String, Any?> = Ddl.script(type, connection, request).toMap()

    /** 테이블 구조 (컬럼, 기본 키, 인덱스, 외래 키) */
    fun tableInfo(request: Ddl.Request): Map<String, Any?> = TableInfo.describe(type, connection, request)

    fun planObject(request: PlanObject.Request): Map<String, Any?> = PlanObject.describe(type, connection, request)

    /** 이름 바꾸기 리팩토링 ([Refactor]): 테이블·컬럼 이름을 쓰는 DB 객체와 그 소스 */
    fun renameImpact(schema: String, table: String, column: String?): Map<String, Any?> =
        timed { Refactor.renameImpact(type, connection, schema, table, column) }

    /** 리버스 엔지니어링 ([Reverse]): 스키마 모델(테이블·컬럼·키·인덱스) */
    fun reverseModel(schema: String, only: Set<String>?, includeViews: Boolean): Map<String, Any?> =
        timed { Reverse.model(type, connection, schema, only, includeViews) }

    /** 스키마 전체 DDL. 객체 사이에서 cancel로 멈춘다. */
    fun schemaDdl(schema: String, only: Set<String>?, includeViews: Boolean, includeRoutines: Boolean): Map<String, Any?> {
        cancelRequested = false
        return timed { Reverse.schemaDdl(type, connection, schema, only, includeViews, includeRoutines) { cancelRequested } }
    }

    /** 모니터링 ([Monitor]): 세션 목록, 락 대기, 용량, 세션 종료 */
    fun monitorSessions(): Map<String, Any?> = timed { Monitor.sessions(type, connection) }
    fun monitorLocks(): Map<String, Any?> = timed { Monitor.locks(type, connection) }
    fun monitorSpace(): Map<String, Any?> = timed { Monitor.space(type, connection) }
    fun killSession(sessionId: String, cancelQuery: Boolean): Map<String, Any?> = Monitor.kill(type, connection, sessionId, cancelQuery)

    private fun timed(work: () -> Map<String, Any?>): Map<String, Any?> {
        val start = System.nanoTime()
        return work() + ("elapsedMs" to (System.nanoTime() - start) / 1_000_000)
    }

    /**
     * 문장 여러 개를 차례로 실행한다 (데이터 불러오기, 결과 편집 적용). 앱과 한 번만 주고받는다.
     * JDBC 배치는 실패한 지점을 드라이버마다 다르게 알려 줘서 쓰지 않고, 하나씩 실행해 정확히 남긴다.
     * stopOnError가 아니면 실패한 문장을 건너뛰고 계속한다 (PostgreSQL 트랜잭션은 문장마다 세이브포인트로 지킨다).
     * 문장 사이에서 cancel을 받으면 거기서 멈춘다.
     */
    fun executeBatch(statements: List<String>, stopOnError: Boolean): Map<String, Any?> {
        val start = System.nanoTime()
        cancelRequested = false
        var executed = 0
        var failed = 0
        var affected = 0L
        var stoppedAt = -1
        val errors = ArrayList<Map<String, Any?>>()
        val guard = type == "postgres" && !stopOnError && !connection.autoCommit
        connection.createStatement().use { statement ->
            for ((index, sql) in statements.withIndex()) {
                if (cancelRequested) {
                    stoppedAt = index
                    break
                }
                val savepoint = if (guard) connection.setSavepoint() else null
                try {
                    running = statement
                    statement.execute(sql)
                    affected += statement.updateCount.coerceAtLeast(0)
                    executed++
                    savepoint?.let { runCatching { connection.releaseSavepoint(it) } }
                } catch (e: Exception) {
                    savepoint?.let { runCatching { connection.rollback(it) } }
                    failed++
                    if (errors.size < 100) errors.add(mapOf("index" to index, "message" to (e.message ?: e.toString())))
                    if (stopOnError) {
                        stoppedAt = index
                        break
                    }
                } finally {
                    running = null
                }
            }
        }
        return mapOf(
            "executed" to executed,
            "failed" to failed,
            "skipped" to statements.size - executed - failed, // 멈춘 뒤 실행하지 않은 문장
            "affected" to affected,
            "stoppedAt" to stoppedAt,
            "cancelled" to cancelRequested,
            "errors" to errors,
            "elapsedMs" to (System.nanoTime() - start) / 1_000_000,
        )
    }

    fun closeCursor(cursorId: String) {
        cursors.remove(cursorId)?.close()
    }

    fun cancel() {
        cancelRequested = true
        val statement = running ?: return
        runCatching { statement.cancel() }.onFailure { Log.error("Cancel failed", it) }
    }

    // ---------------------------------------------------------------------
    // 메타데이터 (Database Explorer). JDBC DatabaseMetaData를 쓴다.
    // MySQL·MariaDB는 데이터베이스(catalog)가 스키마 역할을 하므로 catalog로 조회한다.

    private val usesCatalogs = type == "mysql" || type == "mariadb"
    private val oracleFamily = type == "oracle" || type == "tibero"

    /** 이 세션의 지금 스키마 (MySQL·MariaDB는 데이터베이스). 모르면 null */
    fun currentSchema(): String? = runCatching {
        when {
            oracleFamily -> queryString("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM DUAL")
            type == "postgres" -> queryString("SELECT current_schema()")
            usesCatalogs -> queryString("SELECT DATABASE()")
            else -> connection.schema
        }
    }.getOrNull()

    /**
     * 이 세션의 기본 스키마를 바꾼다 (이름은 정확한 이름, 따옴표 없이).
     * Oracle·Tibero `ALTER SESSION SET CURRENT_SCHEMA`, PostgreSQL `SET search_path`(public은 뒤에 남긴다),
     * MySQL·MariaDB `USE`, 그 밖은 JDBC setSchema. 실행한 문장(`sql`)과 바꾼 뒤의 스키마(`schema`)를 돌려준다.
     */
    fun setSchema(name: String): Map<String, Any?> {
        require(name.isNotEmpty()) { "Schema name is empty" }
        val sql = when {
            oracleFamily -> "ALTER SESSION SET CURRENT_SCHEMA = ${doubleQuoted(name)}"
            type == "postgres" ->
                if (name == "public") "SET search_path TO public" else "SET search_path TO ${doubleQuoted(name)}, public"
            usesCatalogs -> "USE `" + name.replace("`", "``") + "`"
            else -> null
        }
        if (sql != null) connection.createStatement().use { it.execute(sql) } else connection.schema = name
        return mapOf("schema" to currentSchema(), "sql" to sql)
    }

    private fun doubleQuoted(name: String) = "\"" + name.replace("\"", "\"\"") + "\""

    private fun queryString(sql: String): String? =
        connection.createStatement().use { st -> st.executeQuery(sql).use { rs -> if (rs.next()) rs.getString(1) else null } }

    /**
     * 접속하자마자 탭이 고른 스키마로 바꾼다. 실패해도(스키마가 사라짐, 권한 없음) 접속은 그대로 두고 오류 글을 돌려준다.
     * PostgreSQL 수동 커밋은 SET이 트랜잭션 안에서 실행되므로 바로 커밋해서 나중에 롤백해도 돌아가지 않게 한다.
     */
    fun applyInitialSchema(name: String): String? {
        val error = runCatching { setSchema(name) }.exceptionOrNull()
        if (type == "postgres" && !connection.autoCommit) {
            runCatching { if (error == null) connection.commit() else connection.rollback() }
        }
        return error?.let { it.message ?: it.toString() }
    }

    fun schemas(): Map<String, Any?> {
        val meta = connection.metaData
        val names = ArrayList<String>()
        (if (usesCatalogs) meta.catalogs else meta.schemas).use { rs ->
            while (rs.next()) rs.getString(1)?.let { names.add(it) }
        }
        val current = runCatching { if (usesCatalogs) connection.catalog else connection.schema }.getOrNull()
        return mapOf("names" to names.sorted(), "current" to current)
    }

    /**
     * 같은 서버의 데이터베이스 목록 (PostgreSQL). 접속할 수 있는 것만, 템플릿은 뺀다.
     * PostgreSQL은 세션 안에서 데이터베이스를 바꿀 수 없어서 앱이 고른 데이터베이스로 다시 접속한다.
     * 다른 DB는 빈 목록 (MySQL·MariaDB의 데이터베이스는 스키마 목록에 나온다).
     */
    fun databases(): Map<String, Any?> {
        if (type != "postgres") return mapOf("names" to emptyList<String>(), "current" to null)
        val names = ArrayList<String>()
        connection.createStatement().use { st ->
            st.executeQuery("SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate ORDER BY datname")
                .use { rs -> while (rs.next()) rs.getString(1)?.let { names.add(it) } }
        }
        return mapOf("names" to names, "current" to queryString("SELECT current_database()"))
    }

    fun objects(schema: String): Map<String, Any?> {
        val meta = connection.metaData
        val catalog = if (usesCatalogs) schema else null
        val schemaPattern = if (usesCatalogs) null else pattern(schema)
        val objects = ArrayList<Map<String, Any?>>()

        val untyped = ArrayList<String>() // 드라이버가 종류를 주지 않은 객체
        meta.getTables(catalog, schemaPattern, "%", null).use { rs ->
            while (rs.next() && objects.size < MAX_OBJECTS) {
                val name = rs.getString("TABLE_NAME")
                val tableType = rs.getString("TABLE_TYPE")
                if (tableType == null) untyped.add(name)
                objects.add(
                    mapOf(
                        "name" to name,
                        "type" to (tableType ?: "TABLE"),
                        "remarks" to rs.getString("REMARKS"),
                    ),
                )
            }
        }
        // PostgreSQL JDBC는 분할 테이블의 인덱스(relkind 'I')에 종류를 주지 않는다. 테이블로 보이지 않게 pg_class에서 찾는다
        if (type == "postgres" && untyped.isNotEmpty()) {
            val kinds = runCatching { postgresRelkinds(schema, untyped) }.getOrDefault(emptyMap())
            for (i in objects.indices) {
                val kind = kinds[objects[i]["name"]] ?: continue
                objects[i] = objects[i] + ("type" to relkindType(kind))
            }
        }

        // 함수와 프로시저. Oracle의 패키지 안 루틴(카탈로그 = 패키지 이름)은 제외한다.
        val functions = HashSet<String>()
        runCatching {
            meta.getFunctions(catalog, schemaPattern, "%").use { rs ->
                while (rs.next() && objects.size < MAX_OBJECTS) {
                    if (oracleFamily && rs.getString("FUNCTION_CAT") != null) continue
                    val name = rs.getString("FUNCTION_NAME") ?: continue
                    if (functions.add(name)) objects.add(mapOf("name" to name, "type" to "FUNCTION", "remarks" to rs.getString("REMARKS")))
                }
            }
        }
        runCatching {
            val procedures = HashSet<String>()
            meta.getProcedures(catalog, schemaPattern, "%").use { rs ->
                while (rs.next() && objects.size < MAX_OBJECTS) {
                    if (oracleFamily && rs.getString("PROCEDURE_CAT") != null) continue
                    val name = rs.getString("PROCEDURE_NAME") ?: continue
                    if (name !in functions && procedures.add(name)) {
                        objects.add(mapOf("name" to name, "type" to "PROCEDURE", "remarks" to rs.getString("REMARKS")))
                    }
                }
            }
        }
        return mapOf("objects" to objects, "truncated" to (objects.size >= MAX_OBJECTS))
    }

    private fun postgresRelkinds(schema: String, names: List<String>): Map<String, String> {
        val kinds = HashMap<String, String>()
        connection.prepareStatement(
            "SELECT c.relname, c.relkind::text FROM pg_catalog.pg_class c " +
                "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = ? AND c.relname = ANY(?)",
        ).use { st ->
            st.setString(1, schema)
            st.setArray(2, connection.createArrayOf("text", names.toTypedArray()))
            st.executeQuery().use { rs -> while (rs.next()) kinds[rs.getString(1)] = rs.getString(2) }
        }
        return kinds
    }

    private fun relkindType(kind: String) = when (kind) {
        "i", "I" -> "INDEX"
        "S" -> "SEQUENCE"
        "v" -> "VIEW"
        "m" -> "MATERIALIZED VIEW"
        "p" -> "PARTITIONED TABLE"
        "f" -> "FOREIGN TABLE"
        "c" -> "TYPE"
        else -> "TABLE"
    }

    fun columns(schema: String, table: String): Map<String, Any?> {
        val meta = connection.metaData
        val catalog = if (usesCatalogs) schema else null
        val exactSchema = if (usesCatalogs) null else schema

        val primaryKey = HashSet<String>()
        runCatching {
            meta.getPrimaryKeys(catalog, exactSchema, table).use { rs ->
                while (rs.next()) rs.getString("COLUMN_NAME")?.let { primaryKey.add(it) }
            }
        }

        val columns = ArrayList<Map<String, Any?>>()
        meta.getColumns(catalog, if (usesCatalogs) null else pattern(schema), pattern(table), "%").use { rs ->
            while (rs.next()) {
                val name = rs.getString("COLUMN_NAME")
                columns.add(
                    mapOf(
                        "name" to name,
                        "type" to columnType(rs),
                        "nullable" to (rs.getInt("NULLABLE") != java.sql.DatabaseMetaData.columnNoNulls),
                        "primaryKey" to (name in primaryKey),
                        "default" to rs.getString("COLUMN_DEF")?.trim(),
                        "remarks" to rs.getString("REMARKS"),
                    ),
                )
            }
        }
        return mapOf("columns" to columns)
    }

    /** 메타데이터 조회는 LIKE 패턴이므로 이름의 _ 와 % 를 이스케이프한다. */
    private fun pattern(name: String): String {
        val escape = runCatching { connection.metaData.searchStringEscape }.getOrNull()
        if (escape.isNullOrEmpty()) return name
        return name.replace(escape, escape + escape).replace("_", escape + "_").replace("%", escape + "%")
    }

    /** 탐색기·자동완성의 컬럼 타입 글 (테이블 상세·리버스 엔지니어링과 같은 규칙) */
    private fun columnType(rs: ResultSet): String = TableInfo.columnType(rs)

    fun commit() = connection.commit()

    fun rollback() = connection.rollback()

    fun setAutoCommit(autoCommit: Boolean) {
        connection.autoCommit = autoCommit
    }

    override fun close() {
        for (cursor in cursors.values) cursor.close()
        cursors.clear()
        // 커밋하지 않은 변경은 버린다 (드라이버마다 close 때의 동작이 달라서 명시적으로 롤백).
        runCatching { if (!connection.autoCommit) connection.rollback() }
        runCatching { connection.close() }
        executor.shutdown()
    }

    private fun readPage(resultSet: ResultSet, types: IntArray, count: Int): Page {
        val rows = ArrayList<List<Any?>>(minOf(count, 1000))
        while (rows.size < count && resultSet.next()) {
            rows.add(List(types.size) { Values.read(resultSet, it + 1, types[it]) })
        }
        return Page(rows, rows.size == count)
    }

    private fun warnings(statement: Statement): List<String> {
        val messages = ArrayList<String>()
        var warning = runCatching { statement.warnings }.getOrNull()
        while (warning != null && messages.size < 100) {
            messages.add(warning.message ?: warning.toString())
            warning = warning.nextWarning
        }
        return messages
    }

    private fun enableServerOutput(): Boolean = try {
        connection.createStatement().use { it.execute("BEGIN DBMS_OUTPUT.ENABLE(NULL); END;") }
        true
    } catch (e: Exception) {
        Log.info("DBMS_OUTPUT is not available: ${e.message}")
        false
    }

    private fun readServerOutput(): List<String> {
        if (!readsServerOutput) return emptyList()
        val lines = ArrayList<String>()
        try {
            connection.prepareCall("BEGIN DBMS_OUTPUT.GET_LINE(?, ?); END;").use { call ->
                call.registerOutParameter(1, Types.VARCHAR)
                call.registerOutParameter(2, Types.INTEGER)
                while (lines.size < 10_000) {
                    call.execute()
                    if (call.getInt(2) != 0) break
                    lines.add(call.getString(1) ?: "")
                }
            }
        } catch (e: Exception) {
            Log.info("Could not read DBMS_OUTPUT: ${e.message}")
        }
        return lines
    }
}
