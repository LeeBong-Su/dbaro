package dbaro.engine.exec

import dbaro.engine.exec.Explain.num
import dbaro.engine.exec.Explain.str
import java.sql.Connection
import java.sql.SQLException
import java.sql.Statement

/**
 * 실제 실행 계획: 문장을 실행해서 단계마다 실제 행 수와 시간을 얻는다 (예상 계획은 [Explain]).
 *
 * 문장이 실제로 실행되므로 트랜잭션 안에서 실행하고 끝나면 되돌린다. 자동 커밋이면 잠시 끄고 전부 되돌리며,
 * 수동 커밋이면 세이브포인트까지만 되돌려 그 전에 하던 변경은 그대로 둔다. 그래서 DML을 분석해도 데이터는 바뀌지 않는다.
 *
 * 결과는 예상 계획과 같은 모양(columns, nodes, text)에 더해
 * - actual: true, rolledBack: DML이라 변경을 되돌렸음, resultRows: 돌려준(바꾼) 행 수, executionMs, planningMs(PostgreSQL)
 * - actualRowsColumn: 실제 행 수 컬럼 이름. 노드의 rows {estimated, actual}(반복 전체)로 앱이 예상과 크게 다른 곳을 강조한다
 * - textFirst: 트리보다 글을 먼저 보인다 (Oracle: DBA가 늘 보던 DISPLAY_CURSOR 'ADVANCED ALLSTATS LAST' 글)
 *
 * - Oracle: STATISTICS_LEVEL=ALL로 실행한 뒤 V$SQL_PLAN_STATISTICS_ALL(트리)과 DBMS_XPLAN.DISPLAY_CURSOR 'ADVANCED ALLSTATS LAST'(글).
 *   V$SESSION, V$SQL_PLAN_STATISTICS_ALL을 읽을 권한이 있어야 한다 (예: SELECT_CATALOG_ROLE)
 * - PostgreSQL: EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
 * - MySQL: EXPLAIN ANALYZE (8.0.18 이상). 트리 글을 단계별로 나눈다
 * - MariaDB: ANALYZE (EXPLAIN 표에 r_rows, r_filtered가 붙는다)
 * - 그 밖(H2 등): EXPLAIN ANALYZE의 글
 * - Tibero: 아직 지원하지 않는다
 */
object ActualPlan {
    private val queries = setOf("SELECT", "WITH", "VALUES", "TABLE")
    private val changes = setOf("INSERT", "UPDATE", "DELETE", "MERGE", "REPLACE")

    /**
     * [track]: 실행 중인 문장 (취소할 수 있게). 끝나면 null.
     */
    fun explain(type: String, connection: Connection, sql: String, timeoutSec: Int, track: (Statement?) -> Unit): Map<String, Any?> {
        val kind = statementKind(sql)
        if (kind !in queries && kind !in changes) {
            throw IllegalArgumentException(
                "The actual plan runs the statement, so it works only for SELECT, INSERT, UPDATE, DELETE and MERGE statements.",
            )
        }
        if (type == "tibero") {
            throw UnsupportedOperationException("The actual plan is not available for Tibero yet. Use the estimated plan (Ctrl+E).")
        }
        val runner = Runner(connection, timeoutSec, track)
        val plan = when (type) {
            "oracle" -> oracle(runner, connection, sql)
            "postgres" -> rolledBack(connection) {
                val json = runner.query("EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) $sql").second.joinToString("") { it[0].orEmpty() }
                Explain.postgresPlan(json, null, actual = true)
            }
            "mysql" -> rolledBack(connection) {
                val text = runner.query("EXPLAIN ANALYZE $sql").second.joinToString("\n") { it[0].orEmpty() }
                Explain.withTableNames(sql, mysqlPlan(text))
            }
            "mariadb" -> rolledBack(connection) {
                val (columns, rows) = runner.query("ANALYZE $sql")
                Explain.withTableNames(sql, mariadbPlan(columns, rows))
            }
            else -> rolledBack(connection) {
                val (columns, rows) = runner.query("EXPLAIN ANALYZE $sql")
                Explain.tabularPlan(columns, rows, null)
            }
        }
        val result = LinkedHashMap(plan)
        result["actual"] = true
        result["rolledBack"] = kind in changes
        if (result["executionMs"] == null) result["executionMs"] = runner.elapsedMs
        // DML의 맨 위 단계(ModifyTable, Update 등)는 행을 돌려주지 않아 0으로 나온다. 바꾼 행 수는 Oracle만 안다
        if (kind in changes && type != "oracle") result.remove("resultRows")
        return result
    }

    /** 문장의 첫 낱말 (주석·괄호는 건너뛴다). 대문자 */
    internal fun statementKind(sql: String): String {
        var i = 0
        while (i < sql.length) {
            i = when {
                sql[i].isWhitespace() || sql[i] == '(' -> i + 1
                sql.startsWith("--", i) -> sql.indexOf('\n', i).let { if (it < 0) sql.length else it + 1 }
                sql.startsWith("/*", i) -> sql.indexOf("*/", i + 2).let { if (it < 0) sql.length else it + 2 }
                else -> break
            }
        }
        return sql.substring(i).takeWhile { it.isLetter() }.uppercase()
    }

    /**
     * [block]을 트랜잭션 안에서 실행하고 되돌린다.
     * 자동 커밋이면 잠시 꺼서 전부 되돌리고, 수동 커밋이면 세이브포인트까지만 되돌린다.
     */
    internal fun <T> rolledBack(connection: Connection, block: () -> T): T {
        val autoCommit = connection.autoCommit
        if (autoCommit) connection.autoCommit = false
        try {
            val savepoint = if (autoCommit) null else connection.setSavepoint()
            try {
                return block()
            } finally {
                if (savepoint != null) connection.rollback(savepoint) else connection.rollback()
            }
        } finally {
            if (autoCommit) runCatching { connection.autoCommit = true }
        }
    }

    /** 문장을 실행한다. 실행 중인 문장을 알려 취소할 수 있게 하고, 걸린 시간을 잰다. */
    private class Runner(val connection: Connection, val timeoutSec: Int, val track: (Statement?) -> Unit) {
        var elapsedMs = 0.0
            private set

        private fun <T> run(block: (Statement) -> T): T = connection.createStatement().use { statement ->
            if (timeoutSec > 0) statement.queryTimeout = timeoutSec
            statement.fetchSize = 1000
            track(statement)
            val start = System.nanoTime()
            try {
                block(statement)
            } finally {
                elapsedMs += (System.nanoTime() - start) / 1_000_000.0
                track(null)
            }
        }

        /** 결과 표를 돌려주는 문장 (EXPLAIN ANALYZE 등) */
        fun query(sql: String): Pair<List<String>, List<List<String?>>> = run { Explain.readTable(it.executeQuery(sql)) }

        /** 분석할 문장 그대로. 결과 행은 모두 읽어서 버리고 행 수만 센다 (바꾼 행 수) */
        fun statement(sql: String): Long = run { statement ->
            if (statement.execute(sql)) {
                statement.resultSet.use { rs ->
                    var rows = 0L
                    while (rs.next()) rows++
                    rows
                }
            } else {
                statement.largeUpdateCountOrNull() ?: -1L
            }
        }

        private fun Statement.largeUpdateCountOrNull(): Long? =
            runCatching { largeUpdateCount }.getOrElse { updateCount.toLong() }.takeIf { it >= 0 }
    }

    // ---------------------------------------------------------------------
    // Oracle

    private fun oracle(runner: Runner, connection: Connection, sql: String): Map<String, Any?> {
        // 권한이 없으면 문장을 실행하기 전에 알린다
        try {
            connection.createStatement().use {
                it.executeQuery("SELECT 1 FROM V\$SESSION, V\$SQL_PLAN_STATISTICS_ALL WHERE 1 = 0").close()
            }
        } catch (e: SQLException) {
            throw SQLException(
                "The actual plan needs read access to V\$SESSION and V\$SQL_PLAN_STATISTICS_ALL " +
                    "(for example SELECT_CATALOG_ROLE). ${e.message}",
                e.sqlState, e.errorCode, e,
            )
        }
        // 단계마다 실제 행 수·시간을 모으도록 잠시 STATISTICS_LEVEL=ALL (끝나면 원래 값으로)
        val level = runCatching {
            connection.createStatement().use { statement ->
                statement.executeQuery("SELECT VALUE FROM V\$PARAMETER WHERE NAME = 'statistics_level'").use { rs ->
                    if (rs.next()) rs.getString(1) else null
                }
            }
        }.getOrNull()?.takeIf { it.matches(Regex("[A-Za-z]+")) } ?: "TYPICAL"
        connection.createStatement().use { it.execute("ALTER SESSION SET STATISTICS_LEVEL = ALL") }

        val resultRows: Long
        val sqlId: String
        val child: Long
        try {
            val found = rolledBack(connection) {
                val rows = runner.statement(sql)
                // 바로 앞에 실행한 문장 = 분석한 문장 (그 사이에 다른 문장을 실행하면 안 된다)
                connection.createStatement().use { statement ->
                    statement.executeQuery(
                        "SELECT PREV_SQL_ID, PREV_CHILD_NUMBER FROM V\$SESSION WHERE SID = SYS_CONTEXT('USERENV', 'SID')",
                    ).use { rs ->
                        if (!rs.next() || rs.getString(1) == null) throw SQLException("Could not find the statement that was run.")
                        Triple(rows, rs.getString(1), rs.getLong(2))
                    }
                }
            }
            resultRows = found.first
            sqlId = found.second
            child = found.third
        } finally {
            runCatching { connection.createStatement().use { it.execute("ALTER SESSION SET STATISTICS_LEVEL = $level") } }
        }

        val steps = connection.prepareStatement(
            "SELECT * FROM V\$SQL_PLAN_STATISTICS_ALL WHERE SQL_ID = ? AND CHILD_NUMBER = ? ORDER BY ID",
        ).use { query ->
            query.setString(1, sqlId)
            query.setLong(2, child)
            query.executeQuery().use { Explain.readRows(it) }
        }
        if (steps.isEmpty()) throw SQLException("The plan of the statement ($sqlId) is no longer in the shared pool.")
        // SQL*Plus에서 DISPLAY_CURSOR(NULL, NULL, 'ADVANCED ALLSTATS LAST')로 보던 그대로의 글:
        // 계획 표에 Query Block Name / Object Alias, Outline Data, Peeked Binds, Predicate Information,
        // Column Projection Information, Hint Report, Note까지. 실패하면 이유를 글로 남기고 트리를 먼저 보인다
        val text = runCatching {
            connection.prepareStatement(
                "SELECT PLAN_TABLE_OUTPUT FROM TABLE(DBMS_XPLAN.DISPLAY_CURSOR(?, ?, 'ADVANCED ALLSTATS LAST'))",
            ).use { query ->
                query.setString(1, sqlId)
                query.setLong(2, child)
                query.executeQuery().use { rs -> buildList { while (rs.next()) add(rs.getString(1) ?: "") }.joinToString("\n") }
            }
        }
        return oraclePlan(steps, text.getOrElse { "-- DBMS_XPLAN.DISPLAY_CURSOR failed: ${it.message}" }) +
            mapOf("resultRows" to resultRows.takeIf { it >= 0 }, "textFirst" to text.isSuccess)
    }

    /**
     * V$SQL_PLAN_STATISTICS_ALL 행(컬럼 이름은 대문자) → 트리. LAST_*는 마지막 실행의 값.
     * DBMS_XPLAN 'ALLSTATS LAST'와 같은 값: Starts, E-Rows, A-Rows, A-Time, Buffers, Reads, Writes,
     * OMem·1Mem(작업 영역 예상 크기), Used-Mem(쓴 메모리, 괄호 안은 디스크를 거친 횟수: 0이면 메모리 안에서 끝남), Used-Tmp
     */
    internal fun oraclePlan(rows: List<Map<String, Any?>>, text: String?): Map<String, Any?> {
        val columns = listOf(
            "Operation", "Object", "Starts", "E-Rows", "A-Rows", "A-Time (ms)", "Buffers", "Reads", "Writes",
            "OMem", "1Mem", "Used-Mem", "Used-Tmp", "Cost", "Pstart", "Pstop", "Predicates",
        )
        val nodes = rows.map { row ->
            val step = Explain.OracleStep(row)
            val starts = row.num("LAST_STARTS")
            val estimated = row.num("CARDINALITY")
            val actual = row.num("LAST_OUTPUT_ROWS")
            val buffers = listOfNotNull(row.num("LAST_CR_BUFFER_GETS"), row.num("LAST_CU_BUFFER_GETS")).takeIf { it.isNotEmpty() }?.sum()
            val node = linkedMapOf<String, Any?>(
                "id" to (row.num("ID") ?: 0L),
                "parent" to (row.num("PARENT_ID") ?: -1L),
                "object" to step.planObject,
                "cells" to listOf(
                    step.operation,
                    step.objectName,
                    starts?.toString().orEmpty(),
                    estimated?.toString().orEmpty(),
                    actual?.toString().orEmpty(),
                    // 마이크로초
                    row.num("LAST_ELAPSED_TIME")?.let { Explain.milliseconds(it / 1000.0) }.orEmpty(),
                    buffers?.toString().orEmpty(),
                    row.num("LAST_DISK_READS")?.toString().orEmpty(),
                    row.num("LAST_DISK_WRITES")?.toString().orEmpty(),
                    // 모두 바이트 (Oracle Free 26ai에서 DISPLAY_CURSOR와 맞춰 봄). 쓴 메모리가 0이면 DISPLAY_CURSOR처럼 비운다
                    row.num("ESTIMATED_OPTIMAL_SIZE")?.let { size(it) }.orEmpty(),
                    row.num("ESTIMATED_ONEPASS_SIZE")?.let { size(it) }.orEmpty(),
                    row.num("LAST_MEMORY_USED")?.takeIf { it > 0 }?.let { "${size(it)} (${passes(row.str("LAST_EXECUTION"))})" }.orEmpty(),
                    row.num("LAST_TEMPSEG_SIZE")?.let { size(it) }.orEmpty(),
                    row.num("COST")?.toString().orEmpty(),
                    row.str("PARTITION_START"),
                    row.str("PARTITION_STOP"),
                    step.predicates,
                ),
            )
            // E-Rows는 한 번 시작할 때의 예상이라 Starts를 곱해 A-Rows(전체)와 비교한다
            if (starts != null && starts > 0 && estimated != null && actual != null)
                node["rows"] = mapOf("estimated" to estimated * starts, "actual" to actual)
            node
        }
        return Explain.withoutEmptyColumns(columns, nodes, keep = 0) + mapOf("text" to text, "actualRowsColumn" to "A-Rows")
    }

    /** DBMS_XPLAN처럼 바이트를 짧게: 2048, 1161K, 73M, 20G */
    internal fun size(bytes: Long): String = when {
        bytes < 10L * 1024 -> "$bytes"
        bytes < 10L * 1024 * 1024 -> "${bytes / 1024}K"
        bytes < 10L * 1024 * 1024 * 1024 -> "${bytes / 1024 / 1024}M"
        else -> "${bytes / 1024 / 1024 / 1024}G"
    }

    /** LAST_EXECUTION(OPTIMAL, 1 PASS, 3 PASSES) → 디스크를 거친 횟수 */
    private fun passes(execution: String): String = when {
        execution.isEmpty() || execution.equals("OPTIMAL", ignoreCase = true) -> "0"
        else -> execution.takeWhile { it.isDigit() }.ifEmpty { "0" }
    }

    // ---------------------------------------------------------------------
    // MySQL: EXPLAIN ANALYZE의 트리 글
    //   -> Nested loop inner join  (cost=4.95 rows=9) (actual time=0.153..0.200 rows=9 loops=1)
    //       -> Table scan on t1  (cost=1.15 rows=9) (actual time=0.081..0.093 rows=9 loops=1)

    private val mysqlCost = Regex("""\s*\(cost=([0-9.eE+-]+)(?:\.\.([0-9.eE+-]+))? rows=([0-9.eE+-]+)\)""")
    private val mysqlActual = Regex("""\s*\(actual time=([0-9.eE+-]+)\.\.([0-9.eE+-]+) rows=([0-9.eE+-]+) loops=([0-9]+)\)""")
    private val mysqlNever = Regex("""\s*\(never executed\)""")

    // "Table scan on t", "Index lookup on t2 using idx (a=t1.a)", "Single-row covering index lookup on d using PRIMARY (...)"
    private val mysqlObject = Regex("""^[A-Za-z -]*?\b(?:scan|lookup|search)\b[A-Za-z -]*? on (\S+)(?: using (\S+))?""")

    internal fun mysqlPlan(text: String): Map<String, Any?> {
        val columns = listOf("Operation", "Rows", "Actual Rows", "Loops", "Time (ms)", "Cost")
        val nodes = ArrayList<MutableMap<String, Any?>>()
        val parents = ArrayList<Pair<Int, Long>>() // (들여쓰기, id)
        var resultRows: Long? = null

        for (line in text.lines()) {
            if (line.isBlank()) continue
            val arrow = line.indexOf("-> ")
            if (arrow < 0 || line.substring(0, arrow).isNotBlank()) {
                // 앞 단계 설명이 다음 줄로 이어진 것
                nodes.lastOrNull()?.let { node ->
                    @Suppress("UNCHECKED_CAST")
                    val cells = (node["cells"] as List<String>).toMutableList()
                    cells[0] = cells[0] + " " + line.trim()
                    node["cells"] = cells
                }
                continue
            }
            var operation = line.substring(arrow + 3)
            val cost = mysqlCost.find(operation)
            val actual = mysqlActual.find(operation)
            val never = mysqlNever.containsMatchIn(operation)
            operation = operation.replace(mysqlCost, "").replace(mysqlActual, "").replace(mysqlNever, "").trim()

            while (parents.isNotEmpty() && parents.last().first >= arrow) parents.removeAt(parents.size - 1)
            val id = nodes.size.toLong()
            val parent = parents.lastOrNull()?.second ?: -1L
            parents.add(arrow to id)

            // 반복 한 번의 평균이라 반복 횟수를 곱해 전체로 보인다
            val loops = actual?.groupValues?.get(4)?.toLongOrNull()
            val estimated = cost?.groupValues?.get(3)?.toDoubleOrNull()
            val rows = actual?.groupValues?.get(3)?.toDoubleOrNull()
            val last = actual?.groupValues?.get(2)?.toDoubleOrNull()
            val totalRows = if (rows != null && loops != null) Math.round(rows * loops) else null
            if (id == 0L) resultRows = totalRows

            val node = linkedMapOf<String, Any?>("id" to id, "parent" to parent)
            mysqlObject.find(operation)?.let { match ->
                val table = match.groupValues[1].trim('`')
                if (!table.startsWith("<")) {
                    node["object"] = mapOf(
                        "schema" to null, "name" to table, "type" to "TABLE",
                        "index" to match.groupValues[2].trim('`').ifEmpty { null },
                    )
                }
            }
            if (estimated != null && totalRows != null && loops != null && loops > 0)
                node["rows"] = mapOf("estimated" to Math.round(estimated * loops), "actual" to totalRows)
            node["cells"] = listOf(
                operation,
                estimated?.let { Math.round(it).toString() }.orEmpty(),
                if (never) "never executed" else totalRows?.toString().orEmpty(),
                loops?.toString().orEmpty(),
                if (last != null && loops != null) Explain.milliseconds(last * loops) else "",
                cost?.let { match -> listOfNotNull(match.groupValues[1], match.groupValues[2].ifEmpty { null }).joinToString("..") }.orEmpty(),
            )
            nodes.add(node)
        }
        return Explain.withoutEmptyColumns(columns, nodes, keep = 0) +
            mapOf("text" to text, "resultRows" to resultRows, "actualRowsColumn" to "Actual Rows")
    }

    // ---------------------------------------------------------------------
    // MariaDB: ANALYZE는 EXPLAIN 표에 r_rows(실제 행 수, 반복 평균), r_filtered가 붙는다

    internal fun mariadbPlan(columns: List<String>, rows: List<List<String?>>): Map<String, Any?> {
        val plan = Explain.tabularPlan(columns, rows, null)
        val estimatedColumn = columns.indexOfFirst { it.equals("rows", ignoreCase = true) }
        val actualColumn = columns.indexOfFirst { it.equals("r_rows", ignoreCase = true) }
        if (estimatedColumn < 0 || actualColumn < 0) return plan
        @Suppress("UNCHECKED_CAST")
        val nodes = (plan["nodes"] as List<Map<String, Any?>>).mapIndexed { index, node ->
            val estimated = rows[index].getOrNull(estimatedColumn)?.toDoubleOrNull()
            val actual = rows[index].getOrNull(actualColumn)?.toDoubleOrNull()
            if (estimated == null || actual == null) node
            else node + ("rows" to mapOf("estimated" to Math.round(estimated), "actual" to Math.round(actual)))
        }
        return plan + mapOf("nodes" to nodes, "actualRowsColumn" to columns[actualColumn])
    }
}
