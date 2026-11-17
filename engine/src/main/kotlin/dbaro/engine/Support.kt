package dbaro.engine

import java.util.concurrent.ThreadFactory
import java.util.concurrent.atomic.AtomicInteger

const val ENGINE_VERSION = "0.1.0"

/** 로그는 항상 표준 오류로 보낸다. 표준 출력은 프로토콜 전용이다. */
object Log {
    fun info(message: String) = System.err.println("[engine] $message")
    fun error(message: String, error: Throwable? = null) =
        System.err.println("[engine] ERROR $message" + (error?.let { ": ${it.javaClass.simpleName}: ${it.message}" } ?: ""))
}

/** 데몬 스레드 팩토리. 앱이 연결을 끊으면 엔진 프로세스가 남지 않고 끝나게 한다. */
fun daemonThreads(name: String): ThreadFactory {
    val counter = AtomicInteger(1)
    return ThreadFactory { task -> Thread(task, "$name-${counter.getAndIncrement()}").apply { isDaemon = true } }
}

// 요청 파라미터 읽기 도우미
fun Map<String, Any?>.string(key: String): String =
    this[key] as? String ?: throw IllegalArgumentException("Missing parameter: $key")

fun Map<String, Any?>.stringOrNull(key: String): String? = (this[key] as? String)?.takeIf { it.isNotEmpty() }

fun Map<String, Any?>.int(key: String, default: Int): Int = (this[key] as? Number)?.toInt() ?: default

fun Map<String, Any?>.bool(key: String, default: Boolean): Boolean = this[key] as? Boolean ?: default

fun Map<String, Any?>.stringList(key: String): List<String> =
    (this[key] as? List<*>)?.mapNotNull { it as? String } ?: emptyList()
