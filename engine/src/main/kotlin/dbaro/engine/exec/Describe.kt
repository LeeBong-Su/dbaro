package dbaro.engine.exec

import java.sql.Connection
import java.sql.SQLException

/**
 * DESC[RIBE] [schema.]object. SQL*Plus(Oracle)와 psql(\d)의 클라이언트 명령이라 서버가 모르므로(ORA-00900, syntax error)
 * 딕셔너리·카탈로그를 조회해 같은 내용의 표로 만든다. MySQL·MariaDB의 DESC는 서버가 직접 처리하므로 쓰지 않는다.
 *
 * Oracle·Tibero
 * - 테이블·뷰·구체화 뷰: Name, Null?, Type, Comment (컬럼 순서대로)
 * - 프로시저·함수: Argument, Type, In/Out, Default? (함수의 반환값은 In/Out이 RETURN인 줄)
 * - 패키지: 위 내용에 Subprogram, Overload를 앞에 붙여 모든 서브프로그램을 보여 준다
 * - 객체 타입: 속성 이름과 타입. 컬렉션 타입: 요소 타입 한 줄
 * - 따옴표로 감싸지 않은 이름은 대문자. 스키마가 없으면 현재 스키마의 객체, 그다음 자기 시노님, PUBLIC 시노님 순서로 찾고
 *   시노님은 가리키는 객체까지 따라간다
 *
 * PostgreSQL
 * - 테이블·뷰·구체화 뷰·외부 테이블·복합 타입: Name, Null?, Type, Default, Comment
 * - 함수·프로시저: 오버로드마다 Function, Arguments, Returns, Kind
 * - 따옴표로 감싸지 않은 이름은 소문자. 스키마가 없으면 search_path에서 보이는 객체 (psql의 \d와 같다)
 *
 * 버전·제품마다 딕셔너리의 컬럼이 조금씩 달라서 있는 컬럼만 골라 조회한다.
 */
object Describe {
    class Target(val schema: String?, val name: String, val schemaQuoted: Boolean, val nameQuoted: Boolean)

    class Table(val columns: List<String>, val rows: List<List<String>>)

    const val USAGE = "Usage: DESC[RIBE] [schema.]object"

    private val COMMANDS = setOf("DESC", "DESCR", "DESCRI", "DESCRIB", "DESCRIBE")
    private const val MAX_SYNONYM_HOPS = 10

    /** 이 DB에서 DESC를 직접 처리하는지 (MySQL·MariaDB는 서버가 처리한다) */
    fun supports(dbType: String) = dbType == "oracle" || dbType == "tibero" || dbType == "postgres"

    /** DESC 명령이면 대상, 다른 문장이면 null. DESC인데 대상이 잘못되었으면 SQLException. */
    fun parse(sql: String): Target? {
        var i = skipSpaceAndComments(sql, 0)
        val keywordStart = i
        while (i < sql.length && sql[i].isLetter()) i++
        if (sql.substring(keywordStart, i).uppercase() !in COMMANDS) return null
        if (i < sql.length && !sql[i].isWhitespace() && sql[i] != '"' && sql[i] != ';' && sql[i] != '-' && sql[i] != '/') return null

        i = skipSpaceAndComments(sql, i)
        val first = readPart(sql, i) ?: throw SQLException(USAGE)
        i = first.end
        var second: Part? = null
        if (i < sql.length && sql[i] == '.') {
            second = readPart(sql, i + 1) ?: throw SQLException(USAGE)
            i = second.end
        }
        if (i < sql.length && sql[i] == '@')
            throw SQLException("DESC over a database link is not supported.")
        i = skipSpaceAndComments(sql, i)
        if (i < sql.length && sql[i] == ';') i = skipSpaceAndComments(sql, i + 1)
        if (i < sql.length) throw SQLException(USAGE)

        return if (second == null) Target(null, first.text, false, first.quoted)
        else Target(first.text, second.text, first.quoted, second.quoted)
    }

    fun describe(dbType: String, connection: Connection, target: Target): Table = when (dbType) {
        "postgres" -> Postgres(connection).describe(target)
        else -> Oracle(connection).describe(target)
    }

    // ---------------------------------------------------------------------
    // 이름 읽기

    private class Part(val text: String, val quoted: Boolean, val end: Int)

    private fun readPart(s: String, from: Int): Part? {
        if (from >= s.length) return null
        if (s[from] == '"') {
            val text = StringBuilder()
            var j = from + 1
            while (j < s.length) {
                if (s[j] == '"') {
                    if (j + 1 < s.length && s[j + 1] == '"') {
                        text.append('"')
                        j += 2
                        continue
                    }
                    return if (text.isEmpty()) null else Part(text.toString(), true, j + 1)
                }
                text.append(s[j])
                j++
            }
            return null // 닫는 따옴표가 없다
        }
        var j = from
        while (j < s.length && (s[j].isLetterOrDigit() || s[j] == '_' || s[j] == '$' || s[j] == '#')) j++
        return if (j == from) null else Part(s.substring(from, j), false, j)
    }

    private fun skipSpaceAndComments(s: String, from: Int): Int {
        var i = from
        while (i < s.length) {
            when {
                s[i].isWhitespace() -> i++
                s.startsWith("--", i) -> i = s.indexOf('\n', i).let { if (it < 0) s.length else it + 1 }
                s.startsWith("/*", i) -> i = s.indexOf("*/", i + 2).let { if (it < 0) s.length else it + 2 }
                else -> return i
            }
        }
        return i
    }

    // ---------------------------------------------------------------------
    // Oracle 타입 표시 (SQL*Plus와 같은 모양)

    private val BUILTIN_TYPE_OWNERS = setOf("SYS", "PUBLIC")

    internal fun oracleType(
        dataType: String?,
        typeOwner: String? = null,
        length: Int? = null,
        precision: Int? = null,
        scale: Int? = null,
        charLength: Int? = null,
        charUsed: String? = null,
    ): String {
        val type = dataType?.trim().orEmpty()
        return when (type) {
            "" -> ""
            "VARCHAR2", "VARCHAR", "CHAR" -> when {
                charUsed == "C" && (charLength ?: 0) > 0 -> "$type($charLength CHAR)"
                (length ?: 0) > 0 -> "$type($length)"
                else -> type
            }
            "NVARCHAR2", "NCHAR" -> when {
                (charLength ?: 0) > 0 -> "$type($charLength)"
                (length ?: 0) > 0 -> "$type(${length!! / 2})" // AL16UTF16: 글자당 2바이트
                else -> type
            }
            "NUMBER" -> when {
                precision == null && scale == null -> "NUMBER"
                precision == null -> if (scale == 0) "NUMBER(38)" else "NUMBER(38,$scale)"
                scale == null || scale == 0 -> "NUMBER($precision)"
                else -> "NUMBER($precision,$scale)"
            }
            "FLOAT" -> if (precision != null) "FLOAT($precision)" else "FLOAT"
            "RAW", "UROWID" -> if ((length ?: 0) > 0) "$type($length)" else type
            else -> if (!typeOwner.isNullOrEmpty() && typeOwner !in BUILTIN_TYPE_OWNERS) "$typeOwner.$type" else type
        }
    }

    // ---------------------------------------------------------------------
    // 조회 도우미

    private class Row(private val values: Map<String, Any?>) {
        fun string(column: String): String? = values[column]?.toString()
        fun int(column: String): Int? = when (val value = values[column]) {
            null -> null
            is Number -> value.toInt()
            else -> value.toString().trim().toIntOrNull()
        }
        fun bool(column: String): Boolean = when (val value = values[column]) {
            is Boolean -> value
            null -> false
            else -> value.toString().let { it == "t" || it == "true" || it == "1" }
        }
    }

    private abstract class Dictionary(val c: Connection) {
        private val viewColumns = HashMap<String, Set<String>>()

        /** 딕셔너리 뷰에 실제로 있는 컬럼 (DB 버전·제품마다 다르다) */
        protected fun columnsOf(view: String): Set<String> = viewColumns.getOrPut(view) {
            c.createStatement().use { statement ->
                statement.executeQuery("SELECT * FROM $view WHERE 1 = 0").use { rs ->
                    (1..rs.metaData.columnCount).map { rs.metaData.getColumnLabel(it).uppercase() }.toSet()
                }
            }
        }

        protected fun rows(sql: String, vararg params: Any?): List<Row> =
            c.prepareStatement(sql).use { statement ->
                params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
                statement.executeQuery().use { rs ->
                    val meta = rs.metaData
                    val labels = (1..meta.columnCount).map { meta.getColumnLabel(it).uppercase() }
                    buildList {
                        while (rs.next()) add(Row(labels.withIndex().associate { (index, label) -> label to rs.getObject(index + 1) }))
                    }
                }
            }

        protected fun strings(sql: String, vararg params: Any?): List<String?> =
            c.prepareStatement(sql).use { statement ->
                params.forEachIndexed { index, value -> statement.setObject(index + 1, value) }
                statement.executeQuery().use { rs -> buildList { while (rs.next()) add(rs.getString(1)) } }
            }
    }

    // ---------------------------------------------------------------------
    // Oracle·Tibero 딕셔너리

    private class Oracle(c: Connection) : Dictionary(c) {
        /** 같은 이름이 여러 종류로 있을 때 (예: 구체화 뷰는 TABLE도 있다) 앞의 것을 고른다. */
        private val typeOrder = listOf(
            "TABLE", "MATERIALIZED VIEW", "VIEW", "PACKAGE", "PROCEDURE", "FUNCTION", "TYPE", "SYNONYM",
            "SEQUENCE", "INDEX", "TRIGGER",
        )

        fun describe(target: Target): Table {
            val current = currentSchema()
            var owner = target.schema?.let { if (target.schemaQuoted) it else it.uppercase() } ?: current
            var name = if (target.nameQuoted) target.name else target.name.uppercase()
            val shown = if (target.schema == null) name else "$owner.$name"
            var type = objectType(owner, name)

            // 시노님은 가리키는 객체까지 따라간다. 스키마 없이 쓴 이름은 PUBLIC 시노님도 찾는다.
            var hops = 0
            while (type == null || type == "SYNONYM") {
                val publicToo = type == null && target.schema == null && hops == 0
                if (type == null && !publicToo) break
                val synonym = synonym(owner, name, publicToo) ?: break
                if (++hops > MAX_SYNONYM_HOPS) throw SQLException("Synonym $shown is part of a loop or a chain that is too long.")
                if (synonym.dbLink != null)
                    throw SQLException("$shown is a synonym for an object over a database link, which DESC does not support.")
                owner = synonym.owner
                name = synonym.name
                type = objectType(owner, name)
            }
            if (type == null) {
                if (hops > 0) throw SQLException("Synonym $shown points to $owner.$name, which does not exist.")
                throw SQLException("Object $shown does not exist.")
            }

            return when (type) {
                "TABLE", "VIEW", "MATERIALIZED VIEW" -> columns(owner, name)
                "PROCEDURE", "FUNCTION" -> arguments(owner, name, packaged = false)
                "PACKAGE" -> arguments(owner, name, packaged = true)
                "TYPE" -> typeAttributes(owner, name)
                else -> throw SQLException("DESC does not support $type objects ($owner.$name).")
            }
        }

        private fun currentSchema(): String =
            runCatching { strings("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM DUAL").firstOrNull() }.getOrNull()
                ?: runCatching { c.schema }.getOrNull()
                ?: throw SQLException("Could not determine the current schema.")

        private fun objectType(owner: String, name: String): String? {
            val types = strings("SELECT OBJECT_TYPE FROM ALL_OBJECTS WHERE OWNER = ? AND OBJECT_NAME = ?", owner, name)
                .filterNotNull().toSet()
            return typeOrder.firstOrNull { it in types } ?: types.firstOrNull()
        }

        private class Synonym(val owner: String, val name: String, val dbLink: String?)

        private fun synonym(owner: String, name: String, publicToo: Boolean): Synonym? {
            val sql = "SELECT TABLE_OWNER, TABLE_NAME, DB_LINK FROM ALL_SYNONYMS WHERE SYNONYM_NAME = ? AND " +
                if (publicToo) "OWNER IN (?, 'PUBLIC') ORDER BY CASE WHEN OWNER = 'PUBLIC' THEN 1 ELSE 0 END" else "OWNER = ?"
            val row = rows(sql, name, owner).firstOrNull() ?: return null
            val targetOwner = row.string("TABLE_OWNER") ?: owner
            val targetName = row.string("TABLE_NAME") ?: return null
            return Synonym(targetOwner, targetName, row.string("DB_LINK")?.takeIf { it.isNotBlank() })
        }

        private fun columns(owner: String, name: String): Table {
            val available = columnsOf("ALL_TAB_COLUMNS")
            val select = listOf(
                "COLUMN_NAME", "NULLABLE", "DATA_TYPE", "DATA_TYPE_OWNER", "DATA_LENGTH", "DATA_PRECISION", "DATA_SCALE",
                "CHAR_LENGTH", "CHAR_USED",
            ).filter { it in available }
            val order = if ("COLUMN_ID" in available) "COLUMN_ID, COLUMN_NAME" else "COLUMN_NAME"
            val columns = rows(
                "SELECT ${select.joinToString(", ")} FROM ALL_TAB_COLUMNS WHERE OWNER = ? AND TABLE_NAME = ? ORDER BY $order",
                owner, name,
            )
            val comments = runCatching {
                rows(
                    "SELECT COLUMN_NAME, COMMENTS FROM ALL_COL_COMMENTS WHERE OWNER = ? AND TABLE_NAME = ? AND COMMENTS IS NOT NULL",
                    owner, name,
                ).associate { it.string("COLUMN_NAME") to it.string("COMMENTS") }
            }.getOrDefault(emptyMap())

            return Table(
                listOf("Name", "Null?", "Type", "Comment"),
                columns.map { row ->
                    val column = row.string("COLUMN_NAME").orEmpty()
                    listOf(
                        column,
                        if (row.string("NULLABLE") == "N") "NOT NULL" else "",
                        oracleType(
                            row.string("DATA_TYPE"), row.string("DATA_TYPE_OWNER"), row.int("DATA_LENGTH"),
                            row.int("DATA_PRECISION"), row.int("DATA_SCALE"), row.int("CHAR_LENGTH"), row.string("CHAR_USED"),
                        ),
                        comments[column].orEmpty(),
                    )
                },
            )
        }

        private fun arguments(owner: String, name: String, packaged: Boolean): Table {
            val available = columnsOf("ALL_ARGUMENTS")
            val select = listOf(
                "OBJECT_NAME", "OVERLOAD", "ARGUMENT_NAME", "POSITION", "DATA_TYPE", "IN_OUT", "DEFAULTED",
                "TYPE_OWNER", "TYPE_NAME", "TYPE_SUBNAME", "DATA_PRECISION", "DATA_SCALE",
            ).filter { it in available }
            val where = StringBuilder("OWNER = ? AND ")
            where.append(if (packaged) "PACKAGE_NAME = ?" else "OBJECT_NAME = ? AND PACKAGE_NAME IS NULL")
            if ("DATA_LEVEL" in available) where.append(" AND DATA_LEVEL = 0")
            val order = buildList {
                if (packaged) add(if ("SUBPROGRAM_ID" in available) "SUBPROGRAM_ID" else "OBJECT_NAME")
                if ("OVERLOAD" in available) add("OVERLOAD")
                add(if ("SEQUENCE" in available) "SEQUENCE" else "POSITION")
            }
            val arguments = rows(
                "SELECT ${select.joinToString(", ")} FROM ALL_ARGUMENTS WHERE $where ORDER BY ${order.joinToString(", ")}",
                owner, name,
            )

            val rows = arguments.mapNotNull { row ->
                val argument = row.string("ARGUMENT_NAME")
                val dataType = row.string("DATA_TYPE")
                val isReturn = argument == null && row.int("POSITION") == 0
                // 인자가 없는 프로시저는 이름도 타입도 없는 줄 하나가 있다. 패키지에서는 서브프로그램이 보이도록 남긴다.
                if (argument == null && dataType == null && !isReturn && !packaged) return@mapNotNull null
                val typeName = row.string("TYPE_NAME")
                val type = if (typeName != null) {
                    listOfNotNull(row.string("TYPE_OWNER"), typeName, row.string("TYPE_SUBNAME")).joinToString(".")
                } else {
                    oracleType(dataType, null, null, row.int("DATA_PRECISION"), row.int("DATA_SCALE"))
                }
                val inOut = when {
                    isReturn -> "RETURN"
                    argument == null && dataType == null -> ""
                    else -> row.string("IN_OUT").orEmpty()
                }
                val values = listOf(argument.orEmpty(), type, inOut, if (row.string("DEFAULTED") == "Y") "DEFAULT" else "")
                if (packaged) listOf(row.string("OBJECT_NAME").orEmpty(), row.string("OVERLOAD").orEmpty()) + values else values
            }
            val header = listOf("Argument", "Type", "In/Out", "Default?")
            return Table(if (packaged) listOf("Subprogram", "Overload") + header else header, rows)
        }

        private fun typeAttributes(owner: String, name: String): Table {
            val available = columnsOf("ALL_TYPE_ATTRS")
            val select = listOf("ATTR_NAME", "ATTR_TYPE_OWNER", "ATTR_TYPE_NAME", "LENGTH", "PRECISION", "SCALE")
                .filter { it in available }
            val order = if ("ATTR_NO" in available) "ATTR_NO" else "ATTR_NAME"
            val attributes = rows(
                "SELECT ${select.joinToString(", ")} FROM ALL_TYPE_ATTRS WHERE OWNER = ? AND TYPE_NAME = ? ORDER BY $order",
                owner, name,
            )
            if (attributes.isNotEmpty()) {
                return Table(
                    listOf("Name", "Type"),
                    attributes.map { row ->
                        listOf(
                            row.string("ATTR_NAME").orEmpty(),
                            oracleType(
                                row.string("ATTR_TYPE_NAME"), row.string("ATTR_TYPE_OWNER"), row.int("LENGTH"),
                                row.int("PRECISION"), row.int("SCALE"),
                            ),
                        )
                    },
                )
            }

            // 컬렉션 타입 (TABLE OF, VARRAY)
            val collection = runCatching {
                rows(
                    "SELECT COLL_TYPE, UPPER_BOUND, ELEM_TYPE_OWNER, ELEM_TYPE_NAME, LENGTH, PRECISION, SCALE " +
                        "FROM ALL_COLL_TYPES WHERE OWNER = ? AND TYPE_NAME = ?",
                    owner, name,
                ).firstOrNull()
            }.getOrNull()
            val rows = if (collection == null) emptyList() else {
                val element = oracleType(
                    collection.string("ELEM_TYPE_NAME"), collection.string("ELEM_TYPE_OWNER"), collection.int("LENGTH"),
                    collection.int("PRECISION"), collection.int("SCALE"),
                )
                val kind = collection.string("COLL_TYPE").orEmpty()
                val type = if (kind.startsWith("VARYING") || kind == "VARRAY") "VARRAY(${collection.int("UPPER_BOUND") ?: ""}) OF $element"
                else "TABLE OF $element"
                listOf(listOf(name, type))
            }
            return Table(listOf("Name", "Type"), rows)
        }
    }

    // ---------------------------------------------------------------------
    // PostgreSQL 카탈로그

    private class Postgres(c: Connection) : Dictionary(c) {
        fun describe(target: Target): Table {
            val schema = target.schema?.let { if (target.schemaQuoted) it else it.lowercase() }
            val name = if (target.nameQuoted) target.name else target.name.lowercase()
            val shown = if (schema == null) name else "$schema.$name"
            val params = listOfNotNull(name, schema).toTypedArray()

            // 스키마가 없으면 psql의 \d처럼 search_path에서 보이는 것 (pg_temp 포함, 앞 스키마가 이긴다)
            val relation = rows(
                "SELECT c.relkind AS relkind, n.nspname AS nspname FROM pg_catalog.pg_class c " +
                    "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = ? AND " +
                    (if (schema == null) "pg_catalog.pg_table_is_visible(c.oid)" else "n.nspname = ?"),
                *params,
            ).firstOrNull()
            if (relation != null) {
                val owner = relation.string("NSPNAME").orEmpty()
                return when (val kind = relation.string("RELKIND")) {
                    "r", "p", "v", "m", "f", "c" -> columns(owner, name)
                    else -> throw SQLException("DESC does not support ${relkindName(kind)} objects ($owner.$name).")
                }
            }
            val routines = routines(schema, name, params)
            if (routines.rows.isNotEmpty()) return routines
            throw SQLException("Object $shown does not exist.")
        }

        private fun relkindName(kind: String?) = when (kind) {
            "S" -> "SEQUENCE"
            "i", "I" -> "INDEX"
            "t" -> "TOAST TABLE"
            else -> "relkind '$kind'"
        }

        private fun columns(schema: String, name: String): Table {
            val available = columnsOf("pg_catalog.pg_attribute")
            val identity = if ("ATTIDENTITY" in available) "a.attidentity" else "''" // PostgreSQL 10+
            val generated = if ("ATTGENERATED" in available) "a.attgenerated" else "''" // PostgreSQL 12+
            val columns = rows(
                "SELECT a.attname AS name, pg_catalog.format_type(a.atttypid, a.atttypmod) AS type, a.attnotnull AS not_null, " +
                    "pg_catalog.pg_get_expr(d.adbin, d.adrelid) AS default_value, " +
                    "pg_catalog.col_description(a.attrelid, a.attnum) AS comment, " +
                    "$identity AS identity_kind, $generated AS generated_kind " +
                    "FROM pg_catalog.pg_attribute a " +
                    "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum " +
                    "WHERE a.attrelid = (SELECT c.oid FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n " +
                    "ON n.oid = c.relnamespace WHERE n.nspname = ? AND c.relname = ?) " +
                    "AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum",
                schema, name,
            )
            return Table(
                listOf("Name", "Null?", "Type", "Default", "Comment"),
                columns.map { row ->
                    val expression = row.string("DEFAULT_VALUE").orEmpty()
                    val default = when {
                        row.string("IDENTITY_KIND") == "a" -> "GENERATED ALWAYS AS IDENTITY"
                        row.string("IDENTITY_KIND") == "d" -> "GENERATED BY DEFAULT AS IDENTITY"
                        row.string("GENERATED_KIND") == "s" -> "GENERATED ALWAYS AS ($expression) STORED"
                        else -> expression
                    }
                    listOf(
                        row.string("NAME").orEmpty(),
                        if (row.bool("NOT_NULL")) "NOT NULL" else "",
                        row.string("TYPE").orEmpty(),
                        default,
                        row.string("COMMENT").orEmpty(),
                    )
                },
            )
        }

        /** 함수·프로시저. 오버로드마다 한 줄 */
        private fun routines(schema: String?, name: String, params: Array<String>): Table {
            val kind = if ("PROKIND" in columnsOf("pg_catalog.pg_proc")) "p.prokind" else "'f'" // PostgreSQL 11+
            val routines = rows(
                "SELECT n.nspname AS nspname, pg_catalog.pg_get_function_arguments(p.oid) AS arguments, " +
                    "pg_catalog.pg_get_function_result(p.oid) AS result, $kind AS kind " +
                    "FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace " +
                    "WHERE p.proname = ? AND " +
                    (if (schema == null) "pg_catalog.pg_function_is_visible(p.oid)" else "n.nspname = ?") +
                    " ORDER BY n.nspname, p.pronargs, 2",
                *params,
            )
            return Table(
                listOf("Function", "Arguments", "Returns", "Kind"),
                routines.map { row ->
                    listOf(
                        "${row.string("NSPNAME")}.$name",
                        row.string("ARGUMENTS").orEmpty(),
                        row.string("RESULT").orEmpty(),
                        when (row.string("KIND")) {
                            "p" -> "PROCEDURE"
                            "a" -> "AGGREGATE"
                            "w" -> "WINDOW"
                            else -> "FUNCTION"
                        },
                    )
                },
            )
        }
    }
}
