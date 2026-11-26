package dbaro.engine.exec

import java.sql.Connection

/**
 * 이름 바꾸기 리팩토링: 테이블(또는 그 컬럼)의 이름을 바꿀 때 함께 고쳐야 할 DB 객체와 그 소스.
 * 소스 안에서 이름을 찾아 바꾸는 일은 앱이 한다 (파일·쿼리 탭과 같은 규칙으로).
 *
 * - Oracle·Tibero: ALL_DEPENDENCIES로 그 테이블에 기대는 뷰·프로시저·함수·패키지·트리거·타입 본문,
 *   테이블 이름이면 그 테이블을 가리키는 시노님도. 소스는 DBMS_METADATA (안 되면 ALL_VIEWS·ALL_SOURCE).
 *   인덱스·제약 조건·권한·테이블의 트리거는 이름을 바꿔도 따라간다. 기대던 객체는 다시 만들 때까지 INVALID다.
 * - PostgreSQL: 뷰·제약 조건·인덱스·트리거는 PostgreSQL이 스스로 고친다 (autoUpdated로 알려 준다).
 *   함수·프로시저 본문은 글이라 이름이 들어 있는 것을 찾는다 (pg_get_functiondef).
 * - MySQL·MariaDB: 뷰·루틴·트리거는 글로 저장된다. 이름이 들어 있는 것을 찾는다 (SHOW CREATE …).
 *   루틴·트리거는 지우고 다시 만든다 (before에 DROP).
 *
 * 결과 objects: [{schema, name, type, source, before: [문장], terminator(";" 또는 "delimiter" 또는 ""),
 * autoUpdated, note}], notes
 */
object Refactor {
    private const val MAX_OBJECTS = 500

    private class Dependent(
        val schema: String,
        val name: String,
        val type: String,
        var source: String? = null,
        val before: List<String> = emptyList(),
        val terminator: String = "",
        val autoUpdated: Boolean = false,
        var note: String? = null,
    ) {
        fun toMap() = mapOf(
            "schema" to schema, "name" to name, "type" to type, "source" to source, "before" to before,
            "terminator" to terminator, "autoUpdated" to autoUpdated, "note" to note,
        )
    }

    fun renameImpact(type: String, c: Connection, schema: String, table: String, column: String?): Map<String, Any?> {
        val notes = ArrayList<String>()
        val objects = when (type) {
            "oracle", "tibero" -> oracle(type, c, schema, table, column, notes)
            "postgres" -> postgres(c, schema, table, column, notes)
            "mysql", "mariadb" -> mysql(type, c, schema, table, column, notes)
            else -> emptyList<Dependent>().also { notes.add("Dependent objects are not searched for $type.") }
        }
        return mapOf("objects" to objects.take(MAX_OBJECTS).map { it.toMap() }, "notes" to notes)
    }

    // ---------------------------------------------------------------------

    private fun oracle(type: String, c: Connection, schema: String, table: String, column: String?, notes: MutableList<String>): List<Dependent> {
        notes.add("Oracle keeps the table's indexes, constraints, grants and triggers. Objects that use the table stay invalid until they are recreated.")
        val metadata = type == "oracle" && runCatching {
            c.prepareCall(
                "BEGIN DBMS_METADATA.SET_TRANSFORM_PARAM(DBMS_METADATA.SESSION_TRANSFORM, 'SQLTERMINATOR', TRUE); " +
                    "DBMS_METADATA.SET_TRANSFORM_PARAM(DBMS_METADATA.SESSION_TRANSFORM, 'PRETTY', TRUE); END;",
            ).use { it.execute() }
        }.isSuccess

        val result = ArrayList<Dependent>()
        val rows = rows(
            c,
            """
            SELECT DISTINCT owner, name, type FROM all_dependencies
             WHERE referenced_owner = ? AND referenced_name = ? AND referenced_type = 'TABLE'
               AND type IN ('VIEW', 'MATERIALIZED VIEW', 'PROCEDURE', 'FUNCTION', 'PACKAGE', 'PACKAGE BODY', 'TRIGGER', 'TYPE BODY')
             ORDER BY 3, 1, 2
            """,
            schema, table,
        )
        for (row in rows) {
            val owner = row[0] as String
            val name = row[1] as String
            val kind = row[2] as String
            if (kind == "MATERIALIZED VIEW") {
                result.add(Dependent(owner, name, kind, note = "A materialized view cannot be replaced: recreate it by hand."))
                continue
            }
            val source = (if (metadata) runCatching { metadataDdl(c, kind, owner, name) }.getOrNull() else null)
                ?: runCatching { dictionarySource(c, kind, owner, name) }.getOrNull()
            result.add(Dependent(owner, name, kind, source, note = if (source == null) "Could not read the source." else null))
        }
        if (column == null) {
            // 테이블을 가리키는 시노님은 새 이름으로 다시 만든다
            rows(
                c,
                "SELECT owner, synonym_name FROM all_synonyms WHERE table_owner = ? AND table_name = ? AND db_link IS NULL ORDER BY 1, 2",
                schema, table,
            ).forEach { row ->
                val target = "${Ddl.doubleQuoted(schema)}.${Ddl.doubleQuoted(table)}"
                result.add(Dependent(row[0] as String, row[1] as String, "SYNONYM", Ddl.oracleSynonym(row[0] as String, row[1] as String, target)))
            }
        }
        return result
    }

    private fun metadataDdl(c: Connection, kind: String, owner: String, name: String): String? {
        val metadataType = when (kind) {
            "PACKAGE" -> "PACKAGE_SPEC"
            "PACKAGE BODY" -> "PACKAGE_BODY"
            "TYPE BODY" -> "TYPE_BODY"
            else -> kind.replace(' ', '_')
        }
        return scalar(c, "SELECT DBMS_METADATA.GET_DDL(?, ?, ?) FROM DUAL", metadataType, name, owner)?.trim()
    }

    /** DBMS_METADATA를 쓸 수 없을 때 (Tibero, 권한): 뷰는 ALL_VIEWS, 코드는 ALL_SOURCE */
    private fun dictionarySource(c: Connection, kind: String, owner: String, name: String): String? {
        if (kind == "VIEW") {
            val text = scalar(c, "SELECT text FROM all_views WHERE owner = ? AND view_name = ?", owner, name) ?: return null
            return "CREATE OR REPLACE VIEW ${Ddl.doubleQuoted(owner)}.${Ddl.doubleQuoted(name)} AS\n${text.trim().trimEnd(';')};"
        }
        val lines = rows(c, "SELECT text FROM all_source WHERE owner = ? AND name = ? AND type = ? ORDER BY line", owner, name, kind)
        if (lines.isEmpty()) return null
        val body = lines.joinToString("") { it[0]?.toString() ?: "" }.trimEnd()
        return "CREATE OR REPLACE $body\n/"
    }

    private fun postgres(c: Connection, schema: String, table: String, column: String?, notes: MutableList<String>): List<Dependent> {
        notes.add("PostgreSQL updates views, constraints, indexes and triggers by itself. Function bodies are text, so they are listed here.")
        val result = ArrayList<Dependent>()
        val oid = scalar(
            c,
            "SELECT c.oid FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = ? AND c.relname = ?",
            schema, table,
        )?.toLongOrNull()
        if (oid != null) {
            val columnFilter = if (column != null) {
                "AND d.refobjsubid = (SELECT attnum FROM pg_attribute WHERE attrelid = $oid AND attname = ?)"
            } else ""
            val params = if (column != null) arrayOf<Any?>(column) else emptyArray()
            rows(
                c,
                """
                SELECT DISTINCT vn.nspname, v.relname, CASE v.relkind WHEN 'm' THEN 'MATERIALIZED VIEW' ELSE 'VIEW' END
                  FROM pg_depend d
                  JOIN pg_rewrite r ON r.oid = d.objid
                  JOIN pg_class v ON v.oid = r.ev_class
                  JOIN pg_namespace vn ON vn.oid = v.relnamespace
                 WHERE d.classid = 'pg_rewrite'::regclass AND d.refobjid = $oid AND v.oid <> $oid $columnFilter
                 ORDER BY 1, 2
                """,
                *params,
            ).forEach { row ->
                result.add(Dependent(row[0] as String, row[1] as String, row[2] as String, autoUpdated = true,
                    note = "PostgreSQL updates this by itself."))
            }
        }
        val word = column ?: table
        rows(
            c,
            """
            SELECT n.nspname, p.proname, CASE p.prokind WHEN 'p' THEN 'PROCEDURE' ELSE 'FUNCTION' END, pg_get_functiondef(p.oid)
              FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE p.prokind IN ('f', 'p') AND n.nspname NOT IN ('pg_catalog', 'information_schema')
               AND p.prosrc ~* ?
               AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.objid = p.oid AND d.deptype = 'e')
             ORDER BY 1, 2
            """,
            "\\m" + word.replace(Regex("""[.*+?^${'$'}()\[\]{}|\\]"""), """\\$0""") + "\\M",
        ).forEach { row ->
            result.add(Dependent(row[0] as String, row[1] as String, row[2] as String, row[3]?.toString()?.trim(), terminator = ";"))
        }
        return result
    }

    private fun mysql(type: String, c: Connection, schema: String, table: String, column: String?, notes: MutableList<String>): List<Dependent> {
        notes.add("MySQL keeps the table's indexes, constraints and triggers. Views, routines and trigger bodies are stored as text, so they are listed here.")
        val result = ArrayList<Dependent>()
        val word = column ?: table
        val like = "%$word%"
        // LIKE는 낱말 일부(temporary의 emp)에도 걸리므로 낱말로 다시 거르고, 시스템 스키마는 뺀다
        val wordPattern = Regex("(?<![\\w$#])" + Regex.escape(word) + "(?![\\w$#])", RegexOption.IGNORE_CASE)
        val system = "('mysql', 'sys', 'performance_schema', 'information_schema')"
        fun q(name: String) = "`" + name.replace("`", "``") + "`"
        fun uses(definition: Any?) = definition != null && wordPattern.containsMatchIn(definition.toString())

        rows(c, "SELECT TABLE_SCHEMA, TABLE_NAME, VIEW_DEFINITION FROM information_schema.VIEWS WHERE VIEW_DEFINITION LIKE ? AND TABLE_SCHEMA NOT IN $system ORDER BY 1, 2", like)
            .filter { uses(it[2]) }
            .forEach { row ->
                val s = row[0] as String
                val n = row[1] as String
                val source = runCatching { showCreate(c, "VIEW", "${q(s)}.${q(n)}", 2) }.getOrNull()
                    ?.replaceFirst(Regex("^CREATE\\s+", RegexOption.IGNORE_CASE), "CREATE OR REPLACE ")
                result.add(Dependent(s, n, "VIEW", source, terminator = ";", note = if (source == null) "Could not read the view (SHOW VIEW privilege?)." else null))
            }
        // MariaDB는 CREATE OR REPLACE로 한 번에 바꾼다. MySQL은 그런 문법이 없어 지우고 다시 만든다.
        val replace = type == "mariadb"
        fun recreate(source: String?) = if (replace) source?.replaceFirst(Regex("^CREATE\\s+", RegexOption.IGNORE_CASE), "CREATE OR REPLACE ") else source
        rows(c, "SELECT ROUTINE_SCHEMA, ROUTINE_NAME, ROUTINE_TYPE, ROUTINE_DEFINITION FROM information_schema.ROUTINES WHERE ROUTINE_DEFINITION LIKE ? AND ROUTINE_SCHEMA NOT IN $system ORDER BY 1, 2", like)
            .filter { uses(it[3]) }
            .forEach { row ->
                val s = row[0] as String
                val n = row[1] as String
                val kind = row[2] as String
                val source = runCatching { showCreate(c, kind, "${q(s)}.${q(n)}", 3) }.getOrNull()
                result.add(Dependent(s, n, kind, recreate(source),
                    before = if (replace) emptyList() else listOf("DROP $kind IF EXISTS ${q(s)}.${q(n)};"), terminator = "delimiter",
                    note = if (source == null) "Could not read the routine." else null))
            }
        rows(
            c,
            "SELECT TRIGGER_SCHEMA, TRIGGER_NAME, ACTION_STATEMENT FROM information_schema.TRIGGERS WHERE ACTION_STATEMENT LIKE ? AND TRIGGER_SCHEMA NOT IN $system ORDER BY 1, 2",
            like,
        ).filter { uses(it[2]) }.forEach { row ->
            val s = row[0] as String
            val n = row[1] as String
            val source = runCatching { showCreate(c, "TRIGGER", "${q(s)}.${q(n)}", 3) }.getOrNull()
            result.add(Dependent(s, n, "TRIGGER", recreate(source),
                before = if (replace) emptyList() else listOf("DROP TRIGGER IF EXISTS ${q(s)}.${q(n)};"), terminator = "delimiter",
                note = if (source == null) "Could not read the trigger." else null))
        }
        if (!replace) {
            notes.add("MySQL has no CREATE OR REPLACE for routines and triggers, so the script drops and creates them again " +
                "(each original definition is kept as a comment). With binary logging on, creating a function or trigger needs " +
                "SUPER or log_bin_trust_function_creators = 1.")
        }
        return result
    }

    private fun showCreate(c: Connection, kind: String, qualified: String, column: Int): String? =
        c.createStatement().use { st -> st.executeQuery("SHOW CREATE $kind $qualified").use { rs -> if (rs.next()) rs.getString(column) else null } }

    // ---------------------------------------------------------------------

    private fun rows(c: Connection, sql: String, vararg params: Any?): List<List<Any?>> =
        c.prepareStatement(sql.trimIndent()).use { statement ->
            params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
            statement.executeQuery().use { rs ->
                val count = rs.metaData.columnCount
                val list = ArrayList<List<Any?>>()
                while (rs.next() && list.size < MAX_OBJECTS) {
                    list.add((1..count).map { column ->
                        when (val value = rs.getObject(column)) {
                            is java.sql.Clob -> value.getSubString(1, value.length().toInt())
                            else -> value
                        }
                    })
                }
                list
            }
        }

    private fun scalar(c: Connection, sql: String, vararg params: Any?): String? =
        rows(c, sql, *params).firstOrNull()?.firstOrNull()?.toString()
}
