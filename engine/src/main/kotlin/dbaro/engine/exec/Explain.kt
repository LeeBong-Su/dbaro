package dbaro.engine.exec

import java.sql.Connection
import java.sql.ResultSet
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong

/**
 * 예상 실행 계획 (문장은 실행하지 않는다). 실행해서 얻는 실제 계획은 [ActualPlan].
 * 결과: `columns`(표 머리글), `nodes`(id, parent(-1이면 맨 위), cells, 있으면 object), 있으면 `text`(DB 고유 형식의 계획 글).
 * `object`: 그 단계가 읽는 객체 {schema, name, type(TABLE, INDEX, ...), index(테이블을 읽을 때 쓴 인덱스)}. 앱이 객체 정보 창에 쓴다.
 * - Oracle·Tibero: EXPLAIN PLAN → PLAN_TABLE을 트리로, Oracle은 DBMS_XPLAN 글도 함께
 * - PostgreSQL: EXPLAIN (FORMAT JSON)을 트리로, 기본 EXPLAIN 글도 함께
 * - MySQL·MariaDB와 그 밖: EXPLAIN 결과 표 그대로 (MySQL 8은 FORMAT=TREE 글도 함께)
 */
object Explain {
    private val statementIds = AtomicLong(System.currentTimeMillis() % 1_000_000)

    fun explain(type: String, connection: Connection, sql: String): Map<String, Any?> = when (type) {
        "oracle", "tibero" -> oracle(connection, sql, withXplan = type == "oracle")
        "postgres" -> postgres(connection, sql)
        "mysql", "mariadb" -> withTableNames(sql, tabular(connection, sql, if (type == "mysql") "EXPLAIN FORMAT=TREE $sql" else null))
        else -> tabular(connection, sql, null)
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
        val columns = listOf("Operation", "Object", "Rows", "Bytes", "Cost", "Time", "Pstart", "Pstop", "Predicates")
        val nodes = rows.map { row ->
            val step = OracleStep(row)
            mapOf(
                "id" to (row.num("ID") ?: 0L),
                "parent" to (row.num("PARENT_ID") ?: -1L),
                "object" to step.planObject,
                "cells" to listOf(
                    step.operation,
                    step.objectName,
                    row.num("CARDINALITY")?.toString().orEmpty(),
                    row.num("BYTES")?.toString().orEmpty(),
                    row.num("COST")?.toString().orEmpty(),
                    row.num("TIME")?.let(::clock).orEmpty(),
                    row.str("PARTITION_START"),
                    row.str("PARTITION_STOP"),
                    step.predicates,
                ),
            )
        }
        return withoutEmptyColumns(columns, nodes, keep = 0) + ("text" to text)
    }

    internal fun Map<String, Any?>.str(name: String) = this[name]?.toString()?.trim().orEmpty()
    internal fun Map<String, Any?>.num(name: String) = (this[name] as? Number)?.toLong() ?: this[name]?.toString()?.toLongOrNull()

    /** PLAN_TABLE·V$SQL_PLAN 한 줄의 오퍼레이션, 객체, 조건 */
    internal class OracleStep(row: Map<String, Any?>) {
        val operation = listOf(row.str("OPERATION"), row.str("OPTIONS")).filter { it.isNotEmpty() }.joinToString(" ")
        private val owner = row.str("OBJECT_OWNER")
        private val name = row.str("OBJECT_NAME")
        val objectName = if (owner.isNotEmpty() && name.isNotEmpty()) "$owner.$name" else name
        val predicates = listOfNotNull(
            row.str("ACCESS_PREDICATES").takeIf { it.isNotEmpty() }?.let { "access($it)" },
            row.str("FILTER_PREDICATES").takeIf { it.isNotEmpty() }?.let { "filter($it)" },
        ).joinToString(" ")

        // OBJECT_TYPE: TABLE, TABLE (FIXED), INDEX, INDEX (UNIQUE), VIEW, MAT_VIEW ...
        private val objectType = row.str("OBJECT_TYPE").let { type ->
            when {
                type.startsWith("INDEX") -> "INDEX"
                type.startsWith("TABLE") -> "TABLE"
                type.isEmpty() && operation.startsWith("INDEX") -> "INDEX"
                type.isEmpty() && operation.startsWith("TABLE ACCESS") -> "TABLE"
                else -> type.ifEmpty { null }
            }
        }
        val planObject = if (name.isEmpty() || owner.isEmpty()) null else mapOf("schema" to owner, "name" to name, "type" to objectType)
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

    // EXPLAIN ANALYZE에만 있는 것
    private val postgresActualDetails = listOf(
        "Rows Removed by Filter", "Rows Removed by Join Filter", "Rows Removed by Index Recheck", "Heap Fetches",
        "Sort Method", "Sort Space Used", "Peak Memory Usage", "Batches",
    )

    /**
     * EXPLAIN (FORMAT JSON) → 트리. actual이면 EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)의 결과로
     * 실제 행 수(반복 전체), 반복 횟수, 시간(반복 전체, ms), 버퍼까지 보여 주고 planningMs, executionMs, resultRows도 넣는다.
     */
    internal fun postgresPlan(json: String, text: String?, actual: Boolean = false): Map<String, Any?> {
        val columns = if (actual) listOf("Operation", "Object", "Rows", "Actual Rows", "Loops", "Time (ms)", "Cost", "Buffers", "Details")
        else listOf("Operation", "Object", "Rows", "Width", "Cost", "Details")
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

            val details = (if (actual) postgresConditions + postgresActualDetails else postgresConditions).mapNotNull { key ->
                when (val value = plan[key]) {
                    null -> null
                    is List<*> -> "$key: ${value.joinToString(", ")}"
                    else -> "$key: $value"
                }
            }.joinToString("; ")

            fun number(key: String) = (plan[key] as? Number)
            val relationName = plan["Relation Name"]?.toString()
            val indexName = plan["Index Name"]?.toString()
            val planObject = when {
                relationName != null ->
                    mapOf("schema" to plan["Schema"]?.toString(), "name" to relationName, "type" to "TABLE", "index" to indexName)
                indexName != null -> mapOf("schema" to null, "name" to indexName, "type" to "INDEX") // Bitmap Index Scan
                else -> null
            }
            val startup = number("Startup Cost")
            val total = number("Total Cost")
            val cost = if (startup != null && total != null) String.format(Locale.ROOT, "%.2f..%.2f", startup.toDouble(), total.toDouble()) else ""

            val node = linkedMapOf<String, Any?>("id" to id, "parent" to parent, "object" to planObject)
            if (!actual) {
                node["cells"] = listOf(
                    operation,
                    objectName,
                    number("Plan Rows")?.toLong()?.toString().orEmpty(),
                    number("Plan Width")?.toLong()?.toString().orEmpty(),
                    cost,
                    details,
                )
            } else {
                // Actual Rows·시간은 반복 한 번의 평균이라 반복 횟수를 곱해 전체로 보인다
                val loops = number("Actual Loops")?.toLong()
                val estimated = number("Plan Rows")?.toDouble()
                val rows = number("Actual Rows")?.toDouble()
                val never = loops == 0L
                if (loops != null && loops > 0 && estimated != null && rows != null)
                    node["rows"] = mapOf("estimated" to Math.round(estimated * loops), "actual" to Math.round(rows * loops))
                node["cells"] = listOf(
                    operation,
                    objectName,
                    estimated?.let { Math.round(it).toString() }.orEmpty(),
                    if (never) "never executed" else rows?.let { Math.round(it * (loops ?: 1)).toString() }.orEmpty(),
                    loops?.toString().orEmpty(),
                    if (never) "" else number("Actual Total Time")?.let { milliseconds(it.toDouble() * (loops ?: 1)) }.orEmpty(),
                    cost,
                    postgresBuffers(plan),
                    details,
                )
            }
            nodes.add(node)
            (plan["Plans"] as? List<*>)?.forEach { child -> (child as? Map<*, *>)?.let { visit(it, id) } }
        }

        val root = Json.parse(json)
        val plans = if (root is List<*>) root else listOf(root)
        var timing: Map<String, Any?> = emptyMap()
        for (entry in plans) {
            val plan = (entry as? Map<*, *>)?.get("Plan") as? Map<*, *> ?: continue
            if (actual && timing.isEmpty()) {
                val loops = (plan["Actual Loops"] as? Number)?.toDouble() ?: 1.0
                timing = mapOf(
                    "planningMs" to ((entry as Map<*, *>)["Planning Time"] as? Number)?.toDouble(),
                    "executionMs" to (entry["Execution Time"] as? Number)?.toDouble(),
                    "resultRows" to (plan["Actual Rows"] as? Number)?.let { Math.round(it.toDouble() * loops) },
                    "actualRowsColumn" to "Actual Rows",
                )
            }
            visit(plan, -1)
        }
        return withoutEmptyColumns(columns, nodes, keep = 0) + ("text" to text) + timing
    }

    /** "shared hit=12 read=3 temp written=5" (0인 것은 뺀다) */
    private fun postgresBuffers(plan: Map<*, *>): String {
        fun part(prefix: String, keys: List<Pair<String, String>>): String? {
            val values = keys.mapNotNull { (key, label) ->
                (plan[key] as? Number)?.toLong()?.takeIf { it > 0 }?.let { "$label=$it" }
            }
            return if (values.isEmpty()) null else "$prefix ${values.joinToString(" ")}"
        }
        return listOfNotNull(
            part("shared", listOf("Shared Hit Blocks" to "hit", "Shared Read Blocks" to "read", "Shared Dirtied Blocks" to "dirtied", "Shared Written Blocks" to "written")),
            part("local", listOf("Local Hit Blocks" to "hit", "Local Read Blocks" to "read", "Local Written Blocks" to "written")),
            part("temp", listOf("Temp Read Blocks" to "read", "Temp Written Blocks" to "written")),
        ).joinToString(" ")
    }

    /** 밀리초: 소수 셋째 자리까지 */
    internal fun milliseconds(value: Double): String = String.format(Locale.ROOT, "%.3f", value)

    // ---------------------------------------------------------------------
    // MySQL·MariaDB와 그 밖 (H2 등): EXPLAIN 결과를 그대로

    private fun tabular(connection: Connection, sql: String, textSql: String?): Map<String, Any?> {
        val (columns, rows) = connection.createStatement().use { statement -> readTable(statement.executeQuery("EXPLAIN $sql")) }
        // 컬럼이 하나뿐이면 계획 글이다 (H2 등)
        if (columns.size == 1) return tabularPlan(columns, rows, null)

        val text = textSql?.let { query ->
            runCatching {
                connection.createStatement().use { statement ->
                    statement.executeQuery(query).use { rs -> buildList { while (rs.next()) add(rs.getString(1)) }.joinToString("\n") }
                }
            }.getOrNull()
        }
        return tabularPlan(columns, rows, text)
    }

    /** 결과 집합 전체를 문자열 표로 (닫는다) */
    internal fun readTable(rs: ResultSet): Pair<List<String>, List<List<String?>>> = rs.use {
        val meta = rs.metaData
        val labels = (1..meta.columnCount).map { meta.getColumnLabel(it) }
        labels to buildList { while (rs.next()) add(labels.indices.map { rs.getString(it + 1) }) }
    }

    /** EXPLAIN 결과 표 → 노드 (컬럼이 하나뿐이면 계획 글) */
    internal fun tabularPlan(columns: List<String>, rows: List<List<String?>>, text: String?): Map<String, Any?> {
        if (columns.size == 1) return mapOf("columns" to emptyList<String>(), "nodes" to emptyList<Any>(), "text" to rows.joinToString("\n") { it[0].orEmpty() })
        // MySQL·MariaDB: table(별칭일 수 있음)과 key(쓴 인덱스). <derived2>, <subquery2> 같은 임시 결과는 빼고
        val tableColumn = columns.indexOfFirst { it.equals("table", ignoreCase = true) }
        val keyColumn = columns.indexOfFirst { it.equals("key", ignoreCase = true) }
        val nodes = rows.mapIndexed { index, cells ->
            val table = cells.getOrNull(tableColumn)?.takeIf { it.isNotEmpty() && !it.startsWith("<") }
            val planObject = table?.let {
                mapOf("schema" to null, "name" to it, "type" to "TABLE", "index" to cells.getOrNull(keyColumn)?.takeIf { k -> k.isNotEmpty() })
            }
            mapOf("id" to index.toLong(), "parent" to -1L, "object" to planObject, "cells" to cells.map { it.orEmpty() })
        }
        return mapOf("columns" to columns, "nodes" to nodes, "text" to text)
    }

    // ---------------------------------------------------------------------
    // MySQL·MariaDB 계획의 테이블 이름은 문장에 쓴 별칭이다 (FROM orders o → o).
    // 객체 정보 창이 테이블을 찾을 수 있게 문장의 FROM·JOIN에서 별칭이 가리키는 테이블(과 스키마)로 바꾼다.

    @Suppress("UNCHECKED_CAST")
    internal fun withTableNames(sql: String, plan: Map<String, Any?>): Map<String, Any?> {
        val nodes = plan["nodes"] as? List<Map<String, Any?>> ?: return plan
        val tables = tableAliases(sql)
        if (tables.isEmpty()) return plan
        val resolved = nodes.map { node ->
            val target = node["object"] as? Map<String, Any?> ?: return@map node
            val (schema, table) = tables[(target["name"] as? String)?.lowercase()] ?: return@map node
            node + ("object" to target + mapOf("schema" to (schema ?: target["schema"]), "name" to table))
        }
        return plan + ("nodes" to resolved)
    }

    private val notAliases = setOf(
        "WHERE", "ON", "USING", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "NATURAL", "STRAIGHT_JOIN",
        "GROUP", "ORDER", "HAVING", "LIMIT", "OFFSET", "FETCH", "UNION", "EXCEPT", "INTERSECT", "WINDOW", "FOR", "LOCK",
        "SET", "VALUES", "VALUE", "SELECT", "FORCE", "USE", "IGNORE", "PARTITION", "LATERAL", "AS", "WITH", "RETURNING",
    )
    private val fromEnds = setOf("WHERE", "GROUP", "ORDER", "HAVING", "LIMIT", "UNION", "EXCEPT", "INTERSECT", "WINDOW", "SET")

    /** 문장의 테이블 참조: 별칭(과 스키마를 붙여 쓴 테이블 이름) 소문자 → (스키마, 테이블) */
    internal fun tableAliases(sql: String): Map<String, Pair<String?, String>> {
        val tokens = identifierTokens(sql)
        val result = HashMap<String, Pair<String?, String>>()
        val inFrom = ArrayList<Boolean>().apply { add(false) } // 괄호 깊이마다 FROM 목록 안인지
        fun word(i: Int) = tokens.getOrNull(i)?.takeIf { !it.quoted }?.text?.uppercase()
        fun name(i: Int) = tokens.getOrNull(i)?.takeIf { it.quoted || (it.text.first().let { c -> c.isLetter() || c == '_' } && it.text.uppercase() !in notAliases) }

        var i = 0
        while (i < tokens.size) {
            val token = tokens[i]
            val upper = word(i)
            when {
                token.text == "(" && !token.quoted -> inFrom.add(false)
                token.text == ")" && !token.quoted -> if (inFrom.size > 1) inFrom.removeAt(inFrom.size - 1)
                upper == "FROM" || upper == "JOIN" -> inFrom[inFrom.size - 1] = true
                upper in fromEnds -> inFrom[inFrom.size - 1] = false
            }
            val startsReference = upper == "FROM" || upper == "JOIN" || upper == "UPDATE" || upper == "INTO" ||
                (token.text == "," && !token.quoted && inFrom.last())
            if (startsReference) {
                val first = name(i + 1)
                if (first != null) {
                    var j = i + 2
                    var schema: String? = null
                    var table = first.text
                    if (tokens.getOrNull(j)?.text == "." && name(j + 1) != null) {
                        schema = first.text
                        table = tokens[j + 1].text
                        j += 2
                        result[table.lowercase()] = schema to table
                    }
                    if (word(j) == "AS") j++
                    name(j)?.let { alias -> result[alias.text.lowercase()] = schema to table }
                }
            }
            i++
        }
        return result
    }

    private class Token(val text: String, val quoted: Boolean)

    /** 이름·낱말과 괄호·쉼표·점만 남긴다 (문자열과 주석은 건너뛴다). `이름`·"이름"은 따옴표를 벗긴다 */
    private fun identifierTokens(sql: String): List<Token> {
        val tokens = ArrayList<Token>()
        var i = 0
        while (i < sql.length) {
            val c = sql[i]
            when {
                c.isWhitespace() -> i++
                sql.startsWith("--", i) || c == '#' -> i = sql.indexOf('\n', i).let { if (it < 0) sql.length else it + 1 }
                sql.startsWith("/*", i) -> i = sql.indexOf("*/", i + 2).let { if (it < 0) sql.length else it + 2 }
                c == '\'' -> {
                    i++
                    while (i < sql.length && sql[i] != '\'') i += if (sql[i] == '\\') 2 else 1
                    i++
                }
                c == '`' || c == '"' -> {
                    val text = StringBuilder()
                    i++
                    while (i < sql.length) {
                        if (sql[i] == c) {
                            if (sql.getOrNull(i + 1) == c) { text.append(c); i += 2; continue }
                            break
                        }
                        text.append(sql[i++])
                    }
                    i++
                    tokens.add(Token(text.toString(), quoted = true))
                }
                c.isLetterOrDigit() || c == '_' || c == '$' -> {
                    val start = i
                    while (i < sql.length && (sql[i].isLetterOrDigit() || sql[i] == '_' || sql[i] == '$')) i++
                    tokens.add(Token(sql.substring(start, i), quoted = false))
                }
                else -> {
                    tokens.add(Token(c.toString(), quoted = false))
                    i++
                }
            }
        }
        return tokens
    }

    // ---------------------------------------------------------------------

    internal fun readRows(rs: ResultSet): List<Map<String, Any?>> {
        val meta = rs.metaData
        val labels = (1..meta.columnCount).map { meta.getColumnLabel(it).uppercase() }
        return buildList {
            while (rs.next()) add(labels.withIndex().associate { (index, label) -> label to rs.getObject(index + 1) })
        }
    }

    /** 모든 노드에서 비어 있는 컬럼은 뺀다 (예: 분할 테이블이 없으면 Pstart/Pstop). */
    internal fun withoutEmptyColumns(columns: List<String>, nodes: List<Map<String, Any?>>, keep: Int): Map<String, Any?> {
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
