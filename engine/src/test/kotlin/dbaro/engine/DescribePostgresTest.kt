package dbaro.engine

import dbaro.engine.exec.Describe
import dbaro.engine.exec.Session
import java.sql.Connection
import java.sql.DriverManager
import java.sql.SQLException
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/**
 * PostgreSQL DESC. 실제 서버 없이 H2에 pg_catalog 표와 함수(별칭)를 흉내 내어 조회 흐름을 확인한다.
 * 카탈로그 SQL 자체가 실제 PostgreSQL에서 맞는지는 이 테스트로 알 수 없다.
 */
class DescribePostgresTest {
    companion object {
        // search_path: app, public. public.emp는 app.emp에 가려진다.
        private val visibleRelations = setOf(10L, 12L, 13L, 14L, 15L, 16L)
        private val types = mapOf(23L to "integer", 1043L to "character varying", 1700L to "numeric", 25L to "text")
        private val comments = mapOf((10L to 2) to "사원 이름")
        private val visibleFunctions = setOf(100L, 101L, 102L)
        private val arguments = mapOf(100L to "p_id integer", 101L to "p_id integer, p_name text DEFAULT 'x'::text", 102L to "")
        private val results = mapOf(100L to "numeric", 101L to "numeric", 102L to null)

        @JvmStatic fun tableIsVisible(oid: Long?): Boolean = oid in visibleRelations

        @JvmStatic fun functionIsVisible(oid: Long?): Boolean = oid in visibleFunctions

        @JvmStatic fun formatType(type: Long?, typmod: Int?): String? {
            val name = types[type] ?: return null
            return if (typmod != null && typmod > 4 && type == 1043L) "$name(${typmod - 4})" else name
        }

        @JvmStatic fun getExpr(expression: String?, relation: Long?): String? = expression

        @JvmStatic fun colDescription(relation: Long?, column: Int?): String? = comments[relation to column]

        @JvmStatic fun functionArguments(oid: Long?): String? = arguments[oid]

        @JvmStatic fun functionResult(oid: Long?): String? = results[oid]
    }

    private lateinit var connection: Connection

    private fun describe(sql: String): Describe.Table =
        Describe.describe("postgres", connection, Describe.parse(sql) ?: error("not a DESC command: $sql"))

    @BeforeTest
    fun setUp() {
        connection = DriverManager.getConnection("jdbc:h2:mem:pgdescribe${System.nanoTime()}", "sa", "")
        val self = "dbaro.engine.DescribePostgresTest"
        connection.createStatement().use { s ->
            for (sql in listOf(
                "CREATE SCHEMA IF NOT EXISTS pg_catalog",
                "CREATE TABLE pg_catalog.pg_namespace (oid BIGINT, nspname VARCHAR(64))",
                "CREATE TABLE pg_catalog.pg_class (oid BIGINT, relname VARCHAR(64), relnamespace BIGINT, relkind CHAR(1))",
                "CREATE TABLE pg_catalog.pg_attribute (attrelid BIGINT, attname VARCHAR(64), attnum INT, atttypid BIGINT, " +
                    "atttypmod INT, attnotnull BOOLEAN, attisdropped BOOLEAN, attidentity CHAR(1), attgenerated CHAR(1))",
                "CREATE TABLE pg_catalog.pg_attrdef (adrelid BIGINT, adnum INT, adbin VARCHAR(200))",
                "CREATE TABLE pg_catalog.pg_proc (oid BIGINT, proname VARCHAR(64), pronamespace BIGINT, pronargs INT, prokind CHAR(1))",
                "CREATE ALIAS pg_catalog.pg_table_is_visible FOR '$self.tableIsVisible'",
                "CREATE ALIAS pg_catalog.pg_function_is_visible FOR '$self.functionIsVisible'",
                "CREATE ALIAS pg_catalog.format_type FOR '$self.formatType'",
                "CREATE ALIAS pg_catalog.pg_get_expr FOR '$self.getExpr'",
                "CREATE ALIAS pg_catalog.col_description FOR '$self.colDescription'",
                "CREATE ALIAS pg_catalog.pg_get_function_arguments FOR '$self.functionArguments'",
                "CREATE ALIAS pg_catalog.pg_get_function_result FOR '$self.functionResult'",

                "INSERT INTO pg_catalog.pg_namespace VALUES (1, 'app'), (2, 'public'), (3, 'sales')",
                // app.emp (보임), public.emp (가려짐), sales.orders, 대소문자 이름, 뷰, 시퀀스, 인덱스, 복합 타입
                "INSERT INTO pg_catalog.pg_class VALUES (10, 'emp', 1, 'r'), (11, 'emp', 2, 'r'), (12, 'Order Items', 1, 'r'), " +
                    "(13, 'emp_v', 1, 'v'), (14, 'emp_seq', 1, 'S'), (15, 'emp_pk', 1, 'i'), (16, 'address', 1, 'c'), " +
                    "(20, 'orders', 3, 'p')",
                "INSERT INTO pg_catalog.pg_attribute VALUES " +
                    "(10, 'id', 1, 23, -1, TRUE, FALSE, 'a', ''), " +
                    "(10, 'name', 2, 1043, 44, TRUE, FALSE, '', ''), " +
                    "(10, '........pg.dropped.3........', 3, 0, -1, FALSE, TRUE, '', ''), " +
                    "(10, 'salary', 4, 1700, -1, FALSE, FALSE, '', ''), " +
                    "(10, 'bonus', 5, 1700, -1, FALSE, FALSE, '', 's'), " +
                    "(10, 'ctid', -1, 0, -1, TRUE, FALSE, '', ''), " +
                    "(11, 'old_column', 1, 25, -1, FALSE, FALSE, '', ''), " +
                    "(12, 'Line', 1, 23, -1, FALSE, FALSE, '', ''), " +
                    "(16, 'city', 1, 25, -1, FALSE, FALSE, '', ''), " +
                    "(20, 'order_id', 1, 23, -1, TRUE, FALSE, 'd', '')",
                "INSERT INTO pg_catalog.pg_attrdef VALUES (10, 4, '0'), (10, 5, '(salary * 0.1)')",
                // 함수 오버로드 두 개(보임), 다른 스키마의 같은 이름(안 보임), 프로시저
                "INSERT INTO pg_catalog.pg_proc VALUES (100, 'get_salary', 1, 1, 'f'), (101, 'get_salary', 1, 2, 'f'), " +
                    "(103, 'get_salary', 3, 1, 'f'), (102, 'refresh_all', 1, 0, 'p')",
            )) s.execute(sql)
        }
    }

    @AfterTest
    fun tearDown() {
        connection.close()
    }

    @Test
    fun describesColumnsWithDefaultsIdentityAndComments() {
        val table = describe("DESC EMP") // 따옴표 없는 이름은 소문자로
        assertEquals(listOf("Name", "Null?", "Type", "Default", "Comment"), table.columns)
        assertEquals(
            listOf(
                listOf("id", "NOT NULL", "integer", "GENERATED ALWAYS AS IDENTITY", ""),
                listOf("name", "NOT NULL", "character varying(40)", "", "사원 이름"),
                listOf("salary", "", "numeric", "0", ""),
                listOf("bonus", "", "numeric", "GENERATED ALWAYS AS ((salary * 0.1)) STORED", ""),
            ),
            table.rows, // 지운 컬럼과 시스템 컬럼은 빠진다
        )
    }

    @Test
    fun resolvesNamesLikePsql() {
        assertEquals("old_column", describe("desc public.emp").rows.single()[0]) // 스키마를 쓰면 그 스키마
        assertEquals("Line", describe("DESC \"Order Items\"").rows.single()[0])
        assertEquals("Name", describe("DESC emp_v").columns.first()) // 뷰도 컬럼 표 (가짜 카탈로그에는 컬럼이 없다)
        assertEquals("city", describe("DESC address").rows.single()[0]) // 복합 타입
        assertEquals(listOf("order_id", "NOT NULL", "integer", "GENERATED BY DEFAULT AS IDENTITY", ""), describe("DESC sales.orders").rows.single())

        fun error(sql: String) = assertFailsWith<SQLException>(sql) { describe(sql) }.message!!
        assertEquals("Object orders does not exist.", error("DESC orders")) // search_path에 없는 스키마
        assertEquals("Object app.nope does not exist.", error("DESC app.nope"))
        assertTrue(error("DESC emp_seq").contains("does not support SEQUENCE"), error("DESC emp_seq"))
        assertTrue(error("DESC emp_pk").contains("does not support INDEX"), error("DESC emp_pk"))
    }

    @Test
    fun describesFunctionsAndProcedures() {
        val functions = describe("DESC get_salary")
        assertEquals(listOf("Function", "Arguments", "Returns", "Kind"), functions.columns)
        assertEquals(
            listOf(
                listOf("app.get_salary", "p_id integer", "numeric", "FUNCTION"),
                listOf("app.get_salary", "p_id integer, p_name text DEFAULT 'x'::text", "numeric", "FUNCTION"),
            ),
            functions.rows,
        )
        assertEquals(listOf(listOf("app.refresh_all", "", "", "PROCEDURE")), describe("DESC refresh_all").rows)
    }

    @Test
    fun postgresSessionKeepsTheOpenTransactionUsable() {
        connection.autoCommit = false
        val session = Session("p1", "postgres", connection)
        connection.createStatement().use { it.execute("CREATE TABLE work (id INT)") }
        session.execute("INSERT INTO work VALUES (1)", 500, 0)
        assertEquals("resultSet", session.execute("DESC emp", 500, 0)["kind"])
        assertFailsWith<SQLException> { session.execute("DESC no_such_thing", 500, 0) }
        // 실패한 DESC 뒤에도 같은 트랜잭션에서 계속 작업할 수 있고, 앞의 INSERT도 남아 있다
        val result = session.execute("SELECT COUNT(*) FROM work", 500, 0)
        assertEquals(1L, ((result["rows"] as List<*>).single() as List<*>).single())
    }
}
