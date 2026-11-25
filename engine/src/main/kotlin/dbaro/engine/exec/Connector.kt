package dbaro.engine.exec

import dbaro.engine.Log
import dbaro.engine.bool
import dbaro.engine.daemonThreads
import dbaro.engine.driver.DriverRegistry
import dbaro.engine.int
import dbaro.engine.string
import dbaro.engine.stringList
import dbaro.engine.stringOrNull
import java.sql.Connection
import java.sql.SQLException
import java.sql.SQLTimeoutException
import java.util.Properties
import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

/** 연결 열기와 연결 테스트. */
object Connector {
    /** Oracle V$SESSION의 PROGRAM·MODULE에 보일 이름. DBA가 세션을 DBaro 것으로 알아볼 수 있게 한다. */
    const val CLIENT_NAME = "DBaro"

    private val pool = Executors.newCachedThreadPool(daemonThreads("connect"))

    fun open(drivers: DriverRegistry, params: Map<String, Any?>, sessionId: String): Session {
        val type = params.string("type")
        val connection = connect(drivers, params)
        try {
            if (type == "oracle") {
                // MODULE은 접속 속성으로 정할 수 없어서 클라이언트 정보로 보낸다 (다음 왕복 때 함께 전달됨).
                runCatching { connection.setClientInfo("OCSID.MODULE", CLIENT_NAME) }
                    .onFailure { Log.error("Could not set the Oracle module name", it) }
            }
            connection.autoCommit = params.bool("autoCommit", false)
            if (params.bool("readOnly", false)) runCatching { connection.isReadOnly = true }
            for (sql in params.stringList("initSql")) {
                connection.createStatement().use { it.execute(sql) }
            }
            return Session(sessionId, type, connection)
        } catch (e: Throwable) {
            runCatching { connection.close() }
            throw e
        }
    }

    fun test(drivers: DriverRegistry, params: Map<String, Any?>): Map<String, Any?> {
        val start = System.nanoTime()
        connect(drivers, params).use { connection ->
            val meta = connection.metaData
            return mapOf(
                "product" to meta.databaseProductName,
                "productVersion" to meta.databaseProductVersion,
                "driverName" to meta.driverName,
                "driverVersion" to meta.driverVersion,
                "elapsedMs" to (System.nanoTime() - start) / 1_000_000,
            )
        }
    }

    private fun connect(drivers: DriverRegistry, params: Map<String, Any?>): Connection {
        val type = params.string("type")
        val url = params.string("url")
        val driver = drivers.driver(type, params.stringOrNull("driverPath"), params.stringOrNull("driverClass"))
        val timeoutSec = params.int("connectTimeoutSec", 10).coerceAtLeast(1)

        val properties = connectionProperties(type, params, timeoutSec)

        // 드라이버가 접속 시간 제한을 지키지 않아도 앱이 기다리지 않도록 따로 시간을 잰다.
        val future = pool.submit(Callable {
            driver.connect(url, properties) ?: throw SQLException("The JDBC driver does not accept this URL: $url")
        })
        try {
            return future.get(timeoutSec + 5L, TimeUnit.SECONDS)
        } catch (e: TimeoutException) {
            pool.execute { runCatching { future.get().close() } } // 늦게 열리면 바로 닫는다
            throw SQLTimeoutException("Connection timed out after $timeoutSec s.")
        } catch (e: ExecutionException) {
            throw e.cause ?: e
        }
    }

    internal fun connectionProperties(type: String, params: Map<String, Any?>, timeoutSec: Int): Properties {
        val properties = Properties()
        params.stringOrNull("user")?.let { properties.setProperty("user", it) }
        (params["password"] as? String)?.let { properties.setProperty("password", it) }
        when (type) {
            "oracle" -> {
                properties.setProperty("oracle.net.CONNECT_TIMEOUT", (timeoutSec * 1000).toString())
                // 테이블·컬럼 코멘트(REMARKS)를 메타데이터에 포함한다 (국내는 코멘트에 한글 이름을 적는 경우가 많다).
                properties.setProperty("remarksReporting", "true")
                // V$SESSION.PROGRAM (기본값은 "JDBC Thin Client")
                properties.setProperty("v\$session.program", CLIENT_NAME)
            }
            "postgres" -> properties.setProperty("connectTimeout", timeoutSec.toString())
            "mysql", "mariadb" -> {
                properties.setProperty("connectTimeout", (timeoutSec * 1000).toString())
                // YEAR를 날짜(2024-01-01)가 아니라 연도 숫자로
                properties.setProperty("yearIsDateType", "false")
            }
        }
        return properties
    }
}
