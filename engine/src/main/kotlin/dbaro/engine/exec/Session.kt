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
            result["elapsedMs"] = (System.nanoTime() - start) / 1_000_000
            result["warnings"] = warnings(statement)
            result["output"] = readServerOutput()
            return result
        } finally {
            if (!keepOpen) runCatching { statement.close() }
        }
    }

    fun fetch(cursorId: String, count: Int): Map<String, Any?> {
        val cursor = cursors[cursorId] ?: throw IllegalStateException("The result is no longer available.")
        val page = try {
            readPage(cursor.resultSet, cursor.types, count)
        } catch (e: Exception) {
            cursors.remove(cursorId)?.close()
            throw e
        }
        if (!page.hasMore) cursors.remove(cursorId)?.close()
        return mapOf("rows" to page.rows, "hasMore" to page.hasMore)
    }

    fun closeCursor(cursorId: String) {
        cursors.remove(cursorId)?.close()
    }

    fun cancel() {
        val statement = running ?: return
        runCatching { statement.cancel() }.onFailure { Log.error("Cancel failed", it) }
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
