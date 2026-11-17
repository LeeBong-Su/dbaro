package dbaro.engine.dialect.oracle

import dbaro.engine.dialect.Dialect

/** Oracle 계열 기반. 메타데이터는 USER_/DBA_ 뷰를 우선 쓴다 (ALL_ 뷰는 객체가 많으면 느리다). */
open class OracleDialect : Dialect {
    override val id = "oracle"

    override fun splitStatements(script: String): List<String> =
        TODO("PL/SQL 블록 안의 ; 와, 한 줄에 / 만 있는 블록 끝 표시 처리")
}

/** Tibero는 Oracle 호환을 지향하므로 Oracle과 다른 부분만 override 한다. */
class TiberoDialect : OracleDialect() {
    override val id = "tibero"
}
