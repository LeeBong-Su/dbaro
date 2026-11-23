package dbaro.engine.exec

import java.sql.Connection
import java.sql.DatabaseMetaData
import java.sql.ResultSet
import java.sql.SQLException
import java.sql.Types

/**
 * 테이블(또는 뷰) 하나의 구조: 컬럼, 기본 키, 인덱스, 외래 키, 그 테이블을 가리키는 외래 키.
 * 결과 편집(기본 키로 행을 찾는다)과 테이블 상세 탭, 데이터 불러오기(컬럼 타입)에 쓴다.
 *
 * 이름은 DB 규칙대로 찾는다: 따옴표가 없으면 Oracle·Tibero는 대문자, PostgreSQL은 소문자,
 * 그 밖은 그대로·대문자·소문자 순. 스키마가 없으면 현재 스키마(PostgreSQL은 search_path)이고,
 * Oracle·Tibero는 시노님(자기 스키마 → PUBLIC)도 따라간다.
 */
object TableInfo {
    private const val MAX_SYNONYM_HOPS = 10

    private class Location(val catalog: String?, val schema: String?, val name: String, val type: String, val remarks: String?)

    fun describe(dbType: String, c: Connection, request: Ddl.Request): Map<String, Any?> {
        val location = locate(dbType, c, request)
            ?: throw SQLException("Table ${listOfNotNull(request.schema, request.name).joinToString(".")} does not exist.")
        val meta = c.metaData

        val primaryKey = ArrayList<Pair<Int, String>>()
        runCatching {
            meta.getPrimaryKeys(location.catalog, location.schema, location.name).use { rs ->
                while (rs.next()) primaryKey.add(rs.getInt("KEY_SEQ") to rs.getString("COLUMN_NAME"))
            }
        }
        val pkColumns = primaryKey.sortedBy { it.first }.map { it.second }

        val columns = ArrayList<Map<String, Any?>>()
        meta.getColumns(location.catalog, pattern(meta, location.schema), pattern(meta, location.name), "%").use { rs ->
            while (rs.next()) {
                val name = rs.getString("COLUMN_NAME")
                val dataType = rs.getInt("DATA_TYPE")
                columns.add(
                    mapOf(
                        "name" to name,
                        "type" to columnType(rs),
                        "typeName" to (rs.getString("TYPE_NAME") ?: ""),
                        "numeric" to Values.isNumeric(dataType),
                        "temporal" to (dataType == Types.DATE || dataType == Types.TIMESTAMP || dataType == Types.TIMESTAMP_WITH_TIMEZONE),
                        "nullable" to (rs.getInt("NULLABLE") != DatabaseMetaData.columnNoNulls),
                        "primaryKey" to (name in pkColumns),
                        "default" to rs.getString("COLUMN_DEF")?.trim(),
                        "remarks" to rs.getString("REMARKS"),
                        "position" to rs.getInt("ORDINAL_POSITION"),
                    ),
                )
            }
        }
        columns.sortBy { it["position"] as Int }

        return mapOf(
            "schema" to (location.schema ?: location.catalog),
            "name" to location.name,
            "type" to location.type,
            "remarks" to location.remarks,
            "columns" to columns,
            "primaryKey" to pkColumns,
            "indexes" to runCatching { indexes(meta, location) }.getOrDefault(emptyList()),
            "foreignKeys" to runCatching { foreignKeys(meta, location) }.getOrDefault(emptyList()),
            "referencedBy" to runCatching { referencedBy(meta, location) }.getOrDefault(emptyList()),
        )
    }

    // ---------------------------------------------------------------------
    // 이름 찾기

    private fun locate(dbType: String, c: Connection, request: Ddl.Request): Location? {
        val meta = c.metaData
        return when (dbType) {
            "oracle", "tibero" -> {
                var owner = request.schema?.let { if (request.schemaQuoted) it else it.uppercase() }
                    ?: request.currentSchema ?: currentOracleSchema(c)
                var name = if (request.nameQuoted) request.name else request.name.uppercase()
                var found = find(meta, null, owner, name)
                var hops = 0
                while (found == null && hops++ < MAX_SYNONYM_HOPS) {
                    val synonym = synonym(c, owner, name, publicToo = request.schema == null || hops > 1) ?: break
                    owner = synonym.first
                    name = synonym.second
                    found = find(meta, null, owner, name)
                }
                found
            }
            "postgres" -> {
                val name = if (request.nameQuoted) request.name else request.name.lowercase()
                val schema = request.schema?.let { if (request.schemaQuoted) it else it.lowercase() }
                    ?: request.currentSchema?.takeIf { find(meta, null, it, name) != null }
                    ?: visiblePostgresSchema(c, name)
                    ?: return null
                find(meta, null, schema, name)
            }
            "mysql", "mariadb" -> {
                val database = request.schema ?: request.currentSchema ?: c.catalog
                candidates(request.name, request.nameQuoted).firstNotNullOfOrNull { find(meta, database, null, it) }
            }
            else -> {
                val schemas = if (request.schema != null) candidates(request.schema, request.schemaQuoted)
                else listOfNotNull(request.currentSchema, runCatching { c.schema }.getOrNull()).distinct()
                schemas.firstNotNullOfOrNull { schema ->
                    candidates(request.name, request.nameQuoted).firstNotNullOfOrNull { find(meta, null, schema, it) }
                }
            }
        }
    }

    private fun candidates(name: String, quoted: Boolean) =
        if (quoted) listOf(name) else listOf(name, name.uppercase(), name.lowercase()).distinct()

    private fun find(meta: DatabaseMetaData, catalog: String?, schema: String?, name: String): Location? =
        meta.getTables(catalog, pattern(meta, schema), pattern(meta, name), null).use { rs ->
            while (rs.next()) {
                // 패턴이라 _ 가 다른 글자와도 맞을 수 있다: 이름이 정확히 같은 것만
                if (rs.getString("TABLE_NAME") != name) continue
                return Location(
                    rs.getString("TABLE_CAT") ?: catalog,
                    rs.getString("TABLE_SCHEM") ?: schema,
                    name,
                    rs.getString("TABLE_TYPE") ?: "TABLE",
                    rs.getString("REMARKS"),
                )
            }
            null
        }

    private fun currentOracleSchema(c: Connection): String =
        runCatching {
            c.createStatement().use { s ->
                s.executeQuery("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM DUAL").use { rs -> if (rs.next()) rs.getString(1) else null }
            }
        }.getOrNull() ?: runCatching { c.schema }.getOrNull() ?: throw SQLException("Could not determine the current schema.")

    private fun synonym(c: Connection, owner: String, name: String, publicToo: Boolean): Pair<String, String>? = runCatching {
        val sql = "SELECT TABLE_OWNER, TABLE_NAME FROM ALL_SYNONYMS WHERE SYNONYM_NAME = ? AND DB_LINK IS NULL AND " +
            if (publicToo) "OWNER IN (?, 'PUBLIC') ORDER BY CASE WHEN OWNER = 'PUBLIC' THEN 1 ELSE 0 END" else "OWNER = ?"
        c.prepareStatement(sql).use { s ->
            s.setString(1, name)
            s.setString(2, owner)
            s.executeQuery().use { rs -> if (rs.next()) rs.getString(1) to rs.getString(2) else null }
        }
    }.getOrNull()

    private fun visiblePostgresSchema(c: Connection, name: String): String? = runCatching {
        c.prepareStatement(
            "SELECT n.nspname FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace " +
                "WHERE c.relname = ? AND pg_catalog.pg_table_is_visible(c.oid)",
        ).use { s ->
            s.setString(1, name)
            s.executeQuery().use { rs -> if (rs.next()) rs.getString(1) else null }
        }
    }.getOrNull()

    // ---------------------------------------------------------------------
    // 인덱스와 키

    private fun indexes(meta: DatabaseMetaData, t: Location): List<Map<String, Any?>> {
        class Index(val unique: Boolean) {
            val columns = sortedMapOf<Int, String>()
        }
        val indexes = linkedMapOf<String, Index>()
        // approximate=true: Oracle이 통계를 새로 모으지 않게 한다
        meta.getIndexInfo(t.catalog, t.schema, t.name, false, true).use { rs ->
            while (rs.next()) {
                if (rs.getShort("TYPE") == DatabaseMetaData.tableIndexStatistic) continue
                val name = rs.getString("INDEX_NAME") ?: continue
                val column = rs.getString("COLUMN_NAME") ?: continue
                val descending = rs.getString("ASC_OR_DESC") == "D"
                val index = indexes.getOrPut(name) { Index(!rs.getBoolean("NON_UNIQUE")) }
                index.columns[rs.getInt("ORDINAL_POSITION")] = if (descending) "$column DESC" else column
            }
        }
        return indexes.map { (name, index) -> mapOf("name" to name, "unique" to index.unique, "columns" to index.columns.values.toList()) }
    }

    private class Key(val name: String, val schema: String?, val table: String) {
        val columns = sortedMapOf<Int, Pair<String, String>>() // KEY_SEQ → (이 테이블 컬럼, 상대 테이블 컬럼)
    }

    private fun keys(rs: ResultSet, own: (ResultSet) -> String, other: (ResultSet) -> String, otherSchema: String, otherTable: String): List<Key> {
        val keys = linkedMapOf<String, Key>()
        while (rs.next()) {
            // MySQL·MariaDB는 데이터베이스가 카탈로그 자리에 온다
            val schema = rs.getString(otherSchema) ?: rs.getString(otherSchema.replace("SCHEM", "CAT"))
            val table = rs.getString(otherTable)
            val name = rs.getString("FK_NAME") ?: "$table.${own(rs)}"
            val key = keys.getOrPut("$name|$schema|$table") { Key(name, schema, table) }
            key.columns[rs.getInt("KEY_SEQ")] = own(rs) to other(rs)
        }
        return keys.values.toList()
    }

    private fun foreignKeys(meta: DatabaseMetaData, t: Location): List<Map<String, Any?>> =
        meta.getImportedKeys(t.catalog, t.schema, t.name).use { rs ->
            keys(rs, { it.getString("FKCOLUMN_NAME") }, { it.getString("PKCOLUMN_NAME") }, "PKTABLE_SCHEM", "PKTABLE_NAME")
        }.map { key ->
            mapOf(
                "name" to key.name,
                "columns" to key.columns.values.map { it.first },
                "refSchema" to key.schema,
                "refTable" to key.table,
                "refColumns" to key.columns.values.map { it.second },
            )
        }

    private fun referencedBy(meta: DatabaseMetaData, t: Location): List<Map<String, Any?>> =
        meta.getExportedKeys(t.catalog, t.schema, t.name).use { rs ->
            keys(rs, { it.getString("PKCOLUMN_NAME") }, { it.getString("FKCOLUMN_NAME") }, "FKTABLE_SCHEM", "FKTABLE_NAME")
        }.map { key ->
            mapOf(
                "name" to key.name,
                "schema" to key.schema,
                "table" to key.table,
                "columns" to key.columns.values.map { it.second },
                "refColumns" to key.columns.values.map { it.first },
            )
        }

    // ---------------------------------------------------------------------

    private fun pattern(meta: DatabaseMetaData, name: String?): String? {
        if (name == null) return null
        val escape = runCatching { meta.searchStringEscape }.getOrNull()
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
}
