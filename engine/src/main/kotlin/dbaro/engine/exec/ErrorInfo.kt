package dbaro.engine.exec

import java.sql.SQLException
import java.sql.SQLTimeoutException
import java.util.concurrent.ExecutionException

/**
 * 오류를 앱으로 보낼 형태로 바꾼다. 가능하면 문장 안의 오류 위치도 함께 보낸다.
 * - position: 문장 안 0부터 시작하는 글자 위치 (PostgreSQL, Oracle)
 * - line: 문장 안 1부터 시작하는 줄 (MySQL, MariaDB의 "at line N")
 */
object ErrorInfo {
    fun of(message: String): Map<String, Any?> = mapOf("message" to message)

    fun from(error: Throwable): Map<String, Any?> {
        val root = if (error is ExecutionException && error.cause != null) error.cause!! else error
        val result = linkedMapOf<String, Any?>()
        result["message"] = message(root)
        if (root is SQLException) {
            root.sqlState?.let { result["sqlState"] = it }
            result["vendorCode"] = root.errorCode
        }
        position(root)?.let { result["position"] = it }
        line(root)?.let { result["line"] = it }
        return result
    }

    private fun message(error: Throwable): String = when {
        error is SQLTimeoutException -> error.message ?: "The statement timed out."
        error.message.isNullOrBlank() -> error.javaClass.simpleName
        else -> error.message!!.trim()
    }

    private fun position(error: Throwable): Int? {
        // PostgreSQL: PSQLException.getServerErrorMessage().getPosition() (1부터 시작, 0이면 없음)
        runCatching {
            val serverMessage = error.javaClass.getMethod("getServerErrorMessage").invoke(error)
            val position = serverMessage?.javaClass?.getMethod("getPosition")?.invoke(serverMessage) as? Int
            if (position != null && position > 0) return position - 1
        }
        // Oracle: 원인 사슬 안의 OracleDatabaseException.getErrorPosition()
        var cause: Throwable? = error
        while (cause != null) {
            val current: Throwable = cause
            if (current.javaClass.name == "oracle.jdbc.OracleDatabaseException") {
                runCatching {
                    val position = current.javaClass.getMethod("getErrorPosition").invoke(current) as? Int
                    if (position != null && position > 0) return position
                }
            }
            cause = current.cause
        }
        return null
    }

    private fun line(error: Throwable): Int? =
        Regex("""at line (\d+)""").find(error.message ?: "")?.groupValues?.get(1)?.toIntOrNull()
}
