package dbaro.engine.exec

import java.sql.Connection
import java.sql.DatabaseMetaData
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

/**
 * 리버스 엔지니어링: 스키마 하나의 구조(모델)와 스키마 전체 DDL.
 *
 * 모델(테이블·뷰, 컬럼, 기본 키, 고유 키, 외래 키, 인덱스, 코멘트, 행 수 추정)은 스키마 단위로 한 번에 읽는다.
 * 테이블마다 JDBC 메타데이터를 부르면 테이블이 수백 개일 때 수십 초가 걸려서, 키·인덱스는 DB별 시스템 뷰로 읽는다.
 * - Oracle·Tibero: ALL_TABLES·ALL_VIEWS·ALL_TAB_COMMENTS, ALL_CONSTRAINTS·ALL_CONS_COLUMNS, ALL_INDEXES·ALL_IND_COLUMNS
 * - PostgreSQL: pg_class(파티션 자식은 빼고 부모만), pg_constraint, pg_index
 * - MySQL·MariaDB: information_schema.TABLES·KEY_COLUMN_USAGE·TABLE_CONSTRAINTS·STATISTICS
 * - 그 밖: JDBC 메타데이터 (테이블마다)
 * 컬럼은 모두 JDBC getColumns 한 번으로 읽는다.
 *
 * 스키마 DDL은 객체마다 [Ddl.script]를 이어 붙인다: 시퀀스 → 테이블(외래 키가 가리키는 테이블이 먼저) → 뷰 → 루틴.
 */
object Reverse {
    private const val MAX_TABLES = 3_000

    private class Key(val name: String?) {
        val columns = ArrayList<String>()
    }

    private class ForeignKey(val name: String, val refSchema: String?, val refTable: String?) {
        val columns = ArrayList<String>()
        val refColumns = ArrayList<String>()
    }

    private class Index(val name: String, val unique: Boolean) {
        val columns = ArrayList<String>()
    }

    private class Table(val name: String, val type: String, val remarks: String?, val rows: Long?) {
        val columns = ArrayList<Map<String, Any?>>()
        var primaryKey: Key? = null
        val uniqueKeys = LinkedHashMap<String, Key>()
        val foreignKeys = LinkedHashMap<String, ForeignKey>()
        val indexes = LinkedHashMap<String, Index>()

        fun toMap(): Map<String, Any?> {
            val pk = primaryKey?.columns.orEmpty().toSet()
            val fk = foreignKeys.values.flatMap { it.columns }.toSet()
            return mapOf(
                "name" to name,
                "type" to type,
                "remarks" to remarks?.takeIf { it.isNotBlank() },
                "rows" to rows,
                "columns" to columns.map { it + mapOf("primaryKey" to (it["name"] in pk), "foreignKey" to (it["name"] in fk)) },
                "primaryKey" to primaryKey?.let { mapOf("name" to it.name, "columns" to it.columns) },
                "uniqueKeys" to uniqueKeys.values.map { mapOf("name" to it.name, "columns" to it.columns) },
                "foreignKeys" to foreignKeys.values.map {
                    mapOf("name" to it.name, "columns" to it.columns, "refSchema" to it.refSchema, "refTable" to it.refTable,
                        "refColumns" to it.refColumns)
                },
                "indexes" to indexes.values.map { mapOf("name" to it.name, "unique" to it.unique, "columns" to it.columns) },
            )
        }
    }

    private fun oracleFamily(type: String) = type == "oracle" || type == "tibero"
    private fun mysqlFamily(type: String) = type == "mysql" || type == "mariadb"

    // ---------------------------------------------------------------------
    // 모델

    /**
     * 스키마의 모델. [only]가 있으면 그 이름의 테이블·뷰만 (정확한 이름).
     * 결과: schema, tables: [{name, type(TABLE/VIEW), remarks, rows, columns: [{name, type, nullable, default, remarks,
     * primaryKey, foreignKey}], primaryKey: {name, columns}, uniqueKeys, foreignKeys: [{name, columns, refSchema, refTable,
     * refColumns}], indexes: [{name, unique, columns}]}], truncated
     */
    fun model(type: String, c: Connection, schema: String, only: Set<String>?, includeViews: Boolean): Map<String, Any?> {
        val all = tables(type, c, schema, includeViews)
        val tables = LinkedHashMap<String, Table>()
        for ((name, table) in all) {
            if (only != null && name !in only) continue
            if (tables.size >= MAX_TABLES) break
            tables[name] = table
        }
        columns(type, c, schema, tables)
        keys(type, c, schema, tables)
        indexes(type, c, schema, tables)
        return mapOf(
            "schema" to schema,
            "tables" to tables.values.sortedBy { it.name }.map { it.toMap() },
            "truncated" to (tables.size >= MAX_TABLES),
        )
    }

    private fun tables(type: String, c: Connection, schema: String, includeViews: Boolean): LinkedHashMap<String, Table> {
        val result = LinkedHashMap<String, Table>()
        fun add(name: String?, kind: String, remarks: String?, rows: Long?) {
            if (name != null) result[name] = Table(name, kind, remarks, rows)
        }
        when {
            oracleFamily(type) -> {
                // 휴지통(BIN$)과 IOT 오버플로는 뺀다. Tibero는 컬럼이 달라 기본 조건만 쓴다.
                val filter = if (type == "oracle") "AND t.iot_name IS NULL AND t.nested = 'NO' AND t.secondary = 'N'" else ""
                rows(c, """
                    SELECT t.table_name, cm.comments, t.num_rows
                      FROM all_tables t
                      LEFT JOIN all_tab_comments cm ON cm.owner = t.owner AND cm.table_name = t.table_name
                     WHERE t.owner = ? AND t.table_name NOT LIKE 'BIN${'$'}%' $filter
                """, schema).forEach { add(it[0] as String?, "TABLE", it[1] as String?, (it[2] as Number?)?.toLong()) }
                if (includeViews) {
                    rows(c, """
                        SELECT v.view_name, cm.comments
                          FROM all_views v
                          LEFT JOIN all_tab_comments cm ON cm.owner = v.owner AND cm.table_name = v.view_name
                         WHERE v.owner = ?
                    """, schema).forEach { add(it[0] as String?, "VIEW", it[1] as String?, null) }
                }
            }
            type == "postgres" -> {
                val kinds = if (includeViews) "'r', 'p', 'f', 'v', 'm'" else "'r', 'p', 'f'"
                rows(c, """
                    SELECT c.relname, c.relkind, obj_description(c.oid, 'pg_class'), GREATEST(c.reltuples, 0)::bigint
                      FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
                     WHERE n.nspname = ? AND c.relkind IN ($kinds) AND NOT c.relispartition
                """, schema).forEach {
                    val kind = when (it[1]?.toString()) {
                        "v" -> "VIEW"
                        "m" -> "MATERIALIZED VIEW"
                        "p" -> "PARTITIONED TABLE"
                        "f" -> "FOREIGN TABLE"
                        else -> "TABLE"
                    }
                    add(it[0] as String?, kind, it[2] as String?, if (kind.endsWith("VIEW")) null else (it[3] as Number?)?.toLong())
                }
            }
            mysqlFamily(type) -> {
                val kinds = if (includeViews) "'BASE TABLE', 'VIEW'" else "'BASE TABLE'"
                rows(c, """
                    SELECT TABLE_NAME, TABLE_TYPE, TABLE_COMMENT, TABLE_ROWS
                      FROM information_schema.TABLES
                     WHERE TABLE_SCHEMA = ? AND TABLE_TYPE IN ($kinds)
                """, schema).forEach {
                    val view = it[1] == "VIEW"
                    // 뷰의 코멘트는 늘 "VIEW"라서 뺀다
                    add(it[0] as String?, if (view) "VIEW" else "TABLE", if (view) null else it[2] as String?,
                        if (view) null else (it[3] as Number?)?.toLong())
                }
            }
            else -> {
                val types = if (includeViews) arrayOf("TABLE", "VIEW") else arrayOf("TABLE")
                c.metaData.getTables(null, schema, "%", types).use { rs ->
                    while (rs.next()) add(rs.getString("TABLE_NAME"), rs.getString("TABLE_TYPE") ?: "TABLE", rs.getString("REMARKS"), null)
                }
            }
        }
        return result
    }

    private fun columns(type: String, c: Connection, schema: String, tables: Map<String, Table>) {
        if (tables.isEmpty()) return
        val meta = c.metaData
        val catalog = if (mysqlFamily(type)) schema else null
        val schemaPattern = if (mysqlFamily(type)) null else pattern(meta, schema)
        val positions = HashMap<String, MutableList<Pair<Int, Map<String, Any?>>>>()
        meta.getColumns(catalog, schemaPattern, "%", "%").use { rs ->
            while (rs.next()) {
                val table = rs.getString("TABLE_NAME") ?: continue
                if (table !in tables) continue
                if (!mysqlFamily(type) && rs.getString("TABLE_SCHEM") != schema) continue // 패턴에 걸린 다른 스키마
                val column = mapOf(
                    "name" to rs.getString("COLUMN_NAME"),
                    "type" to TableInfo.columnType(rs),
                    "nullable" to (rs.getInt("NULLABLE") != DatabaseMetaData.columnNoNulls),
                    "default" to rs.getString("COLUMN_DEF")?.trim()?.takeIf { it.isNotEmpty() },
                    "remarks" to rs.getString("REMARKS")?.takeIf { it.isNotBlank() },
                )
                positions.getOrPut(table) { ArrayList() }.add(rs.getInt("ORDINAL_POSITION") to column)
            }
        }
        for ((name, list) in positions) tables[name]?.columns?.addAll(list.sortedBy { it.first }.map { it.second })
    }

    /** 기본 키·고유 키·외래 키. 한 줄이 제약 조건의 컬럼 하나다. */
    private fun keys(type: String, c: Connection, schema: String, tables: Map<String, Table>) {
        if (tables.isEmpty()) return
        val sql = when {
            oracleFamily(type) -> """
                SELECT c.table_name, c.constraint_name, c.constraint_type, cc.column_name,
                       r.owner, r.table_name, rc.column_name
                  FROM all_constraints c
                  JOIN all_cons_columns cc ON cc.owner = c.owner AND cc.constraint_name = c.constraint_name AND cc.table_name = c.table_name
                  LEFT JOIN all_constraints r ON r.owner = c.r_owner AND r.constraint_name = c.r_constraint_name
                  LEFT JOIN all_cons_columns rc ON rc.owner = r.owner AND rc.constraint_name = r.constraint_name AND rc.position = cc.position
                 WHERE c.owner = ? AND c.constraint_type IN ('P', 'U', 'R')
                 ORDER BY c.table_name, c.constraint_name, cc.position
            """
            type == "postgres" -> """
                SELECT cl.relname, con.conname, con.contype::text, a.attname, rn.nspname, rcl.relname, ra.attname
                  FROM pg_constraint con
                  JOIN pg_class cl ON cl.oid = con.conrelid
                  JOIN pg_namespace n ON n.oid = cl.relnamespace
                  CROSS JOIN LATERAL unnest(con.conkey) WITH ORDINALITY AS k(attnum, ord)
                  JOIN pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = k.attnum
                  LEFT JOIN pg_class rcl ON rcl.oid = con.confrelid
                  LEFT JOIN pg_namespace rn ON rn.oid = rcl.relnamespace
                  LEFT JOIN pg_attribute ra ON ra.attrelid = con.confrelid AND ra.attnum = con.confkey[k.ord]
                 WHERE n.nspname = ? AND con.contype IN ('p', 'u', 'f')
                 ORDER BY cl.relname, con.conname, k.ord
            """
            mysqlFamily(type) -> """
                SELECT k.TABLE_NAME, k.CONSTRAINT_NAME, t.CONSTRAINT_TYPE, k.COLUMN_NAME,
                       k.REFERENCED_TABLE_SCHEMA, k.REFERENCED_TABLE_NAME, k.REFERENCED_COLUMN_NAME
                  FROM information_schema.KEY_COLUMN_USAGE k
                  JOIN information_schema.TABLE_CONSTRAINTS t
                    ON t.CONSTRAINT_SCHEMA = k.CONSTRAINT_SCHEMA AND t.TABLE_NAME = k.TABLE_NAME AND t.CONSTRAINT_NAME = k.CONSTRAINT_NAME
                 WHERE k.TABLE_SCHEMA = ? AND t.CONSTRAINT_TYPE IN ('PRIMARY KEY', 'UNIQUE', 'FOREIGN KEY')
                 ORDER BY k.TABLE_NAME, k.CONSTRAINT_NAME, k.ORDINAL_POSITION
            """
            else -> return genericKeys(c, schema, tables)
        }
        for (row in rows(c, sql, schema)) {
            val table = tables[row[0] as String? ?: continue] ?: continue
            val name = row[1] as String? ?: continue
            val column = row[3] as String? ?: continue
            when (row[2]?.toString()) {
                "P", "p", "PRIMARY KEY" -> (table.primaryKey ?: Key(name).also { table.primaryKey = it }).columns.add(column)
                "U", "u", "UNIQUE" -> table.uniqueKeys.getOrPut(name) { Key(name) }.columns.add(column)
                "R", "f", "FOREIGN KEY" -> {
                    val fk = table.foreignKeys.getOrPut(name) { ForeignKey(name, row[4] as String?, row[5] as String?) }
                    fk.columns.add(column)
                    (row[6] as String?)?.let { fk.refColumns.add(it) }
                }
            }
        }
    }

    private fun genericKeys(c: Connection, schema: String, tables: Map<String, Table>) {
        val meta = c.metaData
        for (table in tables.values) {
            runCatching {
                meta.getPrimaryKeys(null, schema, table.name).use { rs ->
                    val columns = ArrayList<Pair<Int, String>>()
                    var name: String? = null
                    while (rs.next()) {
                        name = rs.getString("PK_NAME")
                        columns.add(rs.getInt("KEY_SEQ") to rs.getString("COLUMN_NAME"))
                    }
                    if (columns.isNotEmpty()) table.primaryKey = Key(name).also { key -> key.columns.addAll(columns.sortedBy { it.first }.map { it.second }) }
                }
            }
            runCatching {
                meta.getImportedKeys(null, schema, table.name).use { rs ->
                    while (rs.next()) {
                        val name = rs.getString("FK_NAME") ?: "fk_${table.name}"
                        val fk = table.foreignKeys.getOrPut(name) { ForeignKey(name, rs.getString("PKTABLE_SCHEM"), rs.getString("PKTABLE_NAME")) }
                        fk.columns.add(rs.getString("FKCOLUMN_NAME"))
                        fk.refColumns.add(rs.getString("PKCOLUMN_NAME"))
                    }
                }
            }
        }
    }

    private fun indexes(type: String, c: Connection, schema: String, tables: Map<String, Table>) {
        if (tables.isEmpty()) return
        val sql = when {
            oracleFamily(type) -> """
                SELECT i.table_name, i.index_name, i.uniqueness, ic.column_name
                  FROM all_indexes i
                  JOIN all_ind_columns ic ON ic.index_owner = i.owner AND ic.index_name = i.index_name
                 WHERE i.table_owner = ? AND i.index_type NOT IN ('LOB')
                 ORDER BY i.table_name, i.index_name, ic.column_position
            """
            type == "postgres" -> """
                SELECT t.relname, ic.relname, CASE WHEN i.indisunique THEN 'UNIQUE' ELSE 'NONUNIQUE' END,
                       COALESCE(a.attname, '(expression)')
                  FROM pg_index i
                  JOIN pg_class ic ON ic.oid = i.indexrelid
                  JOIN pg_class t ON t.oid = i.indrelid
                  JOIN pg_namespace n ON n.oid = t.relnamespace
                  CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, ord)
                  LEFT JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = k.attnum AND k.attnum > 0
                 WHERE n.nspname = ? AND k.ord <= i.indnkeyatts
                 ORDER BY t.relname, ic.relname, k.ord
            """
            mysqlFamily(type) -> """
                SELECT TABLE_NAME, INDEX_NAME, CASE WHEN NON_UNIQUE = 0 THEN 'UNIQUE' ELSE 'NONUNIQUE' END,
                       COALESCE(COLUMN_NAME, '(expression)')
                  FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA = ? AND INDEX_NAME <> 'PRIMARY'
                 ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX
            """
            else -> return
        }
        val result = runCatching { rows(c, sql, schema) }.getOrElse { return }
        for (row in result) {
            val table = tables[row[0] as String? ?: continue] ?: continue
            val name = row[1] as String? ?: continue
            table.indexes.getOrPut(name) { Index(name, row[2] == "UNIQUE") }.columns.add(row[3] as String? ?: "?")
        }
    }

    // ---------------------------------------------------------------------
    // 스키마 DDL

    /**
     * 스키마 전체 DDL. [only]가 있으면 그 테이블·뷰만. includeRoutines면 시퀀스·함수·프로시저(Oracle 패키지 포함)도.
     * 테이블은 외래 키가 가리키는 테이블을 먼저 둔다 (서로 가리키는 테이블은 이름 순으로 끝에 두고 알린다).
     * 객체 사이에서 [cancelled]가 true면 멈춘다.
     */
    fun schemaDdl(
        type: String, c: Connection, schema: String, only: Set<String>?, includeViews: Boolean, includeRoutines: Boolean,
        cancelled: () -> Boolean,
    ): Map<String, Any?> {
        val all = tables(type, c, schema, includeViews).filterKeys { only == null || it in only }
        keys(type, c, schema, all)
        val (ordered, cycle) = dependencyOrder(all.values.filter { !it.type.endsWith("VIEW") }, schema)
        val views = all.values.filter { it.type.endsWith("VIEW") }.sortedBy { it.name }

        val sequences = if (includeRoutines) sequences(type, c, schema) else emptyList()
        val routines = if (includeRoutines) routines(type, c, schema) else emptyList()

        val out = StringBuilder()
        val stamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm"))
        out.append("-- Schema ").append(schema).append(" DDL, generated by DBaro at ").append(stamp).append('\n')
        if (cycle.isNotEmpty()) {
            out.append("-- These tables refer to each other; create their foreign keys after the tables if needed: ")
                .append(cycle.joinToString(", ") { it.name }).append('\n')
        }
        if (mysqlFamily(type)) out.append("\nSET FOREIGN_KEY_CHECKS = 0;\n")

        val failed = ArrayList<Map<String, Any?>>()
        var written = 0
        var stopped = false
        fun section(title: String, names: List<String>) {
            if (names.isEmpty() || stopped) return
            out.append("\n-- ").append("=".repeat(70)).append("\n-- ").append(title).append(" (").append(names.size).append(")\n")
            for (name in names) {
                if (cancelled()) {
                    stopped = true
                    return
                }
                try {
                    val result = Ddl.script(type, c, Ddl.Request(schema, name, schemaQuoted = true, nameQuoted = true))
                    out.append('\n').append(result.script.trimEnd()).append('\n')
                    written++
                } catch (e: Exception) {
                    failed.add(mapOf("name" to name, "message" to (e.message ?: e.toString())))
                    out.append("\n-- Could not generate the DDL of ").append(name).append(": ")
                        .append((e.message ?: e.toString()).lineSequence().first()).append('\n')
                }
            }
        }
        section("Sequences", sequences)
        section("Tables", (ordered + cycle).map { it.name })
        section("Views", views.map { it.name })
        section("Routines", routines)
        if (mysqlFamily(type)) out.append("\nSET FOREIGN_KEY_CHECKS = 1;\n")
        if (stopped) out.append("\n-- Stopped before all objects were written.\n")

        return mapOf(
            "schema" to schema,
            "script" to out.toString(),
            "objects" to written,
            "failed" to failed,
            "cycle" to cycle.map { it.name },
            "cancelled" to stopped,
        )
    }

    /** 외래 키가 가리키는 테이블이 먼저 오게 (같은 스키마 안에서). 순서를 정할 수 없는 테이블(순환)은 따로 돌려준다. */
    private fun dependencyOrder(tables: List<Table>, schema: String): Pair<List<Table>, List<Table>> {
        val byName = tables.associateBy { it.name }
        val remaining = tables.associate { table ->
            table.name to table.foreignKeys.values
                .filter { (it.refSchema == null || it.refSchema == schema) && it.refTable != table.name && it.refTable in byName }
                .mapNotNull { it.refTable }.toMutableSet()
        }.toMutableMap()
        val ordered = ArrayList<Table>()
        while (remaining.isNotEmpty()) {
            val ready = remaining.filterValues { it.isEmpty() }.keys.sorted()
            if (ready.isEmpty()) break
            for (name in ready) {
                remaining.remove(name)
                ordered.add(byName.getValue(name))
                for (deps in remaining.values) deps.remove(name)
            }
        }
        return ordered to remaining.keys.sorted().map { byName.getValue(it) }
    }

    private fun sequences(type: String, c: Connection, schema: String): List<String> = runCatching {
        when {
            oracleFamily(type) -> rows(c, "SELECT sequence_name FROM all_sequences WHERE sequence_owner = ? ORDER BY 1", schema)
            // 컬럼에 딸린 시퀀스(serial·identity)는 테이블 DDL에 들어 있다
            type == "postgres" -> rows(c, """
                SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
                 WHERE n.nspname = ? AND c.relkind = 'S'
                   AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.objid = c.oid AND d.deptype IN ('a', 'i'))
                 ORDER BY 1
            """, schema)
            else -> emptyList()
        }.mapNotNull { it[0] as String? }
    }.getOrDefault(emptyList())

    private fun routines(type: String, c: Connection, schema: String): List<String> = runCatching {
        when {
            oracleFamily(type) -> rows(c, """
                SELECT object_name FROM all_objects
                 WHERE owner = ? AND object_type IN ('FUNCTION', 'PROCEDURE', 'PACKAGE', 'TYPE')
                 ORDER BY DECODE(object_type, 'TYPE', 0, 'FUNCTION', 1, 'PROCEDURE', 2, 3), object_name
            """, schema)
            type == "postgres" -> rows(c, """
                SELECT DISTINCT p.proname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
                 WHERE n.nspname = ? AND p.prokind IN ('f', 'p')
                   AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.objid = p.oid AND d.deptype = 'e')
                 ORDER BY 1
            """, schema)
            mysqlFamily(type) -> rows(c, """
                SELECT ROUTINE_NAME FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = ? ORDER BY ROUTINE_TYPE, ROUTINE_NAME
            """, schema)
            else -> emptyList()
        }.mapNotNull { it[0] as String? }
    }.getOrDefault(emptyList())

    // ---------------------------------------------------------------------

    private fun rows(c: Connection, sql: String, vararg params: Any?): List<List<Any?>> =
        c.prepareStatement(sql.trimIndent()).use { statement ->
            params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
            statement.executeQuery().use { rs ->
                val count = rs.metaData.columnCount
                val list = ArrayList<List<Any?>>()
                while (rs.next()) {
                    list.add((1..count).map { column ->
                        when (val value = rs.getObject(column)) {
                            is java.math.BigDecimal -> value.toLong()
                            is java.sql.Clob -> value.getSubString(1, minOf(value.length(), 4000L).toInt())
                            else -> value
                        }
                    })
                }
                list
            }
        }

    private fun pattern(meta: DatabaseMetaData, name: String): String {
        val escape = runCatching { meta.searchStringEscape }.getOrNull()
        if (escape.isNullOrEmpty()) return name
        return name.replace(escape, escape + escape).replace("_", escape + "_").replace("%", escape + "%")
    }
}
