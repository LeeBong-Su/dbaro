package dbaro.engine.dialect

/**
 * DB 방언별 차이를 모으는 곳.
 *
 * 다섯 개 DB를 세 계열로 나눈다. 계열마다 기반 클래스를 두고,
 * 같은 계열의 DB는 다른 부분만 override 한다.
 * - oracle: Oracle, Tibero
 * - mysql: MySQL, MariaDB
 * - postgres: PostgreSQL
 */
interface Dialect {
    val id: String

    /** 스크립트를 실행 단위 문장으로 나눈다. 구분 규칙은 계열마다 다르다. */
    fun splitStatements(script: String): List<String>
}
