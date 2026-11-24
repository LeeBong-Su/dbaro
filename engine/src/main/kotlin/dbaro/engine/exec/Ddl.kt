package dbaro.engine.exec

import java.sql.Connection
import java.sql.DatabaseMetaData
import java.sql.SQLException

/**
 * 객체 하나의 DDL 스크립트. 객체 본문에 더해 코멘트, 인덱스, 트리거, 그 객체를 가리키는 시노님, 권한 부여를 함께 만든다.
 * - Oracle·Tibero: DBMS_METADATA (권한이 없거나 지원하지 않으면 딕셔너리 뷰로 직접 만든다)
 * - MySQL·MariaDB: SHOW CREATE … (코멘트는 본문에 들어 있다), 트리거, information_schema 권한
 * - PostgreSQL: 시스템 카탈로그로 직접 만든다 (컬럼, 제약 조건, 인덱스, 소유자, 코멘트, 권한, 트리거)
 * - 그 밖(H2 등): JDBC 메타데이터로 만든다
 *
 * 이름은 따옴표로 감싸지 않았으면 DB 규칙에 맞춘다 (Oracle 대문자, PostgreSQL 소문자). 스키마가 없으면 현재 스키마.
 */
object Ddl {
    /**
     * 찾을 객체. [currentSchema]는 스키마 없이 쓴 이름을 찾을 스키마 (쿼리 탭에서 바꾼 스키마).
     * 메타데이터 세션은 탭과 다른 세션이라 탭의 ALTER SESSION·search_path·USE가 보이지 않아서 따로 받는다.
     */
    class Request(
        val schema: String?,
        val name: String,
        val schemaQuoted: Boolean,
        val nameQuoted: Boolean,
        val currentSchema: String? = null,
    )

    class Result(val schema: String, val name: String, val type: String, val script: String) {
        fun toMap(): Map<String, Any?> = mapOf("schema" to schema, "name" to name, "type" to type, "script" to script)
    }

    fun script(dbType: String, connection: Connection, request: Request): Result = when (dbType) {
        "oracle", "tibero" -> Oracle(connection, dbType == "tibero").script(request)
        "mysql", "mariadb" -> MySql(connection).script(request)
        "postgres" -> Postgres(connection).script(request)
        else -> Generic(connection).script(request)
    }

    // ---------------------------------------------------------------------
    // 공통

    /** 제목 주석이 붙은 구역으로 스크립트를 모은다. 빈 구역은 넣지 않는다. */
    internal class Builder(title: String) {
        private val out = StringBuilder("-- $title\n")

        fun main(ddl: String) {
            out.append('\n').append(ddl.trim()).append('\n')
        }

        fun section(title: String, statements: List<String>) {
            val body = statements.map { it.trim() }.filter { it.isNotEmpty() }
            if (body.isEmpty()) return
            out.append("\n-- ").append(title).append('\n')
            body.forEach { out.append(it).append('\n') }
        }

        fun note(text: String) {
            out.append("-- ").append(text).append('\n')
        }

        override fun toString() = out.toString().trimEnd() + "\n"
    }

    internal fun sqlString(text: String) = "'" + text.replace("'", "''") + "'"

    internal fun doubleQuoted(name: String) = "\"" + name.replace("\"", "\"\"") + "\""

    /** 같은 받는 사람·GRANT OPTION끼리 권한을 묶는다: GRANT SELECT, UPDATE ON obj TO grantee; */
    internal fun grantStatements(objectName: String, grants: List<Triple<String, String, Boolean>>, quoteGrantee: (String) -> String): List<String> =
        grants.groupBy { it.first to it.third }.map { (key, rows) ->
            val privileges = rows.map { it.second }.distinct().joinToString(", ")
            "GRANT $privileges ON $objectName TO ${quoteGrantee(key.first)}" + (if (key.second) " WITH GRANT OPTION" else "") + ";"
        }

    private fun Connection.rows(sql: String, vararg params: Any?): List<List<Any?>> =
        prepareStatement(sql).use { statement ->
            params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
            statement.executeQuery().use { rs ->
                val count = rs.metaData.columnCount
                buildList { while (rs.next()) add((1..count).map { rs.getObject(it) }) }
            }
        }

    private fun Connection.strings(sql: String, vararg params: Any?): List<String?> =
        prepareStatement(sql).use { statement ->
            params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
            statement.executeQuery().use { rs -> buildList { while (rs.next()) add(rs.getString(1)) } }
        }

    private fun Connection.string(sql: String, vararg params: Any?): String? = strings(sql, *params).firstOrNull()

    private fun notFound(schema: String, name: String): Nothing =
        throw SQLException("Object not found: $schema.$name")

    // ---------------------------------------------------------------------
    // Oracle·Tibero

    private class Oracle(val c: Connection, val tibero: Boolean) {
        private val typeOrder = listOf(
            "TABLE", "MATERIALIZED VIEW", "VIEW", "SEQUENCE", "PACKAGE", "TYPE", "PROCEDURE", "FUNCTION",
            "TRIGGER", "SYNONYM", "INDEX",
        )

        fun script(request: Request): Result {
            var owner = request.schema?.let { if (request.schemaQuoted) it else it.uppercase() }
                ?: request.currentSchema
                ?: c.string("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM DUAL")
                ?: throw SQLException("Could not determine the current schema.")
            var name = if (request.nameQuoted) request.name else request.name.uppercase()
            var type = objectType(owner, name)
            var viaSynonym: String? = null

            if (type == null && request.schema == null) {
                // 시노님 이름이면 가리키는 객체를 보여 준다 (자기 스키마 시노님이 PUBLIC보다 먼저).
                val target = c.rows(
                    "SELECT TABLE_OWNER, TABLE_NAME, OWNER FROM ALL_SYNONYMS WHERE SYNONYM_NAME = ? AND OWNER IN (?, 'PUBLIC') " +
                        "AND DB_LINK IS NULL ORDER BY CASE WHEN OWNER = 'PUBLIC' THEN 1 ELSE 0 END",
                    name, owner,
                ).firstOrNull()
                if (target != null) {
                    viaSynonym = "${target[2]}.$name"
                    owner = target[0].toString()
                    name = target[1].toString()
                    type = objectType(owner, name)
                }
            }
            if (type == null) notFound(owner, name)

            setTransforms()
            val qualified = "${doubleQuoted(owner)}.${doubleQuoted(name)}"
            val builder = Builder("$owner.$name ($type)")
            viaSynonym?.let { builder.note("Resolved from synonym $it") }
            builder.main(mainDdl(type, owner, name))

            val relation = type == "TABLE" || type == "VIEW" || type == "MATERIALIZED VIEW"
            if (relation)
                builder.section("Comments", dependent("COMMENT", owner, name) ?: commentFallback(owner, name, qualified))
            if (type == "TABLE" || type == "MATERIALIZED VIEW")
                builder.section("Indexes", indexes(owner, name))
            if (type == "TABLE" || type == "VIEW")
                builder.section("Triggers", dependent("TRIGGER", owner, name).orEmpty())
            if (type != "INDEX" && type != "TRIGGER" && type != "SYNONYM")
                builder.section("Synonyms", synonyms(owner, name, qualified))
            if (type != "INDEX" && type != "TRIGGER")
                builder.section("Grants", dependent("OBJECT_GRANT", owner, name) ?: grantFallback(owner, name, qualified))
            return Result(owner, name, type, builder.toString())
        }

        private fun objectType(owner: String, name: String): String? {
            val types = c.strings(
                "SELECT OBJECT_TYPE FROM ALL_OBJECTS WHERE OWNER = ? AND OBJECT_NAME = ?",
                owner, name,
            ).filterNotNull().toSet()
            return typeOrder.firstOrNull { it in types }
        }

        private fun setTransforms() {
            runCatching {
                c.prepareCall(
                    "BEGIN " +
                        "DBMS_METADATA.SET_TRANSFORM_PARAM(DBMS_METADATA.SESSION_TRANSFORM, 'SQLTERMINATOR', TRUE); " +
                        "DBMS_METADATA.SET_TRANSFORM_PARAM(DBMS_METADATA.SESSION_TRANSFORM, 'PRETTY', TRUE); " +
                        "END;",
                ).use { it.execute() }
            }
        }

        private fun metadataName(type: String) = type.replace(' ', '_')

        private fun mainDdl(type: String, owner: String, name: String): String {
            val viaMetadata = runCatching {
                c.string("SELECT DBMS_METADATA.GET_DDL(?, ?, ?) FROM DUAL", metadataName(type), name, owner)
            }
            viaMetadata.getOrNull()?.takeIf { it.isNotBlank() }?.let { return it }

            // DBMS_METADATA를 쓸 수 없으면 (권한, Tibero 등) 딕셔너리로 직접 만든다.
            val fallback = runCatching {
                when (type) {
                    "TABLE" -> Generic(c).tableDdl(null, owner, name, "${doubleQuoted(owner)}.${doubleQuoted(name)}", ::doubleQuoted)
                    "VIEW" -> c.string("SELECT TEXT FROM ALL_VIEWS WHERE OWNER = ? AND VIEW_NAME = ?", owner, name)
                        ?.let { "CREATE OR REPLACE VIEW ${doubleQuoted(owner)}.${doubleQuoted(name)} AS\n${it.trim()};" }
                    "PACKAGE", "TYPE", "PROCEDURE", "FUNCTION", "TRIGGER" -> sourceDdl(type, owner, name)
                    else -> null
                }
            }.getOrNull()
            if (!fallback.isNullOrBlank()) return fallback
            throw viaMetadata.exceptionOrNull() ?: SQLException("Could not generate the DDL of $owner.$name.")
        }

        private fun sourceDdl(type: String, owner: String, name: String): String? {
            val types = if (type == "PACKAGE" || type == "TYPE") listOf(type, "$type BODY") else listOf(type)
            val parts = types.mapNotNull { sourceType ->
                val lines = c.strings(
                    "SELECT TEXT FROM ALL_SOURCE WHERE OWNER = ? AND NAME = ? AND TYPE = ? ORDER BY LINE",
                    owner, name, sourceType,
                )
                if (lines.isEmpty()) null else "CREATE OR REPLACE " + lines.joinToString("") { it.orEmpty() }.trimEnd() + "\n/"
            }
            return parts.joinToString("\n\n").ifBlank { null }
        }

        /**
         * DBMS_METADATA 종속 객체 DDL. 해당 종속 객체가 없으면(ORA-31608) 빈 목록, 쓸 수 없으면 null.
         * 여러 문장이 한 글로 오면서 두 번째 문장부터 앞에 공백이 붙는다 → 문장 첫 줄의 공백을 뗀다 (트리거 본문은 그대로).
         */
        private fun dependent(kind: String, owner: String, name: String): List<String>? = try {
            listOfNotNull(c.string("SELECT DBMS_METADATA.GET_DEPENDENT_DDL(?, ?, ?) FROM DUAL", kind, name, owner))
                .map { if (kind == "TRIGGER") it else it.replace(statementIndent, "\n") }
        } catch (e: SQLException) {
            if (e.errorCode == 31608) emptyList() else null
        }

        private val statementIndent = Regex("\n[ \t]+(?=(CREATE|COMMENT|ALTER|GRANT) )")

        /**
         * 테이블의 인덱스. 기본 키·고유 키 제약이 쓰는 인덱스는 테이블 DDL의 USING INDEX로 함께 만들어지므로 뺀다
         * (GET_DEPENDENT_DDL('INDEX')는 모두 주어서 스크립트를 실행하면 ORA-00955가 난다).
         */
        private fun indexes(owner: String, name: String): List<String> {
            val names = runCatching {
                c.rows(
                    "SELECT i.OWNER, i.INDEX_NAME FROM ALL_INDEXES i WHERE i.TABLE_OWNER = ? AND i.TABLE_NAME = ? AND i.INDEX_TYPE <> 'LOB' " +
                        "AND NOT EXISTS (SELECT 1 FROM ALL_CONSTRAINTS k WHERE k.OWNER = i.TABLE_OWNER AND k.TABLE_NAME = i.TABLE_NAME " +
                        "AND k.INDEX_OWNER = i.OWNER AND k.INDEX_NAME = i.INDEX_NAME) ORDER BY i.INDEX_NAME",
                    owner, name,
                )
            }.getOrNull() ?: return dependent("INDEX", owner, name).orEmpty()
            return names.map { row ->
                runCatching { c.string("SELECT DBMS_METADATA.GET_DDL('INDEX', ?, ?) FROM DUAL", row[1], row[0]) }
                    .getOrElse { return dependent("INDEX", owner, name).orEmpty() }
                    .orEmpty()
            }
        }

        private fun commentFallback(owner: String, name: String, qualified: String): List<String> = runCatching {
            val statements = ArrayList<String>()
            c.string("SELECT COMMENTS FROM ALL_TAB_COMMENTS WHERE OWNER = ? AND TABLE_NAME = ?", owner, name)
                ?.let { statements.add("COMMENT ON TABLE $qualified IS ${sqlString(it)};") }
            for (row in c.rows(
                "SELECT COLUMN_NAME, COMMENTS FROM ALL_COL_COMMENTS WHERE OWNER = ? AND TABLE_NAME = ? AND COMMENTS IS NOT NULL",
                owner, name,
            )) {
                statements.add("COMMENT ON COLUMN $qualified.${doubleQuoted(row[0].toString())} IS ${sqlString(row[1].toString())};")
            }
            statements
        }.getOrDefault(emptyList())

        private fun grantFallback(owner: String, name: String, qualified: String): List<String> = runCatching {
            val grants = c.rows(
                "SELECT GRANTEE, PRIVILEGE, GRANTABLE FROM ALL_TAB_PRIVS WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? " +
                    "ORDER BY GRANTEE, PRIVILEGE",
                owner, name,
            ).map { Triple(it[0].toString(), it[1].toString(), it[2]?.toString() == "YES") }
            grantStatements(qualified, grants, ::doubleQuoted)
        }.getOrDefault(emptyList())

        private fun synonyms(owner: String, name: String, qualified: String): List<String> = runCatching {
            c.rows(
                "SELECT OWNER, SYNONYM_NAME FROM ALL_SYNONYMS WHERE TABLE_OWNER = ? AND TABLE_NAME = ? AND DB_LINK IS NULL " +
                    "ORDER BY CASE WHEN OWNER = 'PUBLIC' THEN 0 ELSE 1 END, OWNER, SYNONYM_NAME",
                owner, name,
            ).map { oracleSynonym(it[0].toString(), it[1].toString(), qualified) }
        }.getOrDefault(emptyList())
    }

    internal fun oracleSynonym(synonymOwner: String, synonymName: String, target: String): String =
        if (synonymOwner == "PUBLIC") "CREATE OR REPLACE PUBLIC SYNONYM ${doubleQuoted(synonymName)} FOR $target;"
        else "CREATE OR REPLACE SYNONYM ${doubleQuoted(synonymOwner)}.${doubleQuoted(synonymName)} FOR $target;"

    // ---------------------------------------------------------------------
    // MySQL·MariaDB

    private class MySql(val c: Connection) {
        fun quote(name: String) = "`" + name.replace("`", "``") + "`"

        fun script(request: Request): Result {
            val schemaGuess = request.schema ?: request.currentSchema ?: c.string("SELECT DATABASE()")
                ?: throw SQLException("No database selected. Write it as database.name or run USE first.")

            // 대소문자 구분은 서버 설정(lower_case_table_names)에 따라 다르므로 대소문자를 무시하고 찾는다.
            val table = c.rows(
                "SELECT TABLE_SCHEMA, TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES " +
                    "WHERE LOWER(TABLE_SCHEMA) = LOWER(?) AND LOWER(TABLE_NAME) = LOWER(?)",
                schemaGuess, request.name,
            ).firstOrNull()
            if (table != null) {
                val schema = table[0].toString()
                val name = table[1].toString()
                val isView = table[2].toString().contains("VIEW")
                val qualified = "${quote(schema)}.${quote(name)}"
                val builder = Builder("$schema.$name (${if (isView) "VIEW" else "TABLE"})")
                val create = c.prepareStatement("SHOW CREATE ${if (isView) "VIEW" else "TABLE"} $qualified").use { s ->
                    s.executeQuery().use { rs -> if (rs.next()) rs.getString(2) else null }
                } ?: notFound(schema, name)
                builder.main("$create;")
                if (!isView) {
                    val triggers = c.strings(
                        "SELECT TRIGGER_NAME FROM information_schema.TRIGGERS WHERE EVENT_OBJECT_SCHEMA = ? AND EVENT_OBJECT_TABLE = ? ORDER BY ACTION_ORDER",
                        schema, name,
                    ).filterNotNull()
                    builder.section("Triggers", triggers.mapNotNull { showCreate("TRIGGER", schema, it, 3)?.let(::delimited) })
                }
                builder.section("Grants", grants(schema, name, qualified))
                return Result(schema, name, if (isView) "VIEW" else "TABLE", builder.toString())
            }

            val routine = c.rows(
                "SELECT ROUTINE_SCHEMA, ROUTINE_NAME, ROUTINE_TYPE FROM information_schema.ROUTINES " +
                    "WHERE LOWER(ROUTINE_SCHEMA) = LOWER(?) AND LOWER(ROUTINE_NAME) = LOWER(?)",
                schemaGuess, request.name,
            ).firstOrNull()
            if (routine != null) {
                val schema = routine[0].toString()
                val name = routine[1].toString()
                val type = routine[2].toString()
                val builder = Builder("$schema.$name ($type)")
                builder.main(
                    showCreate(type, schema, name, 3)?.let(::delimited)
                        ?: throw SQLException("No privilege to read the source of $schema.$name."),
                )
                return Result(schema, name, type, builder.toString())
            }

            val trigger = c.rows(
                "SELECT TRIGGER_SCHEMA, TRIGGER_NAME FROM information_schema.TRIGGERS " +
                    "WHERE LOWER(TRIGGER_SCHEMA) = LOWER(?) AND LOWER(TRIGGER_NAME) = LOWER(?)",
                schemaGuess, request.name,
            ).firstOrNull() ?: notFound(schemaGuess, request.name)
            val schema = trigger[0].toString()
            val name = trigger[1].toString()
            val builder = Builder("$schema.$name (TRIGGER)")
            builder.main(showCreate("TRIGGER", schema, name, 3)?.let(::delimited) ?: notFound(schema, name))
            return Result(schema, name, "TRIGGER", builder.toString())
        }

        private fun showCreate(kind: String, schema: String, name: String, column: Int): String? =
            c.prepareStatement("SHOW CREATE $kind ${quote(schema)}.${quote(name)}").use { s ->
                s.executeQuery().use { rs -> if (rs.next()) rs.getString(column) else null }
            }

        /** 본문에 ;가 들어가는 루틴·트리거는 DELIMITER로 감싼다 (DBaro 에디터도 DELIMITER를 안다). */
        private fun delimited(body: String) = "DELIMITER $$\n${body.trim()}$$\nDELIMITER ;"

        private fun grants(schema: String, name: String, qualified: String): List<String> = runCatching {
            val rows = c.rows(
                "SELECT GRANTEE, PRIVILEGE_TYPE, IS_GRANTABLE FROM information_schema.TABLE_PRIVILEGES " +
                    "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? ORDER BY GRANTEE, PRIVILEGE_TYPE",
                schema, name,
            ).map { Triple(it[0].toString(), it[1].toString(), it[2]?.toString() == "YES") }
            grantStatements(qualified, rows) { it } // GRANTEE는 이미 'user'@'host' 형식
        }.getOrDefault(emptyList())
    }

    // ---------------------------------------------------------------------
    // PostgreSQL

    private class Postgres(val c: Connection) {
        fun script(request: Request): Result {
            val name = if (request.nameQuoted) request.name else request.name.lowercase()
            // 탭의 search_path 첫 스키마에 없으면 이 세션의 current_schema()에서 찾는다 (public 등)
            val schema = request.schema?.let { if (request.schemaQuoted) it else it.lowercase() }
                ?: request.currentSchema?.takeIf { hint ->
                    c.string(
                        "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = ? AND c.relname = ?",
                        hint, name,
                    ) != null
                }
                ?: c.string("SELECT current_schema()")
                ?: throw SQLException("Could not determine the current schema.")

            val relation = c.rows(
                "SELECT c.oid, c.relkind, quote_ident(n.nspname) || '.' || quote_ident(c.relname), pg_get_userbyid(c.relowner) " +
                    "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = ? AND c.relname = ?",
                schema, name,
            ).firstOrNull()
            if (relation == null) return function(schema, name)

            val oid = (relation[0] as Number).toLong()
            val kind = relation[1].toString()
            val qualified = relation[2].toString()
            val owner = relation[3]?.toString()
            val type = when (kind) {
                "r", "p" -> "TABLE"
                "v" -> "VIEW"
                "m" -> "MATERIALIZED VIEW"
                "S" -> "SEQUENCE"
                "i" -> "INDEX"
                "f" -> "FOREIGN TABLE"
                else -> "RELATION"
            }
            val builder = Builder("$schema.$name ($type)")
            when (kind) {
                "r", "p", "f" -> builder.main(tableDdl(oid, qualified, kind == "p"))
                "v" -> builder.main("CREATE OR REPLACE VIEW $qualified AS\n" + c.string("SELECT pg_get_viewdef(?::oid, true)", oid).orEmpty().trim())
                "m" -> builder.main("CREATE MATERIALIZED VIEW $qualified AS\n" + c.string("SELECT pg_get_viewdef(?::oid, true)", oid).orEmpty().trim().removeSuffix(";") + "\nWITH DATA;")
                "S" -> builder.main(sequenceDdl(schema, name, qualified))
                "i" -> builder.main(c.string("SELECT pg_get_indexdef(?::oid)", oid).orEmpty() + ";")
                else -> notFound(schema, name)
            }
            val keyword = if (kind == "S") "SEQUENCE" else if (kind == "v") "VIEW" else if (kind == "m") "MATERIALIZED VIEW" else "TABLE"
            if (owner != null && kind != "i")
                builder.section("Owner", listOf("ALTER $keyword $qualified OWNER TO ${c.string("SELECT quote_ident(?)", owner)};"))
            if (kind in listOf("r", "p", "f", "v", "m"))
                builder.section("Comments", comments(oid, qualified, keyword))
            if (kind == "p")
                builder.section("Partitions", c.strings(
                    "SELECT 'CREATE TABLE ' || quote_ident(n.nspname) || '.' || quote_ident(c.relname) || ' PARTITION OF ' || ? || ' ' || " +
                        "pg_get_expr(c.relpartbound, c.oid) || ';' FROM pg_inherits h JOIN pg_class c ON c.oid = h.inhrelid " +
                        "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE h.inhparent = ?::oid ORDER BY c.relname",
                    qualified, oid,
                ).filterNotNull())
            if (kind == "r" || kind == "p" || kind == "m")
                builder.section("Indexes", c.strings(
                    "SELECT pg_get_indexdef(i.indexrelid) FROM pg_index i WHERE i.indrelid = ?::oid " +
                        "AND NOT EXISTS (SELECT 1 FROM pg_constraint k WHERE k.conindid = i.indexrelid) " +
                        // 파티션이 부모 인덱스에서 물려받은 인덱스는 부모 인덱스가 만든다
                        "AND NOT EXISTS (SELECT 1 FROM pg_inherits ih WHERE ih.inhrelid = i.indexrelid) ORDER BY 1",
                    oid,
                    // 분할 테이블의 인덱스는 ON ONLY(부모에만)로 나온다 → 다시 만들 때 파티션에도 만들어지게 뺀다
                ).filterNotNull().map { it.replace(" ON ONLY ", " ON ") + ";" })
            if (kind == "r" || kind == "p" || kind == "v")
                builder.section("Triggers", c.strings(
                    "SELECT pg_get_triggerdef(oid, true) FROM pg_trigger WHERE tgrelid = ?::oid AND NOT tgisinternal ORDER BY tgname",
                    oid,
                ).filterNotNull().map { "$it;" })
            if (kind != "i")
                builder.section("Grants", grants(schema, name, qualified, owner, kind == "S"))
            return Result(schema, name, type, builder.toString())
        }

        private fun tableDdl(oid: Long, qualified: String, partitioned: Boolean): String {
            // 파티션: 컬럼은 부모에서 오므로 PARTITION OF 로 (부모에서 물려받은 제약은 뺀다)
            c.rows(
                "SELECT quote_ident(pn.nspname) || '.' || quote_ident(p.relname), pg_get_expr(c.relpartbound, c.oid) " +
                    "FROM pg_class c JOIN pg_inherits h ON h.inhrelid = c.oid JOIN pg_class p ON p.oid = h.inhparent " +
                    "JOIN pg_namespace pn ON pn.oid = p.relnamespace WHERE c.oid = ?::oid AND c.relispartition",
                oid,
            ).firstOrNull()?.let { parent ->
                val own = c.rows(
                    "SELECT quote_ident(conname), pg_get_constraintdef(oid, true) FROM pg_constraint " +
                        "WHERE conrelid = ?::oid AND conparentid = 0 AND coninhcount = 0 ORDER BY conname",
                    oid,
                ).map { "    CONSTRAINT ${it[0]} ${it[1]}" }
                return "CREATE TABLE $qualified PARTITION OF ${parent[0]}" +
                    (if (own.isEmpty()) "" else " (\n" + own.joinToString(",\n") + "\n)") + "\n${parent[1]};"
            }
            val lines = ArrayList<String>()
            for (row in c.rows(
                "SELECT quote_ident(a.attname), format_type(a.atttypid, a.atttypmod), pg_get_expr(d.adbin, d.adrelid), " +
                    "a.attnotnull, a.attidentity FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum " +
                    "WHERE a.attrelid = ?::oid AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum",
                oid,
            )) {
                val identity = row[4]?.toString().orEmpty()
                val line = StringBuilder("    ${row[0]} ${row[1]}")
                when (identity) {
                    "a" -> line.append(" GENERATED ALWAYS AS IDENTITY")
                    "d" -> line.append(" GENERATED BY DEFAULT AS IDENTITY")
                    else -> row[2]?.let { line.append(" DEFAULT ").append(it) }
                }
                if (row[3] == true) line.append(" NOT NULL")
                lines.add(line.toString())
            }
            for (row in c.rows(
                "SELECT quote_ident(conname), pg_get_constraintdef(oid, true) FROM pg_constraint WHERE conrelid = ?::oid " +
                    "ORDER BY CASE contype WHEN 'p' THEN 0 WHEN 'u' THEN 1 WHEN 'c' THEN 2 WHEN 'f' THEN 3 ELSE 4 END, conname",
                oid,
            )) {
                lines.add("    CONSTRAINT ${row[0]} ${row[1]}")
            }
            val partitionKey = if (partitioned) c.string("SELECT pg_get_partkeydef(?::oid)", oid)?.let { "\nPARTITION BY $it" }.orEmpty() else ""
            return "CREATE TABLE $qualified (\n" + lines.joinToString(",\n") + "\n)$partitionKey;"
        }

        private fun sequenceDdl(schema: String, name: String, qualified: String): String {
            val row = c.rows(
                "SELECT start_value, increment_by, min_value, max_value, cache_size, cycle FROM pg_sequences " +
                    "WHERE schemaname = ? AND sequencename = ?",
                schema, name,
            ).firstOrNull() ?: return "CREATE SEQUENCE $qualified;"
            return "CREATE SEQUENCE $qualified\n    START WITH ${row[0]}\n    INCREMENT BY ${row[1]}\n    MINVALUE ${row[2]}\n" +
                "    MAXVALUE ${row[3]}\n    CACHE ${row[4]}" + (if (row[5] == true) "\n    CYCLE" else "") + ";"
        }

        private fun comments(oid: Long, qualified: String, keyword: String): List<String> {
            val statements = ArrayList<String>()
            c.string("SELECT obj_description(?::oid, 'pg_class')", oid)
                ?.let { statements.add("COMMENT ON $keyword $qualified IS ${sqlString(it)};") }
            for (row in c.rows(
                "SELECT quote_ident(a.attname), col_description(a.attrelid, a.attnum) FROM pg_attribute a " +
                    "WHERE a.attrelid = ?::oid AND a.attnum > 0 AND NOT a.attisdropped AND col_description(a.attrelid, a.attnum) IS NOT NULL " +
                    "ORDER BY a.attnum",
                oid,
            )) {
                statements.add("COMMENT ON COLUMN $qualified.${row[0]} IS ${sqlString(row[1].toString())};")
            }
            return statements
        }

        private fun grants(schema: String, name: String, qualified: String, owner: String?, sequence: Boolean): List<String> = runCatching {
            val rows = if (sequence) {
                c.rows(
                    "SELECT grantee, privilege_type, is_grantable FROM information_schema.usage_privileges " +
                        "WHERE object_schema = ? AND object_name = ? AND object_type = 'SEQUENCE' ORDER BY grantee, privilege_type",
                    schema, name,
                )
            } else {
                c.rows(
                    "SELECT grantee, privilege_type, is_grantable FROM information_schema.role_table_grants " +
                        "WHERE table_schema = ? AND table_name = ? ORDER BY grantee, privilege_type",
                    schema, name,
                )
            }
            val grants = rows.filter { it[0]?.toString() != owner } // 소유자 자신의 권한은 빼고
                .map { Triple(it[0].toString(), it[1].toString(), it[2]?.toString() == "YES") }
            val objectName = if (sequence) "SEQUENCE $qualified" else qualified
            grantStatements(objectName, grants) { grantee ->
                if (grantee == "PUBLIC") grantee else c.string("SELECT quote_ident(?)", grantee) ?: grantee
            }
        }.getOrDefault(emptyList())

        private fun function(schema: String, name: String): Result {
            // pg_get_functiondef는 집계 함수(prokind 'a')를 다루지 못한다
            val routines = c.rows(
                "SELECT pg_get_functiondef(p.oid), p.prokind FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace " +
                    "WHERE n.nspname = ? AND p.proname = ? AND p.prokind <> 'a' ORDER BY p.oid",
                schema, name,
            ).filter { it[0] != null }
            if (routines.isEmpty()) notFound(schema, name)
            val type = if (routines.all { it[1]?.toString() == "p" }) "PROCEDURE" else "FUNCTION"
            val builder = Builder("$schema.$name ($type)")
            routines.forEach { builder.main(it[0].toString().trim().let { body -> if (body.endsWith(";")) body else "$body;" }) }
            return Result(schema, name, type, builder.toString())
        }
    }

    // ---------------------------------------------------------------------
    // 그 밖 (H2 등): JDBC 메타데이터

    private class Generic(val c: Connection) {
        private val meta: DatabaseMetaData = c.metaData

        fun quote(name: String): String {
            val q = meta.identifierQuoteString?.trim().orEmpty()
            return if (q.isEmpty()) name else q + name.replace(q, q + q) + q
        }

        fun script(request: Request): Result {
            val schemaGuess = request.schema ?: request.currentSchema ?: c.schema
            // 따옴표 없는 이름은 그대로, 대문자, 소문자 순서로 찾아본다.
            val candidates = if (request.nameQuoted) listOf(request.name) else listOf(request.name, request.name.uppercase(), request.name.lowercase()).distinct()
            val schemaCandidates = when {
                request.schema == null -> listOf(schemaGuess)
                request.schemaQuoted -> listOf(request.schema)
                else -> listOf(request.schema, request.schema.uppercase(), request.schema.lowercase()).distinct()
            }
            for (schema in schemaCandidates) {
                for (name in candidates) {
                    val type = meta.getTables(null, schema, name, null).use { rs -> if (rs.next()) rs.getString("TABLE_TYPE") else null }
                        ?: continue
                    return relation(schema, name, type.uppercase().let { if (it == "BASE TABLE") "TABLE" else it })
                }
            }
            notFound(schemaGuess.orEmpty(), request.name)
        }

        private fun relation(schema: String?, name: String, type: String): Result {
            val qualified = listOfNotNull(schema, name).joinToString(".") { quote(it) }
            val builder = Builder("${listOfNotNull(schema, name).joinToString(".")} ($type)")
            val isView = type.contains("VIEW")
            if (isView) {
                val definition = runCatching {
                    c.string("SELECT VIEW_DEFINITION FROM INFORMATION_SCHEMA.VIEWS WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ?", schema, name)
                }.getOrNull()
                builder.main(if (definition != null) "CREATE VIEW $qualified AS\n${definition.trim()};" else "-- The view definition is not available.")
            } else {
                builder.main(tableDdl(null, schema, name, qualified, ::quote))
            }
            builder.section("Comments", comments(schema, name, qualified))
            if (!isView) builder.section("Indexes", indexes(schema, name, qualified))
            builder.section("Synonyms", synonyms(schema, name, qualified))
            builder.section("Grants", grants(schema, name, qualified))
            return Result(schema.orEmpty(), name, type, builder.toString())
        }

        /** 컬럼·기본값·NOT NULL·기본 키로 CREATE TABLE을 만든다 (Oracle 대체 경로에서도 쓴다). */
        fun tableDdl(catalog: String?, schema: String?, name: String, qualified: String, quote: (String) -> String): String {
            val lines = ArrayList<String>()
            meta.getColumns(catalog, schema, name, "%").use { rs ->
                while (rs.next()) {
                    val type = rs.getString("TYPE_NAME").orEmpty()
                    val size = rs.getInt("COLUMN_SIZE")
                    val digits = rs.getInt("DECIMAL_DIGITS").let { if (rs.wasNull()) null else it }
                    val typeText = when {
                        type.contains('(') -> type
                        type.uppercase() in sizedTypes && digits != null && digits > 0 -> "$type($size,$digits)"
                        type.uppercase() in sizedTypes && size > 0 -> "$type($size)"
                        else -> type
                    }
                    val line = StringBuilder("    ${quote(rs.getString("COLUMN_NAME"))} $typeText")
                    rs.getString("COLUMN_DEF")?.trim()?.takeIf { it.isNotEmpty() }?.let { line.append(" DEFAULT ").append(it) }
                    if (rs.getInt("NULLABLE") == DatabaseMetaData.columnNoNulls) line.append(" NOT NULL")
                    lines.add(line.toString())
                }
            }
            val keys = meta.getPrimaryKeys(catalog, schema, name).use { rs ->
                buildList { while (rs.next()) add(rs.getShort("KEY_SEQ").toInt() to (rs.getString("COLUMN_NAME") to rs.getString("PK_NAME"))) }
            }.sortedBy { it.first }.map { it.second }
            if (keys.isNotEmpty()) {
                val keyName = keys.first().second
                lines.add("    " + (if (keyName != null) "CONSTRAINT ${quote(keyName)} " else "") + "PRIMARY KEY (" + keys.joinToString(", ") { quote(it.first) } + ")")
            }
            return "CREATE TABLE $qualified (\n" + lines.joinToString(",\n") + "\n);"
        }

        private fun comments(schema: String?, name: String, qualified: String): List<String> {
            val statements = ArrayList<String>()
            meta.getTables(null, schema, name, null).use { rs ->
                if (rs.next()) rs.getString("REMARKS")?.takeIf { it.isNotBlank() }
                    ?.let { statements.add("COMMENT ON TABLE $qualified IS ${sqlString(it)};") }
            }
            meta.getColumns(null, schema, name, "%").use { rs ->
                while (rs.next()) {
                    rs.getString("REMARKS")?.takeIf { it.isNotBlank() }?.let {
                        statements.add("COMMENT ON COLUMN $qualified.${quote(rs.getString("COLUMN_NAME"))} IS ${sqlString(it)};")
                    }
                }
            }
            return statements
        }

        private fun indexes(schema: String?, name: String, qualified: String): List<String> = runCatching {
            val primary = meta.getPrimaryKeys(null, schema, name).use { rs -> if (rs.next()) rs.getString("PK_NAME") else null }
            val columns = LinkedHashMap<String, MutableList<Pair<Int, String>>>()
            val unique = HashMap<String, Boolean>()
            meta.getIndexInfo(null, schema, name, false, false).use { rs ->
                while (rs.next()) {
                    val index = rs.getString("INDEX_NAME") ?: continue
                    val column = rs.getString("COLUMN_NAME") ?: continue
                    columns.getOrPut(index) { ArrayList() }.add(rs.getInt("ORDINAL_POSITION") to column)
                    unique[index] = !rs.getBoolean("NON_UNIQUE")
                }
            }
            columns.filterKeys { !it.equals(primary, ignoreCase = true) && !it.startsWith("PRIMARY_KEY") }.map { (index, cols) ->
                val prefix = if (unique[index] == true) "CREATE UNIQUE INDEX" else "CREATE INDEX"
                val indexName = listOfNotNull(schema, index).joinToString(".") { quote(it) }
                "$prefix $indexName ON $qualified (" + cols.sortedBy { it.first }.joinToString(", ") { quote(it.second) } + ");"
            }
        }.getOrDefault(emptyList())

        private fun synonyms(schema: String?, name: String, qualified: String): List<String> = runCatching {
            c.rows(
                "SELECT SYNONYM_SCHEMA, SYNONYM_NAME FROM INFORMATION_SCHEMA.SYNONYMS WHERE SYNONYM_FOR_SCHEMA = ? AND SYNONYM_FOR = ? " +
                    "ORDER BY SYNONYM_SCHEMA, SYNONYM_NAME",
                schema, name,
            ).map { "CREATE OR REPLACE SYNONYM ${quote(it[0].toString())}.${quote(it[1].toString())} FOR $qualified;" }
        }.getOrDefault(emptyList())

        private fun grants(schema: String?, name: String, qualified: String): List<String> = runCatching {
            val rows = meta.getTablePrivileges(null, schema, name).use { rs ->
                buildList {
                    while (rs.next()) {
                        val grantee = rs.getString("GRANTEE") ?: continue
                        add(Triple(grantee, rs.getString("PRIVILEGE"), rs.getString("IS_GRANTABLE") == "YES"))
                    }
                }
            }
            grantStatements(qualified, rows) { quote(it) }
        }.getOrDefault(emptyList())

        companion object {
            val sizedTypes = setOf(
                "CHAR", "VARCHAR", "VARCHAR2", "NCHAR", "NVARCHAR2", "CHARACTER", "CHARACTER VARYING", "NUMBER",
                "NUMERIC", "DECIMAL", "RAW", "VARBINARY", "BINARY", "BINARY VARYING",
            )
        }
    }
}
