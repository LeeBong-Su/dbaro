package dbaro.engine.dialect.postgres

import dbaro.engine.dialect.Dialect

/** PostgreSQL. 메타데이터는 pg_catalog를 쓴다. */
class PostgresDialect : Dialect {
    override val id = "postgres"

    override fun splitStatements(script: String): List<String> =
        TODO("\$\$ 또는 \$tag\$ 로 감싼 함수 본문 안의 ; 는 무시")
}
