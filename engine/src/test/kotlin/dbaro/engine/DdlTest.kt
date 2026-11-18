package dbaro.engine

import dbaro.engine.exec.Ddl
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/** DB 서버 없이 확인할 수 있는 DDL 조립 부분 */
class DdlTest {
    @Test
    fun oracleSynonymStatements() {
        assertEquals(
            "CREATE OR REPLACE PUBLIC SYNONYM \"EMPLOYEES\" FOR \"HR\".\"EMPLOYEES\";",
            Ddl.oracleSynonym("PUBLIC", "EMPLOYEES", "\"HR\".\"EMPLOYEES\""),
        )
        assertEquals(
            "CREATE OR REPLACE SYNONYM \"APP\".\"EMP\" FOR \"HR\".\"EMPLOYEES\";",
            Ddl.oracleSynonym("APP", "EMP", "\"HR\".\"EMPLOYEES\""),
        )
    }

    @Test
    fun grantsAreGroupedByGranteeAndGrantOption() {
        val statements = Ddl.grantStatements(
            "\"HR\".\"EMPLOYEES\"",
            listOf(
                Triple("APP", "SELECT", false),
                Triple("APP", "UPDATE", false),
                Triple("REPORT", "SELECT", true),
                Triple("APP", "SELECT", false), // 중복
            ),
            Ddl::doubleQuoted,
        )
        assertEquals(
            listOf(
                "GRANT SELECT, UPDATE ON \"HR\".\"EMPLOYEES\" TO \"APP\";",
                "GRANT SELECT ON \"HR\".\"EMPLOYEES\" TO \"REPORT\" WITH GRANT OPTION;",
            ),
            statements,
        )
    }

    @Test
    fun builderSkipsEmptySectionsAndEscapesStrings() {
        val builder = Ddl.Builder("HR.EMPLOYEES (TABLE)")
        builder.main("CREATE TABLE x (a INT);")
        builder.section("Comments", listOf("COMMENT ON TABLE x IS ${Ddl.sqlString("O'Brien")};"))
        builder.section("Grants", emptyList())
        val script = builder.toString()
        assertTrue(script.startsWith("-- HR.EMPLOYEES (TABLE)\n\nCREATE TABLE x (a INT);"), script)
        assertTrue(script.contains("-- Comments\nCOMMENT ON TABLE x IS 'O''Brien';"), script)
        assertFalse(script.contains("-- Grants"), script)
    }
}
