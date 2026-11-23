package dbaro.engine

import dbaro.engine.exec.Ddl
import dbaro.engine.exec.PlanObject
import dbaro.engine.exec.Session
import java.sql.DriverManager
import java.sql.SQLException
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/** 테이블 구조 조회와 문장 묶음 실행 (H2) */
class TableInfoTest {
    private lateinit var session: Session

    @BeforeTest
    fun setUp() {
        val connection = DriverManager.getConnection("jdbc:h2:mem:tableinfo${System.nanoTime()}", "sa", "")
        connection.createStatement().use { s ->
            for (sql in listOf(
                "CREATE TABLE DEPT (DEPT_ID INT PRIMARY KEY, NAME VARCHAR(40) NOT NULL)",
                "COMMENT ON TABLE DEPT IS '부서'",
                "CREATE TABLE EMP (EMP_NO INT, BRANCH VARCHAR(10), NAME VARCHAR(40), SALARY DECIMAL(10,2) DEFAULT 0, " +
                    "HIRED DATE, DEPT_ID INT, PRIMARY KEY (BRANCH, EMP_NO), " +
                    "CONSTRAINT EMP_DEPT_FK FOREIGN KEY (DEPT_ID) REFERENCES DEPT (DEPT_ID))",
                "CREATE INDEX EMP_NAME_IX ON EMP (NAME, HIRED DESC)",
                "CREATE SCHEMA SHOP",
                "CREATE TABLE SHOP.\"Order Items\" (\"Line\" INT PRIMARY KEY)",
            )) s.execute(sql)
        }
        session = Session("t1", "generic", connection)
    }

    @AfterTest
    fun tearDown() {
        session.close()
    }

    @Test
    fun switchesCurrentSchema() {
        assertEquals("PUBLIC", session.currentSchema())
        val result = session.setSchema("SHOP")
        assertEquals("SHOP", result["schema"])
        assertEquals("SHOP", session.currentSchema())
        assertEquals(null, session.applyInitialSchema("PUBLIC"))
        assertEquals("PUBLIC", session.currentSchema())
        // 탭에서 바꾼 스키마를 힌트로 받으면 스키마 없이 쓴 이름을 거기서 찾는다
        val items = session.tableInfo(Ddl.Request(null, "Order Items", false, true, currentSchema = "SHOP"))
        assertEquals("SHOP", items["schema"])
        val error = session.applyInitialSchema("NO_SUCH_SCHEMA") // 실패해도 세션은 그대로
        assertTrue(error != null)
        assertEquals("PUBLIC", session.currentSchema())
    }

    @Test
    fun bindsValuesAndReadsOutParameters() {
        // IN 바인드: 위치마다 값 (숫자는 BigDecimal, 나머지는 문자열)
        val selected = session.execute(
            "SELECT NAME FROM EMP WHERE BRANCH = ? AND EMP_NO = ?", 100, 0,
            listOf(Session.Bind("A", "VARCHAR", false), Session.Bind("1", "NUMERIC", false)),
        )
        assertEquals("resultSet", selected["kind"])
        // OUT 파라미터: CallableStatement로 실행하고 값을 돌려준다
        val called = session.execute(
            "{? = CALL ABS(CAST(? AS DECIMAL(10,2)))}", 100, 0,
            listOf(Session.Bind(null, "NUMERIC", true), Session.Bind("-42", "NUMERIC", false)),
        )
        assertEquals("update", called["kind"]) // OUT 값만, 결과 표는 없다
        @Suppress("UNCHECKED_CAST")
        val outs = called["outBinds"] as List<Any?>
        assertEquals(0, java.math.BigDecimal(outs[0].toString()).compareTo(java.math.BigDecimal(42)))
        assertEquals(null, outs[1])
    }

    @Test
    fun describesPlanObjects() {
        // 인덱스 이름으로 물으면 그 인덱스의 테이블을 찾아 주고, 쓴 인덱스를 알려 준다
        val byIndex = session.planObject(PlanObject.Request("PUBLIC", "EMP_NAME_IX", "INDEX", null, null))
        assertEquals("EMP_NAME_IX", byIndex["index"])
        @Suppress("UNCHECKED_CAST")
        val table = byIndex["table"] as Map<String, Any?>
        assertEquals("EMP", table["name"])
        assertTrue(list(table, "indexes").any { it["name"] == "EMP_NAME_IX" })

        val byTable = session.planObject(PlanObject.Request(null, "DEPT", "TABLE", "PRIMARY_KEY_1", null))
        assertEquals("PRIMARY_KEY_1", byTable["index"])
        @Suppress("UNCHECKED_CAST")
        assertEquals("DEPT", (byTable["table"] as Map<String, Any?>)["name"])

        assertFailsWith<SQLException> { session.planObject(PlanObject.Request(null, "NO_SUCH_IX", "INDEX", null, null)) }
    }

    @Suppress("UNCHECKED_CAST")
    private fun list(map: Map<String, Any?>, key: String) = map[key] as List<Map<String, Any?>>

    @Test
    fun describesColumnsKeysAndIndexes() {
        val emp = session.tableInfo(Ddl.Request(null, "emp", false, false)) // 따옴표 없는 이름은 대문자로도 찾는다
        assertEquals("EMP", emp["name"])
        assertEquals("PUBLIC", emp["schema"])
        assertEquals(listOf("EMP_NO", "BRANCH", "NAME", "SALARY", "HIRED", "DEPT_ID"), list(emp, "columns").map { it["name"] })
        assertEquals(listOf("BRANCH", "EMP_NO"), emp["primaryKey"]) // KEY_SEQ 순서
        val salary = list(emp, "columns")[3]
        assertEquals(true, salary["numeric"])
        assertEquals("0", salary["default"].toString().trim('(', ')'))
        assertEquals(true, list(emp, "columns")[4]["temporal"])
        assertEquals(false, list(emp, "columns")[2]["primaryKey"])

        val index = list(emp, "indexes").single { it["name"] == "EMP_NAME_IX" }
        assertEquals(false, index["unique"])
        assertEquals(listOf("NAME", "HIRED DESC"), index["columns"])

        val fk = list(emp, "foreignKeys").single()
        assertEquals("EMP_DEPT_FK", fk["name"])
        assertEquals(listOf("DEPT_ID"), fk["columns"])
        assertEquals("DEPT", fk["refTable"])
        assertEquals(listOf("DEPT_ID"), fk["refColumns"])

        val dept = session.tableInfo(Ddl.Request("PUBLIC", "DEPT", false, false))
        assertEquals("부서", dept["remarks"])
        val referenced = list(dept, "referencedBy").single()
        assertEquals("EMP", referenced["table"])
        assertEquals(listOf("DEPT_ID"), referenced["columns"])
    }

    @Test
    fun quotedNamesAndMissingTables() {
        val items = session.tableInfo(Ddl.Request("shop", "Order Items", false, true))
        assertEquals("Order Items", items["name"])
        assertEquals(listOf("Line"), items["primaryKey"])
        val error = assertFailsWith<SQLException> { session.tableInfo(Ddl.Request(null, "NOPE", false, false)) }
        assertTrue(error.message!!.contains("does not exist"), error.message)
    }

    @Test
    fun executesStatementsInOneRequest() {
        val ok = session.executeBatch(
            listOf("INSERT INTO DEPT VALUES (1, 'A')", "INSERT INTO DEPT VALUES (2, 'B')", "UPDATE DEPT SET NAME = 'X'"),
            true,
        )
        assertEquals(3, ok["executed"])
        assertEquals(0, ok["failed"])
        assertEquals(4L, ok["affected"])

        // 멈추기: 두 번째에서 실패하면 세 번째는 실행하지 않는다
        val stop = session.executeBatch(
            listOf("INSERT INTO DEPT VALUES (3, 'C')", "INSERT INTO DEPT VALUES (1, 'DUP')", "INSERT INTO DEPT VALUES (4, 'D')"),
            true,
        )
        assertEquals(1, stop["executed"])
        assertEquals(1, stop["failed"])
        assertEquals(1, stop["skipped"])
        assertEquals(1, stop["stoppedAt"])
        @Suppress("UNCHECKED_CAST")
        val errors = stop["errors"] as List<Map<String, Any?>>
        assertEquals(1, errors.single()["index"])

        // 계속하기: 실패한 문장만 건너뛴다
        val keepGoing = session.executeBatch(
            listOf("INSERT INTO DEPT VALUES (5, 'E')", "INSERT INTO DEPT VALUES (1, 'DUP')", "INSERT INTO DEPT VALUES (6, 'F')"),
            false,
        )
        assertEquals(2, keepGoing["executed"])
        assertEquals(1, keepGoing["failed"])
        assertEquals(0, keepGoing["skipped"])
    }
}
