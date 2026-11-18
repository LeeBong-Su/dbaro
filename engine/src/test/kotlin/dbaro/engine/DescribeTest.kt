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
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * Oracle DESC. 실제 Oracle 없이 H2에 Oracle 딕셔너리 뷰와 같은 이름·컬럼의 표를 만들어 조회 흐름을 확인한다.
 * (SYS_CONTEXT는 H2 별칭으로 현재 스키마 HR을 돌려준다)
 */
class DescribeTest {
    companion object {
        @JvmStatic
        fun sysContext(namespace: String?, parameter: String?): String? =
            if (namespace == "USERENV" && parameter == "CURRENT_SCHEMA") "HR" else null
    }

    private lateinit var connection: Connection

    private fun describe(sql: String): Describe.Table =
        Describe.describe("oracle", connection, Describe.parse(sql) ?: error("not a DESC command: $sql"))

    @BeforeTest
    fun setUp() {
        connection = DriverManager.getConnection("jdbc:h2:mem:describe${System.nanoTime()};MODE=Oracle", "sa", "")
        connection.createStatement().use { s ->
            for (sql in listOf(
                "CREATE ALIAS SYS_CONTEXT FOR 'dbaro.engine.DescribeTest.sysContext'",
                "CREATE TABLE ALL_OBJECTS (OWNER VARCHAR(128), OBJECT_NAME VARCHAR(128), OBJECT_TYPE VARCHAR(30))",
                "CREATE TABLE ALL_TAB_COLUMNS (OWNER VARCHAR(128), TABLE_NAME VARCHAR(128), COLUMN_NAME VARCHAR(128), " +
                    "COLUMN_ID INT, NULLABLE CHAR(1), DATA_TYPE VARCHAR(128), DATA_TYPE_OWNER VARCHAR(128), DATA_LENGTH INT, " +
                    "DATA_PRECISION INT, DATA_SCALE INT, CHAR_LENGTH INT, CHAR_USED CHAR(1))",
                "CREATE TABLE ALL_COL_COMMENTS (OWNER VARCHAR(128), TABLE_NAME VARCHAR(128), COLUMN_NAME VARCHAR(128), COMMENTS VARCHAR(4000))",
                "CREATE TABLE ALL_SYNONYMS (OWNER VARCHAR(128), SYNONYM_NAME VARCHAR(128), TABLE_OWNER VARCHAR(128), " +
                    "TABLE_NAME VARCHAR(128), DB_LINK VARCHAR(128))",
                // 오래된 버전처럼 SUBPROGRAM_ID가 없는 ALL_ARGUMENTS (있는 컬럼만 골라 쓰는지 확인)
                "CREATE TABLE ALL_ARGUMENTS (OWNER VARCHAR(128), OBJECT_NAME VARCHAR(128), PACKAGE_NAME VARCHAR(128), " +
                    "OVERLOAD VARCHAR(40), ARGUMENT_NAME VARCHAR(128), POSITION INT, SEQUENCE INT, DATA_LEVEL INT, " +
                    "DATA_TYPE VARCHAR(30), IN_OUT VARCHAR(9), DEFAULTED VARCHAR(1), TYPE_OWNER VARCHAR(128), " +
                    "TYPE_NAME VARCHAR(128), TYPE_SUBNAME VARCHAR(128), DATA_PRECISION INT, DATA_SCALE INT)",
                "CREATE TABLE ALL_TYPE_ATTRS (OWNER VARCHAR(128), TYPE_NAME VARCHAR(128), ATTR_NAME VARCHAR(128), " +
                    "ATTR_TYPE_OWNER VARCHAR(128), ATTR_TYPE_NAME VARCHAR(128), LENGTH INT, PRECISION INT, SCALE INT, ATTR_NO INT)",
                "CREATE TABLE ALL_COLL_TYPES (OWNER VARCHAR(128), TYPE_NAME VARCHAR(128), COLL_TYPE VARCHAR(30), UPPER_BOUND INT, " +
                    "ELEM_TYPE_OWNER VARCHAR(128), ELEM_TYPE_NAME VARCHAR(128), LENGTH INT, PRECISION INT, SCALE INT)",

                // HR.EMP 테이블
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'EMP', 'TABLE')",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'EMP_IX', 'INDEX')",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('HR', 'EMP', 'NAME', 2, 'Y', 'VARCHAR2', NULL, 80, NULL, NULL, 20, 'C')",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('HR', 'EMP', 'ID', 1, 'N', 'NUMBER', NULL, 22, 10, 0, NULL, NULL)",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('HR', 'EMP', 'SALARY', 3, 'Y', 'NUMBER', NULL, 22, 9, 2, NULL, NULL)",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('HR', 'EMP', 'HIRED', 4, 'Y', 'TIMESTAMP(6)', NULL, 11, NULL, 6, NULL, NULL)",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('HR', 'EMP', 'SHAPE', 5, 'Y', 'SDO_GEOMETRY', 'MDSYS', 1, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_COL_COMMENTS VALUES ('HR', 'EMP', 'NAME', '사원 이름')",
                "INSERT INTO ALL_COL_COMMENTS VALUES ('HR', 'EMP', 'ID', NULL)",
                // 다른 스키마와 대소문자 이름
                "INSERT INTO ALL_OBJECTS VALUES ('SCOTT', 'BONUS', 'TABLE')",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('SCOTT', 'BONUS', 'ENAME', 1, 'Y', 'VARCHAR2', NULL, 10, NULL, NULL, 10, 'B')",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'Order Items', 'VIEW')",
                "INSERT INTO ALL_TAB_COLUMNS VALUES ('HR', 'Order Items', 'Line', 1, 'Y', 'NVARCHAR2', NULL, 40, NULL, NULL, 20, 'C')",
                // 시노님: PUBLIC STAFF → HR.EMP, 하지만 HR에 자기 STAFF 시노님(→ SCOTT.BONUS)이 있다
                "INSERT INTO ALL_OBJECTS VALUES ('PUBLIC', 'STAFF', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('PUBLIC', 'STAFF', 'HR', 'EMP', NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'STAFF', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'STAFF', 'SCOTT', 'BONUS', NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('PUBLIC', 'WORKERS', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('PUBLIC', 'WORKERS', 'HR', 'EMP', NULL)",
                // 시노님의 시노님, 원격 시노님, 끊긴 시노님, 순환 시노님
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'S1', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'S1', 'HR', 'S2', NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'S2', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'S2', 'HR', 'EMP', NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'REMOTE_EMP', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'REMOTE_EMP', 'HR', 'EMP', 'PROD.EXAMPLE')",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'BROKEN', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'BROKEN', 'HR', 'DROPPED_TABLE', NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'LOOP_A', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'LOOP_A', 'HR', 'LOOP_B', NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'LOOP_B', 'SYNONYM')",
                "INSERT INTO ALL_SYNONYMS VALUES ('HR', 'LOOP_B', 'HR', 'LOOP_A', NULL)",
                // 함수, 인자 없는 프로시저, 패키지
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'GET_SALARY', 'FUNCTION')",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'GET_SALARY', NULL, NULL, NULL, 0, 1, 0, 'NUMBER', 'OUT', 'N', NULL, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'GET_SALARY', NULL, NULL, 'P_ID', 1, 2, 0, 'NUMBER', 'IN', 'N', NULL, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'GET_SALARY', NULL, NULL, 'P_DATE', 2, 3, 0, 'DATE', 'IN', 'Y', NULL, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'REFRESH_ALL', 'PROCEDURE')",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'REFRESH_ALL', NULL, NULL, NULL, 1, 0, 0, NULL, 'IN', 'N', NULL, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'EMP_API', 'PACKAGE')",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'EMP_API', 'PACKAGE BODY')",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'HIRE', 'EMP_API', '1', 'P_NAME', 1, 1, 0, 'VARCHAR2', 'IN', 'N', NULL, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'HIRE', 'EMP_API', '2', 'P_EMP', 1, 1, 0, 'PL/SQL RECORD', 'IN', 'N', 'HR', 'EMP_API', 'EMP_REC', NULL, NULL)",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'HIRE', 'EMP_API', '2', 'FIELD', 1, 2, 1, 'NUMBER', 'IN', 'N', NULL, NULL, NULL, NULL, NULL)",
                "INSERT INTO ALL_ARGUMENTS VALUES ('HR', 'RESET', 'EMP_API', NULL, NULL, 1, 0, 0, NULL, 'IN', 'N', NULL, NULL, NULL, NULL, NULL)",
                // 객체 타입과 컬렉션 타입, 시퀀스
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'ADDRESS_T', 'TYPE')",
                "INSERT INTO ALL_TYPE_ATTRS VALUES ('HR', 'ADDRESS_T', 'CITY', NULL, 'VARCHAR2', 40, NULL, NULL, 2)",
                "INSERT INTO ALL_TYPE_ATTRS VALUES ('HR', 'ADDRESS_T', 'ZIP', NULL, 'NUMBER', NULL, 5, 0, 1)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'ADDRESS_LIST', 'TYPE')",
                "INSERT INTO ALL_COLL_TYPES VALUES ('HR', 'ADDRESS_LIST', 'VARYING ARRAY', 10, 'HR', 'ADDRESS_T', NULL, NULL, NULL)",
                "INSERT INTO ALL_OBJECTS VALUES ('HR', 'EMP_SEQ', 'SEQUENCE')",
            )) s.execute(sql)
        }
    }

    @AfterTest
    fun tearDown() {
        connection.close()
    }

    @Test
    fun recognisesDescribeCommands() {
        fun target(sql: String) = Describe.parse(sql)?.let {
            listOf(if (it.schemaQuoted) "q:" else "", it.schema, "|", if (it.nameQuoted) "q:" else "", it.name).joinToString("")
        }
        assertEquals("null|emp", target("DESC emp"))
        assertEquals("hr|emp", target("describe hr.emp;"))
        assertEquals("null|emp", target("Descr emp ; "))
        assertEquals("q:Hr|q:Emp \"Table\"", target("-- comment\n/* note */ DESC \"Hr\".\"Emp \"\"Table\"\"\""))
        assertEquals("null|EMP_2\$X#", target("desc EMP_2\$X# -- trailing comment"))
        assertEquals("null|사원", target("DESC 사원"))
        assertNull(target("SELECT * FROM emp"))
        assertNull(target("DESCRIPTION emp"))
        assertNull(target("DES emp"))
        assertNull(target("DESC_TABLE"))
        for (bad in listOf("DESC", "DESC ;", "DESC emp extra", "DESC hr.", "DESC \"emp", "DESC a.b.c")) {
            assertEquals(Describe.USAGE, assertFailsWith<SQLException>(bad) { Describe.parse(bad) }.message, bad)
        }
        assertTrue(assertFailsWith<SQLException> { Describe.parse("DESC emp@prod") }.message!!.contains("database link"))
    }

    @Test
    fun formatsTypesLikeSqlPlus() {
        assertEquals("VARCHAR2(20 CHAR)", Describe.oracleType("VARCHAR2", length = 80, charLength = 20, charUsed = "C"))
        assertEquals("VARCHAR2(80)", Describe.oracleType("VARCHAR2", length = 80, charLength = 80, charUsed = "B"))
        assertEquals("CHAR(1)", Describe.oracleType("CHAR", length = 1))
        assertEquals("NVARCHAR2(20)", Describe.oracleType("NVARCHAR2", length = 40, charLength = 20))
        assertEquals("NUMBER", Describe.oracleType("NUMBER", length = 22))
        assertEquals("NUMBER(38)", Describe.oracleType("NUMBER", scale = 0))
        assertEquals("NUMBER(10)", Describe.oracleType("NUMBER", precision = 10, scale = 0))
        assertEquals("NUMBER(9,2)", Describe.oracleType("NUMBER", precision = 9, scale = 2))
        assertEquals("FLOAT(126)", Describe.oracleType("FLOAT", precision = 126))
        assertEquals("RAW(16)", Describe.oracleType("RAW", length = 16))
        assertEquals("TIMESTAMP(6)", Describe.oracleType("TIMESTAMP(6)", length = 11, scale = 6))
        assertEquals("DATE", Describe.oracleType("DATE", length = 7))
        assertEquals("MDSYS.SDO_GEOMETRY", Describe.oracleType("SDO_GEOMETRY", typeOwner = "MDSYS"))
        assertEquals("XMLTYPE", Describe.oracleType("XMLTYPE", typeOwner = "SYS"))
    }

    @Test
    fun describesTableColumnsInOrderWithComments() {
        val table = describe("DESC emp")
        assertEquals(listOf("Name", "Null?", "Type", "Comment"), table.columns)
        assertEquals(
            listOf(
                listOf("ID", "NOT NULL", "NUMBER(10)", ""),
                listOf("NAME", "", "VARCHAR2(20 CHAR)", "사원 이름"),
                listOf("SALARY", "", "NUMBER(9,2)", ""),
                listOf("HIRED", "", "TIMESTAMP(6)", ""),
                listOf("SHAPE", "", "MDSYS.SDO_GEOMETRY", ""),
            ),
            table.rows,
        )
        assertEquals(listOf("ENAME", "", "VARCHAR2(10)", ""), describe("desc scott.bonus").rows.single())
        assertEquals(listOf("Line", "", "NVARCHAR2(20)", ""), describe("DESC \"Order Items\"").rows.single())
    }

    @Test
    fun followsSynonyms() {
        // 자기 스키마 시노님이 PUBLIC 시노님보다 먼저
        assertEquals("ENAME", describe("DESC staff").rows.single()[0])
        assertEquals(5, describe("DESC workers").rows.size) // PUBLIC 시노님
        assertEquals(5, describe("DESC public.staff").rows.size) // PUBLIC 시노님을 직접
        assertEquals(5, describe("DESC s1").rows.size) // 시노님 → 시노님 → 테이블

        fun error(sql: String) = assertFailsWith<SQLException>(sql) { describe(sql) }.message!!
        assertTrue(error("DESC remote_emp").contains("database link"), error("DESC remote_emp"))
        assertEquals("Synonym BROKEN points to HR.DROPPED_TABLE, which does not exist.", error("DESC broken"))
        assertTrue(error("DESC loop_a").contains("loop"), error("DESC loop_a"))
        assertEquals("Object NO_SUCH_TABLE does not exist.", error("DESC no_such_table"))
        assertEquals("Object SCOTT.WORKERS does not exist.", error("DESC scott.workers")) // 스키마를 쓰면 PUBLIC은 찾지 않는다
        assertEquals("Object emp does not exist.", error("DESC \"emp\"")) // 따옴표 이름은 대소문자 그대로
        assertTrue(error("DESC emp_seq").contains("does not support SEQUENCE"), error("DESC emp_seq"))
    }

    @Test
    fun describesProceduresFunctionsAndPackages() {
        val function = describe("DESC get_salary")
        assertEquals(listOf("Argument", "Type", "In/Out", "Default?"), function.columns)
        assertEquals(
            listOf(
                listOf("", "NUMBER", "RETURN", ""),
                listOf("P_ID", "NUMBER", "IN", ""),
                listOf("P_DATE", "DATE", "IN", "DEFAULT"),
            ),
            function.rows,
        )
        assertEquals(emptyList(), describe("DESC refresh_all").rows) // 인자 없는 프로시저

        val pkg = describe("DESC emp_api")
        assertEquals(listOf("Subprogram", "Overload", "Argument", "Type", "In/Out", "Default?"), pkg.columns)
        assertEquals(
            listOf(
                listOf("HIRE", "1", "P_NAME", "VARCHAR2", "IN", ""),
                listOf("HIRE", "2", "P_EMP", "HR.EMP_API.EMP_REC", "IN", ""), // 레코드 안의 필드(DATA_LEVEL 1)는 빼고
                listOf("RESET", "", "", "", "", ""), // 인자 없는 서브프로그램도 보인다
            ),
            pkg.rows,
        )
    }

    @Test
    fun describesObjectAndCollectionTypes() {
        val type = describe("DESC address_t")
        assertEquals(listOf("Name", "Type"), type.columns)
        assertEquals(listOf(listOf("ZIP", "NUMBER(5)"), listOf("CITY", "VARCHAR2(40)")), type.rows)
        assertEquals(listOf(listOf("ADDRESS_LIST", "VARRAY(10) OF HR.ADDRESS_T")), describe("DESC address_list").rows)
    }

    @Test
    fun oracleSessionAnswersDescInsteadOfSendingItToTheServer() {
        Session("s1", "oracle", connection).let { session ->
            val result = session.execute("DESC emp", 500, 0)
            assertEquals("resultSet", result["kind"])
            assertEquals(listOf("Name", "Null?", "Type", "Comment"), (result["columns"] as List<*>).map { (it as Map<*, *>)["name"] })
            assertEquals(5, (result["rows"] as List<*>).size)
            assertEquals(false, result["hasMore"])
            // DESC가 아닌 문장은 그대로 서버로 간다
            assertEquals("resultSet", session.execute("SELECT 1 FROM DUAL", 500, 0)["kind"])
        }
        // 다른 DB에서는 가로채지 않는다 (H2가 DESC를 모르므로 서버 오류)
        // (세션을 닫으면 연결도 닫히므로 새 연결로)
        Session("s2", "generic", DriverManager.getConnection("jdbc:h2:mem:")).use { session ->
            assertFailsWith<SQLException> { session.execute("DESC emp", 500, 0) }
        }
    }
}
