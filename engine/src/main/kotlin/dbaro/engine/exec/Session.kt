package dbaro.engine.exec

import dbaro.engine.Log
import dbaro.engine.daemonThreads
import java.sql.Connection
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

    fun execute(sql: String, pageSize: Int, queryTimeoutSec: Int): Map<String, Any?> {
        // DESC는 SQL*Plus·psql의 명령이라 Oracle·Tibero·PostgreSQL 서버가 모르므로 딕셔너리로 직접 답한다.
        if (Describe.supports(type)) Describe.parse(sql)?.let { return describe(it) }

        val statement = connection.createStatement()
        var keepOpen = false
        val start = System.nanoTime()
        try {
            if (queryTimeoutSec > 0) statement.queryTimeout = queryTimeoutSec
            statement.fetchSize = pageSize

            running = statement
            val hasResultSet = try {
                statement.execute(sql)
            } finally {
                running = null
            }
            val executedAt = System.nanoTime()

            val result = linkedMapOf<String, Any?>()
            if (hasResultSet) {
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

    /** 예상 실행 계획. 문장은 실행하지 않는다. */
    fun explain(sql: String): Map<String, Any?> {
        val start = System.nanoTime()
        val plan = Explain.explain(type, connection, sql)
        return plan + ("elapsedMs" to (System.nanoTime() - start) / 1_000_000)
    }

    /** 객체 하나의 DDL 스크립트 (코멘트, 인덱스, 트리거, 시노님, 권한 포함) */
    fun ddl(request: Ddl.Request): Map<String, Any?> = Ddl.script(type, connection, request).toMap()

    fun closeCursor(cursorId: String) {
        cursors.remove(cursorId)?.close()
    }

    fun cancel() {
        val statement = running ?: return
        runCatching { statement.cancel() }.onFailure { Log.error("Cancel failed", it) }
    }

    // ---------------------------------------------------------------------
    // 메타데이터 (Database Explorer). JDBC DatabaseMetaData를 쓴다.
    // MySQL·MariaDB는 데이터베이스(catalog)가 스키마 역할을 하므로 catalog로 조회한다.

    private val usesCatalogs = type == "mysql" || type == "mariadb"
    private val oracleFamily = type == "oracle" || type == "tibero"

    fun schemas(): Map<String, Any?> {
        val meta = connection.metaData
        val names = ArrayList<String>()
        (if (usesCatalogs) meta.catalogs else meta.schemas).use { rs ->
            while (rs.next()) rs.getString(1)?.let { names.add(it) }
        }
        val current = runCatching { if (usesCatalogs) connection.catalog else connection.schema }.getOrNull()
        return mapOf("names" to names.sorted(), "current" to current)
    }

    fun objects(schema: String): Map<String, Any?> {
        val meta = connection.metaData
        val catalog = if (usesCatalogs) schema else null
        val schemaPattern = if (usesCatalogs) null else pattern(schema)
        val objects = ArrayList<Map<String, Any?>>()

        meta.getTables(catalog, schemaPattern, "%", null).use { rs ->
            while (rs.next() && objects.size < MAX_OBJECTS) {
                objects.add(
                    mapOf(
                        "name" to rs.getString("TABLE_NAME"),
                        "type" to (rs.getString("TABLE_TYPE") ?: "TABLE"),
                        "remarks" to rs.getString("REMARKS"),
                    ),
                )
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

    private fun columnType(rs: ResultSet): String {
        val typeName = rs.getString("TYPE_NAME") ?: "?"
        val size = rs.getInt("COLUMN_SIZE").takeUnless { rs.wasNull() } ?: 0
        val digits = rs.getInt("DECIMAL_DIGITS").takeUnless { rs.wasNull() } ?: 0
        if (typeName.contains('(')) return typeName
        return when (rs.getInt("DATA_TYPE")) {
            Types.CHAR, Types.VARCHAR, Types.NCHAR, Types.NVARCHAR, Types.BINARY, Types.VARBINARY ->
                if (size > 0) "$typeName($size)" else typeName
            Types.NUMERIC, Types.DECIMAL -> when {
                size > 0 && digits > 0 -> "$typeName($size,$digits)"
                size > 0 -> "$typeName($size)"
                else -> typeName
            }
            else -> typeName
        }
    }

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
