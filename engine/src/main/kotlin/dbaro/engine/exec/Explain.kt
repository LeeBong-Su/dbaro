package dbaro.engine.exec

import java.sql.Connection
import java.sql.ResultSet
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong

/**
 * 예상 실행 계획 (문장은 실행하지 않는다).
 * 결과: `columns`(표 머리글), `nodes`(id, parent(-1이면 맨 위), cells), 있으면 `text`(DB 고유 형식의 계획 글).
 * - Oracle·Tibero: EXPLAIN PLAN → PLAN_TABLE을 트리로, Oracle은 DBMS_XPLAN 글도 함께
 * - PostgreSQL: EXPLAIN (FORMAT JSON)을 트리로, 기본 EXPLAIN 글도 함께
 * - MySQL·MariaDB와 그 밖: EXPLAIN 결과 표 그대로 (MySQL 8은 FORMAT=TREE 글도 함께)
 */
object Explain {
    private val statementIds = AtomicLong(System.currentTimeMillis() % 1_000_000)

    fun explain(type: String, connection: Connection, sql: String): Map<String, Any?> = when (type) {
        "oracle", "tibero" -> oracle(connection, sql, withXplan = type == "oracle")
        "postgres" -> postgres(connection, sql)
        else -> tabular(connection, sql, if (type == "mysql") "EXPLAIN FORMAT=TREE $sql" else null)
    }

    // ---------------------------------------------------------------------
    // Oracle·Tibero

    private fun oracle(connection: Connection, sql: String, withXplan: Boolean): Map<String, Any?> {
        val statementId = "DBARO_${statementIds.incrementAndGet()}"
        connection.createStatement().use { it.execute("EXPLAIN PLAN SET STATEMENT_ID = '$statementId' FOR $sql") }
        try {
            val rows = ArrayList<Map<String, Any?>>()
            connection.prepareStatement("SELECT * FROM PLAN_TABLE WHERE STATEMENT_ID = ? ORDER BY ID").use { query ->
                query.setString(1, statementId)
                query.executeQuery().use { rows.addAll(readRows(it)) }
            }
            val text = if (!withXplan) null else runCatching {
                connection.prepareStatement(
                    "SELECT PLAN_TABLE_OUTPUT FROM TABLE(DBMS_XPLAN.DISPLAY('PLAN_TABLE', ?, 'TYPICAL'))",
                ).use { query ->
                    query.setString(1, statementId)
                    query.executeQuery().use { rs -> buildList { while (rs.next()) add(rs.getString(1) ?: "") }.joinToString("\n") }
                }
            }.getOrNull()
            return oraclePlan(rows, text)
        } finally {
            runCatching {
                connection.prepareStatement("DELETE FROM PLAN_TABLE WHERE STATEMENT_ID = ?").use {
                    it.setString(1, statementId)
                    it.executeUpdate()
                }
            }
        }
    }

    /** PLAN_TABLE 행(컬럼 이름은 대문자) → 트리. Tibero처럼 컬럼이 조금 달라도 있는 것만 쓴다. */
    internal fun oraclePlan(rows: List<Map<String, Any?>>, text: String?): Map<String, Any?> {
        fun Map<String, Any?>.str(name: String) = this[name]?.toString()?.trim().orEmpty()
        fun Map<String, Any?>.num(name: String) = (this[name] as? Number)?.toLong() ?: this[name]?.toString()?.toLongOrNull()

        val columns = listOf("Operation", "Object", "Rows", "Bytes", "Cost", "Time", "Pstart", "Pstop", "Predicates")
        val nodes = rows.map { row ->
            val operation = listOf(row.str("OPERATION"), row.str("OPTIONS")).filter { it.isNotEmpty() }.joinToString(" ")
            val owner = row.str("OBJECT_OWNER")
            val name = row.str("OBJECT_NAME")
            val predicates = listOfNotNull(
                row.str("ACCESS_PREDICATES").takeIf { it.isNotEmpty() }?.let { "access($it)" },
                row.str("FILTER_PREDICATES").takeIf { it.isNotEmpty() }?.let { "filter($it)" },
            ).joinToString(" ")
            mapOf(
                "id" to (row.num("ID") ?: 0L),
                "parent" to (row.num("PARENT_ID") ?: -1L),
                "cells" to listOf(
                    operation,
                    if (owner.isNotEmpty() && name.isNotEmpty()) "$owner.$name" else name,
                    row.num("CARDINALITY")?.toString().orEmpty(),
                    row.num("BYTES")?.toString().orEmpty(),
                    row.num("COST")?.toString().orEmpty(),
                    row.num("TIME")?.let(::clock).orEmpty(),
                    row.str("PARTITION_START"),
                    row.str("PARTITION_STOP"),
                    predicates,
                ),
            )
        }
        return withoutEmptyColumns(columns, nodes, keep = 0) + ("text" to text)
    }

    private fun clock(seconds: Long): String =
        String.format(Locale.ROOT, "%02d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60)

    // ---------------------------------------------------------------------
    // PostgreSQL

    private fun postgres(connection: Connection, sql: String): Map<String, Any?> {
        val json = connection.createStatement().use { statement ->
            statement.executeQuery("EXPLAIN (FORMAT JSON) $sql").use { rs ->
                buildString { while (rs.next()) append(rs.getString(1)) }
            }
        }
        val text = runCatching {
            connection.createStatement().use { statement ->
                statement.executeQuery("EXPLAIN $sql").use { rs -> buildList { while (rs.next()) add(rs.getString(1)) }.joinToString("\n") }
            }
        }.getOrNull()
        return postgresPlan(json, text)
    }

    private val postgresConditions = listOf(
        "Index Cond", "Recheck Cond", "Hash Cond", "Merge Cond", "Join Filter", "Filter", "One-Time Filter",
        "Sort Key", "Group Key", "Presorted Key",
    )

    internal fun postgresPlan(json: String, text: String?): Map<String, Any?> {
        val columns = listOf("Operation", "Object", "Rows", "Width", "Cost", "Details")
        val nodes = ArrayList<Map<String, Any?>>()
        var nextId = 0L

        fun visit(plan: Map<*, *>, parent: Long) {
            val id = nextId++
            val nodeType = plan["Node Type"]?.toString().orEmpty()
            val qualifiers = listOfNotNull(
                plan["Join Type"]?.toString()?.takeIf { it != "Inner" },
                plan["Strategy"]?.toString()?.takeIf { it != "Plain" },
                plan["Scan Direction"]?.toString()?.takeIf { it == "Backward" },
                plan["Parent Relationship"]?.toString()?.takeIf { it == "InitPlan" || it == "SubPlan" },
            )
            val operation = if (qualifiers.isEmpty()) nodeType else "$nodeType (${qualifiers.joinToString(", ")})"

            val relation = plan["Relation Name"]?.toString()?.let { name ->
                plan["Schema"]?.toString()?.let { "$it.$name" } ?: name
            }
            val alias = plan["Alias"]?.toString()
            val objectName = listOfNotNull(
                relation?.let { if (alias != null && alias != plan["Relation Name"]) "$it $alias" else it },
                plan["Index Name"]?.toString()?.let { if (relation == null) it else "using $it" },
                plan["CTE Name"]?.toString()?.let { "CTE $it" },
                plan["Function Name"]?.toString(),
            ).joinToString(" ")

            val details = postgresConditions.mapNotNull { key ->
                when (val value = plan[key]) {
                    null -> null
                    is List<*> -> "$key: ${value.joinToString(", ")}"
                    else -> "$key: $value"
                }
            }.joinToString("; ")

            fun number(key: String) = (plan[key] as? Number)
            val startup = number("Startup Cost")
            val total = number("Total Cost")
            val cost = if (startup != null && total != null) String.format(Locale.ROOT, "%.2f..%.2f", startup.toDouble(), total.toDouble()) else ""

            nodes.add(
                mapOf(
                    "id" to id,
                    "parent" to parent,
                    "cells" to listOf(
                        operation,
                        objectName,
                        number("Plan Rows")?.toLong()?.toString().orEmpty(),
                        number("Plan Width")?.toLong()?.toString().orEmpty(),
                        cost,
                        details,
                    ),
                ),
            )
            (plan["Plans"] as? List<*>)?.forEach { child -> (child as? Map<*, *>)?.let { visit(it, id) } }
        }

        val root = Json.parse(json)
        val plans = if (root is List<*>) root else listOf(root)
        for (entry in plans) {
            val plan = (entry as? Map<*, *>)?.get("Plan") as? Map<*, *> ?: continue
            visit(plan, -1)
        }
        return withoutEmptyColumns(columns, nodes, keep = 0) + ("text" to text)
    }

    // ---------------------------------------------------------------------
    // MySQL·MariaDB와 그 밖 (H2 등): EXPLAIN 결과를 그대로

    private fun tabular(connection: Connection, sql: String, textSql: String?): Map<String, Any?> {
        val (columns, rows) = connection.createStatement().use { statement ->
            statement.executeQuery("EXPLAIN $sql").use { rs ->
                val meta = rs.metaData
                val labels = (1..meta.columnCount).map { meta.getColumnLabel(it) }
                labels to buildList { while (rs.next()) add(labels.indices.map { rs.getString(it + 1) }) }
            }
        }
        // 컬럼이 하나뿐이면 계획 글이다 (H2 등)
        if (columns.size == 1) return mapOf("columns" to emptyList<String>(), "nodes" to emptyList<Any>(), "text" to rows.joinToString("\n") { it[0].orEmpty() })

        val text = textSql?.let { query ->
            runCatching {
                connection.createStatement().use { statement ->
                    statement.executeQuery(query).use { rs -> buildList { while (rs.next()) add(rs.getString(1)) }.joinToString("\n") }
                }
            }.getOrNull()
        }
        val nodes = rows.mapIndexed { index, cells -> mapOf("id" to index.toLong(), "parent" to -1L, "cells" to cells.map { it.orEmpty() }) }
        return mapOf("columns" to columns, "nodes" to nodes, "text" to text)
    }

    // ---------------------------------------------------------------------

    private fun readRows(rs: ResultSet): List<Map<String, Any?>> {
        val meta = rs.metaData
        val labels = (1..meta.columnCount).map { meta.getColumnLabel(it).uppercase() }
        return buildList {
            while (rs.next()) add(labels.withIndex().associate { (index, label) -> label to rs.getObject(index + 1) })
        }
    }

    /** 모든 노드에서 비어 있는 컬럼은 뺀다 (예: 분할 테이블이 없으면 Pstart/Pstop). */
    private fun withoutEmptyColumns(columns: List<String>, nodes: List<Map<String, Any?>>, keep: Int): Map<String, Any?> {
        val used = columns.indices.filter { column ->
            column == keep || nodes.any { ((it["cells"] as List<*>)[column] as String).isNotEmpty() }
        }
        return mapOf(
            "columns" to used.map { columns[it] },
            "nodes" to nodes.map { node -> node + ("cells" to used.map { (node["cells"] as List<*>)[it] }) },
        )
    }
}

/** 실행 계획 JSON(PostgreSQL)만 읽는 작은 JSON 파서. 객체는 순서를 지키는 Map, 숫자는 Long 또는 Double. */
internal object Json {
    fun parse(text: String): Any? {
        val reader = Reader(text)
        val value = reader.value()
        reader.skipSpace()
        require(reader.atEnd()) { "Unexpected text after JSON at ${reader.pos}" }
        return value
    }

    private class Reader(val text: String) {
        var pos = 0

        fun atEnd() = pos >= text.length

        fun skipSpace() {
            while (pos < text.length && text[pos].isWhitespace()) pos++
        }

        fun value(): Any? {
            skipSpace()
            require(!atEnd()) { "Unexpected end of JSON" }
            return when (val c = text[pos]) {
                '{' -> obj()
                '[' -> array()
                '"' -> string()
                't' -> literal("true", true)
                'f' -> literal("false", false)
                'n' -> literal("null", null)
                else -> if (c == '-' || c.isDigit()) number() else throw IllegalArgumentException("Unexpected '$c' at $pos")
            }
        }

        private fun literal(word: String, value: Any?): Any? {
            require(text.startsWith(word, pos)) { "Expected $word at $pos" }
            pos += word.length
            return value
        }

        private fun obj(): Map<String, Any?> {
            pos++ // {
            val map = LinkedHashMap<String, Any?>()
            skipSpace()
            if (text[pos] == '}') return map.also { pos++ }
            while (true) {
                skipSpace()
                val key = string()
                skipSpace()
                require(text[pos] == ':') { "Expected ':' at $pos" }
                pos++
                map[key] = value()
                skipSpace()
                when (text[pos++]) {
                    ',' -> continue
                    '}' -> return map
                    else -> throw IllegalArgumentException("Expected ',' or '}' at ${pos - 1}")
                }
            }
        }

        private fun array(): List<Any?> {
            pos++ // [
            val list = ArrayList<Any?>()
            skipSpace()
            if (text[pos] == ']') return list.also { pos++ }
            while (true) {
                list.add(value())
                skipSpace()
                when (text[pos++]) {
                    ',' -> continue
                    ']' -> return list
                    else -> throw IllegalArgumentException("Expected ',' or ']' at ${pos - 1}")
                }
            }
        }

        private fun string(): String {
            require(text[pos] == '"') { "Expected '\"' at $pos" }
            pos++
            val out = StringBuilder()
            while (true) {
                val c = text[pos++]
                when (c) {
                    '"' -> return out.toString()
                    '\\' -> when (val e = text[pos++]) {
                        'n' -> out.append('\n')
                        't' -> out.append('\t')
                        'r' -> out.append('\r')
                        'b' -> out.append('\b')
                        'f' -> out.append('\u000C')
                        'u' -> {
                            out.append(text.substring(pos, pos + 4).toInt(16).toChar())
                            pos += 4
                        }
                        else -> out.append(e) // " \ /
                    }
                    else -> out.append(c)
                }
            }
        }

        private fun number(): Number {
            val start = pos
            while (pos < text.length && (text[pos].isDigit() || text[pos] in "+-.eE")) pos++
            val token = text.substring(start, pos)
            return token.toLongOrNull() ?: token.toDouble()
        }
    }
}
