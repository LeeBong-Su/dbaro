package dbaro.engine.dialect.mysql

import dbaro.engine.dialect.Dialect

/** MySQL 계열 기반. 메타데이터는 information_schema와 SHOW 명령을 쓴다. */
open class MySqlDialect : Dialect {
    override val id = "mysql"

    override fun splitStatements(script: String): List<String> =
        TODO("DELIMITER는 서버 SQL이 아니라 클라이언트 명령이므로 직접 처리")
}

/** MariaDB는 MySQL과 다른 부분(시퀀스, JSON 타입 등)만 override 한다. */
class MariaDbDialect : MySqlDialect() {
    override val id = "mariadb"
}
