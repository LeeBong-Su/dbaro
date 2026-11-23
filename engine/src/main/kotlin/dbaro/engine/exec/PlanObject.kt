package dbaro.engine.exec

import java.sql.Connection
import java.sql.DatabaseMetaData
import java.sql.ResultSet
import java.sql.SQLException

/**
 * 실행 계획에 나온 객체(테이블·인덱스)의 정보: 테이블 구조(컬럼, 인덱스, 키)와 옵티마이저 통계.
 * 인덱스면 그 인덱스가 걸린 테이블을 찾아 테이블 정보를 주고, 쓰인 인덱스 이름(`index`)을 함께 준다.
 *
 * 결과: `table`(TableInfo 결과), `index`(계획이 쓴 인덱스, 없으면 null),
 * `tableStats: [[이름, 값]...]`, `indexStats: {인덱스 이름: [[이름, 값]...]}`.
 * 통계는 DB마다 있는 것만 (Oracle·Tibero 딕셔너리, PostgreSQL 카탈로그·통계 뷰, MySQL information_schema).
 */
object PlanObject {
    /** 계획에서 온 이름은 정확한 이름(따옴표 없이)이다. [currentSchema]는 스키마가 없을 때 찾을 스키마 (탭에서 바꾼 것). */
    class Request(
        val schema: String?,
        val name: String,
        val type: String?,
        val index: String?,
        val currentSchema: String?,
    )

    private const val MAX_TABLES_TO_SCAN = 500

    fun describe(dbType: String, c: Connection, request: Request): Map<String, Any?> {
        var schema = request.schema
        var table = request.name
        var index = request.index
        if (request.type?.uppercase()?.startsWith("INDEX") == true) {
            val found = indexTable(dbType, c, request.schema ?: request.currentSchema, request.name)
                ?: throw SQLException("Index ${listOfNotNull(request.schema, request.name).joinToString(".")} was not found.")
            schema = found.first
            table = found.second
            index = request.name
        }
        val info = TableInfo.describe(dbType, c, Ddl.Request(schema, table, schema != null, true, request.currentSchema))
        val tableSchema = info["schema"]?.toString()
        val tableName = info["name"].toString()
        return mapOf(
            "objectType" to request.type,
            "index" to index,
            "table" to info,
            "tableStats" to runCatching { tableStats(dbType, c, tableSchema, tableName) }.getOrElse { emptyList() },
            "indexStats" to runCatching { indexStats(dbType, c, tableSchema, tableName) }.getOrElse { emptyMap() },
        )
    }

    // ---------------------------------------------------------------------
    // 인덱스 → 테이블

    private fun indexTable(dbType: String, c: Connection, schema: String?, index: String): Pair<String?, String>? =
        when (dbType) {
            "oracle", "tibero" -> {
                val owner = schema ?: c.firstValue("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM DUAL")
                c.rowsOf("SELECT TABLE_OWNER, TABLE_NAME FROM ALL_INDEXES WHERE OWNER = ? AND INDEX_NAME = ?", owner, index)
                    .firstOrNull()?.let { it["TABLE_OWNER"]?.toString() to it["TABLE_NAME"].toString() }
            }
            "postgres" -> {
                val base = "SELECT tn.nspname AS table_schema, t.relname AS table_name FROM pg_index x " +
                    "JOIN pg_class i ON i.oid = x.indexrelid JOIN pg_namespace n ON n.oid = i.relnamespace " +
                    "JOIN pg_class t ON t.oid = x.indrelid JOIN pg_namespace tn ON tn.oid = t.relnamespace WHERE i.relname = ?"
                val rows = if (schema != null) c.rowsOf("$base AND n.nspname = ?", index, schema)
                else c.rowsOf("$base AND pg_catalog.pg_table_is_visible(i.oid)", index)
                rows.firstOrNull()?.let { it["TABLE_SCHEMA"]?.toString() to it["TABLE_NAME"].toString() }
            }
            "mysql", "mariadb" ->
                c.rowsOf(
                    "SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.STATISTICS " +
                        "WHERE INDEX_NAME = ? AND TABLE_SCHEMA = COALESCE(?, DATABASE()) LIMIT 1",
                    index, schema,
                ).firstOrNull()?.let { it["TABLE_SCHEMA"]?.toString() to it["TABLE_NAME"].toString() }
            else -> genericIndexTable(c.metaData, schema ?: runCatching { c.schema }.getOrNull(), index)
        }

    /** 표준 방법이 없어서 스키마의 테이블을 차례로 본다 (H2 같은 작은 DB용) */
    private fun genericIndexTable(meta: DatabaseMetaData, schema: String?, index: String): Pair<String?, String>? {
        val tables = ArrayList<String>()
        meta.getTables(null, schema, "%", arrayOf("TABLE")).use { rs ->
            while (rs.next() && tables.size < MAX_TABLES_TO_SCAN) tables.add(rs.getString("TABLE_NAME"))
        }
        for (table in tables) {
            val found = runCatching {
                meta.getIndexInfo(null, schema, table, false, true).use { rs ->
                    var hit = false
                    while (rs.next()) if (rs.getString("INDEX_NAME").equals(index, ignoreCase = true)) hit = true
                    hit
                }
            }.getOrDefault(false)
            if (found) return schema to table
        }
        return null
    }

    // ---------------------------------------------------------------------
    // 통계

    private fun tableStats(dbType: String, c: Connection, schema: String?, table: String): List<List<String>> =
        when (dbType) {
            "oracle", "tibero" -> pick(
                c.rowsOf("SELECT * FROM ALL_TABLES WHERE OWNER = ? AND TABLE_NAME = ?", schema, table).firstOrNull(),
                "NUM_ROWS" to "Rows (statistics)", "BLOCKS" to "Blocks", "EMPTY_BLOCKS" to "Empty blocks",
                "AVG_ROW_LEN" to "Average row length", "SAMPLE_SIZE" to "Sample size", "LAST_ANALYZED" to "Last analyzed",
                "PARTITIONED" to "Partitioned", "TEMPORARY" to "Temporary", "IOT_TYPE" to "IOT type",
                "TABLESPACE_NAME" to "Tablespace", "DEGREE" to "Degree", "COMPRESSION" to "Compression",
                "STATUS" to "Status",
            )
            "postgres" -> pick(
                c.rowsOf(
                    "SELECT c.reltuples::bigint AS reltuples, c.relpages, " +
                        "pg_size_pretty(pg_total_relation_size(c.oid)) AS total_size, " +
                        "pg_size_pretty(pg_relation_size(c.oid)) AS table_size, s.n_live_tup, s.n_dead_tup, " +
                        "s.seq_scan, s.idx_scan, s.last_analyze, s.last_autoanalyze, s.last_vacuum, s.last_autovacuum " +
                        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace " +
                        "LEFT JOIN pg_stat_all_tables s ON s.relid = c.oid WHERE n.nspname = ? AND c.relname = ?",
                    schema, table,
                ).firstOrNull(),
                "RELTUPLES" to "Rows (statistics)", "RELPAGES" to "Pages", "TOTAL_SIZE" to "Total size (with indexes)",
                "TABLE_SIZE" to "Table size", "N_LIVE_TUP" to "Live rows", "N_DEAD_TUP" to "Dead rows",
                "SEQ_SCAN" to "Sequential scans", "IDX_SCAN" to "Index scans", "LAST_ANALYZE" to "Last analyzed",
                "LAST_AUTOANALYZE" to "Last auto-analyzed", "LAST_VACUUM" to "Last vacuumed",
                "LAST_AUTOVACUUM" to "Last auto-vacuumed",
            )
            "mysql", "mariadb" -> pick(
                c.rowsOf("SELECT * FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ?", schema, table)
                    .firstOrNull(),
                "ENGINE" to "Engine", "TABLE_ROWS" to "Rows (statistics)", "AVG_ROW_LENGTH" to "Average row length",
                "DATA_LENGTH" to "Data size (bytes)", "INDEX_LENGTH" to "Index size (bytes)",
                "AUTO_INCREMENT" to "Next AUTO_INCREMENT", "CREATE_TIME" to "Created", "UPDATE_TIME" to "Updated",
                "TABLE_COLLATION" to "Collation",
            )
            else -> genericTableStats(c.metaData, schema, table)
        }

    private fun genericTableStats(meta: DatabaseMetaData, schema: String?, table: String): List<List<String>> {
        meta.getIndexInfo(null, schema, table, false, true).use { rs ->
            while (rs.next()) {
                if (rs.getShort("TYPE") == DatabaseMetaData.tableIndexStatistic) {
                    return listOfNotNull(
                        rs.getString("CARDINALITY")?.let { listOf("Rows (statistics)", it) },
                        rs.getString("PAGES")?.let { listOf("Pages", it) },
                    )
                }
            }
        }
        return emptyList()
    }

    private fun indexStats(dbType: String, c: Connection, schema: String?, table: String): Map<String, List<List<String>>> =
        when (dbType) {
            "oracle", "tibero" ->
                c.rowsOf("SELECT * FROM ALL_INDEXES WHERE TABLE_OWNER = ? AND TABLE_NAME = ?", schema, table)
                    .associate { row ->
                        row["INDEX_NAME"].toString() to pick(
                            row,
                            "INDEX_TYPE" to "Type", "UNIQUENESS" to "Uniqueness", "STATUS" to "Status",
                            "VISIBILITY" to "Visibility", "BLEVEL" to "B-tree level", "LEAF_BLOCKS" to "Leaf blocks",
                            "DISTINCT_KEYS" to "Distinct keys", "CLUSTERING_FACTOR" to "Clustering factor",
                            "NUM_ROWS" to "Rows (statistics)", "LAST_ANALYZED" to "Last analyzed",
                            "PARTITIONED" to "Partitioned", "TABLESPACE_NAME" to "Tablespace",
                        )
                    }
            "postgres" ->
                c.rowsOf(
                    "SELECT i.relname AS index_name, am.amname, x.indisunique, x.indisprimary, x.indisvalid, " +
                        "pg_size_pretty(pg_relation_size(i.oid)) AS index_size, i.reltuples::bigint AS reltuples, " +
                        "s.idx_scan, s.idx_tup_read, s.idx_tup_fetch, pg_get_indexdef(i.oid) AS definition " +
                        "FROM pg_index x JOIN pg_class i ON i.oid = x.indexrelid JOIN pg_am am ON am.oid = i.relam " +
                        "JOIN pg_class t ON t.oid = x.indrelid JOIN pg_namespace n ON n.oid = t.relnamespace " +
                        "LEFT JOIN pg_stat_all_indexes s ON s.indexrelid = i.oid WHERE n.nspname = ? AND t.relname = ?",
                    schema, table,
                ).associate { row ->
                    row["INDEX_NAME"].toString() to pick(
                        row,
                        "AMNAME" to "Method", "INDISUNIQUE" to "Unique", "INDISPRIMARY" to "Primary key",
                        "INDISVALID" to "Valid", "INDEX_SIZE" to "Size", "RELTUPLES" to "Rows (statistics)",
                        "IDX_SCAN" to "Index scans", "IDX_TUP_READ" to "Entries read", "IDX_TUP_FETCH" to "Rows fetched",
                        "DEFINITION" to "Definition",
                    )
                }
            "mysql", "mariadb" ->
                c.rowsOf(
                    "SELECT INDEX_NAME, INDEX_TYPE, MAX(NON_UNIQUE) AS NON_UNIQUE, MAX(CARDINALITY) AS CARDINALITY, " +
                        "COUNT(*) AS COLUMN_COUNT FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? " +
                        "GROUP BY INDEX_NAME, INDEX_TYPE",
                    schema, table,
                ).associate { row ->
                    row["INDEX_NAME"].toString() to pick(
                        row,
                        "INDEX_TYPE" to "Type", "NON_UNIQUE" to "Non-unique", "CARDINALITY" to "Cardinality",
                        "COLUMN_COUNT" to "Columns",
                    )
                }
            else -> emptyMap()
        }

    // ---------------------------------------------------------------------
    // 도우미

    /** 행에서 있는 컬럼만 골라 [이름, 값] 목록으로 (값이 없으면 건너뛴다) */
    private fun pick(row: Map<String, Any?>?, vararg columns: Pair<String, String>): List<List<String>> {
        if (row == null) return emptyList()
        return columns.mapNotNull { (column, label) -> row[column]?.let { listOf(label, text(it)) } }
    }

    private fun text(value: Any): String = when (value) {
        is java.sql.Timestamp -> value.toString().removeSuffix(".0")
        is Boolean -> if (value) "yes" else "no"
        else -> value.toString().trim()
    }

    /** 컬럼 이름(대문자) → 값 */
    private fun Connection.rowsOf(sql: String, vararg params: Any?): List<Map<String, Any?>> =
        prepareStatement(sql).use { statement ->
            params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
            statement.executeQuery().use { rs -> readRows(rs) }
        }

    private fun Connection.firstValue(sql: String): String? =
        createStatement().use { st -> st.executeQuery(sql).use { rs -> if (rs.next()) rs.getString(1) else null } }

    private fun readRows(rs: ResultSet): List<Map<String, Any?>> {
        val meta = rs.metaData
        val labels = (1..meta.columnCount).map { meta.getColumnLabel(it).uppercase() }
        val rows = ArrayList<Map<String, Any?>>()
        while (rs.next()) rows.add(labels.withIndex().associate { (i, label) -> label to rs.getObject(i + 1) })
        return rows
    }
}
