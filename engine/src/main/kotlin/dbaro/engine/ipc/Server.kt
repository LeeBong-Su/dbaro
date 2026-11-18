package dbaro.engine.ipc

import dbaro.engine.ENGINE_VERSION
import dbaro.engine.Log
import dbaro.engine.bool
import dbaro.engine.daemonThreads
import dbaro.engine.driver.DriverRegistry
import dbaro.engine.exec.Connector
import dbaro.engine.exec.Ddl
import dbaro.engine.exec.ErrorInfo
import dbaro.engine.exec.Session
import dbaro.engine.int
import dbaro.engine.string
import dbaro.engine.stringOrNull
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/**
 * 앱의 요청을 읽어 처리한다.
 * - 읽기 스레드 하나가 요청을 받아 나눠 준다. 취소(cancel)는 실행 중인 문장과 상관없이 바로 처리한다.
 * - 세션마다 전용 스레드가 있어서 한 세션의 작업은 순서대로, 세션끼리는 동시에 실행된다.
 * - 응답은 여러 스레드에서 오므로 쓰기는 잠금으로 묶는다.
 */
class Server(input: InputStream, output: OutputStream, private val drivers: DriverRegistry) {
    private val reader = DataInputStream(BufferedInputStream(input))
    private val writer = DataOutputStream(BufferedOutputStream(output))
    private val writeLock = Any()
    private val sessions = ConcurrentHashMap<String, Session>()
    private val sessionIds = AtomicLong(1)
    private val workers = Executors.newCachedThreadPool(daemonThreads("worker"))

    fun run() {
        try {
            while (true) {
                val frame = Framing.read(reader) ?: break
                val message = try {
                    Cbor.decode(frame) as? Map<*, *>
                } catch (e: Exception) {
                    Log.error("Ignoring a malformed message", e)
                    null
                } ?: continue

                val id = (message["id"] as? Number)?.toLong() ?: continue
                try {
                    handle(id, message)
                } catch (e: Throwable) {
                    replyError(id, ErrorInfo.from(e))
                }
            }
        } finally {
            shutdown()
        }
    }

    private fun handle(id: Long, message: Map<*, *>) {
        val method = message["method"] as? String ?: return replyError(id, ErrorInfo.of("Missing method"))
        @Suppress("UNCHECKED_CAST")
        val params = (message["params"] as? Map<String, Any?>) ?: emptyMap()

        when (method) {
            "hello" -> reply(id, mapOf("version" to ENGINE_VERSION, "java" to System.getProperty("java.version")))
            "cancel" -> {
                sessions[params.string("sessionId")]?.cancel()
                reply(id, emptyMap())
            }
            "test" -> workers.execute { respond(id) { Connector.test(drivers, params) } }
            "connect" -> workers.execute {
                respond(id) {
                    val session = Connector.open(drivers, params, "s${sessionIds.getAndIncrement()}")
                    sessions[session.id] = session
                    session.info()
                }
            }
            "disconnect" -> {
                val session = sessions.remove(params.string("sessionId"))
                if (session == null) reply(id, emptyMap()) else session.submit { respond(id) { session.close(); emptyMap() } }
            }
            "execute" -> withSession(id, params) {
                it.execute(params.string("sql"), params.int("pageSize", 500), params.int("queryTimeoutSec", 0))
            }
            "fetch" -> withSession(id, params) { it.fetch(params.string("cursorId"), params.int("count", 500)) }
            "closeCursor" -> withSession(id, params) { it.closeCursor(params.string("cursorId")); emptyMap() }
            "commit" -> withSession(id, params) { it.commit(); emptyMap() }
            "rollback" -> withSession(id, params) { it.rollback(); emptyMap() }
            "setAutoCommit" -> withSession(id, params) { it.setAutoCommit(params.bool("autoCommit", false)); emptyMap() }
            "explain" -> withSession(id, params) { it.explain(params.string("sql")) }
            "ddl" -> withSession(id, params) {
                it.ddl(
                    Ddl.Request(
                        params.stringOrNull("schema")?.takeIf { s -> s.isNotEmpty() },
                        params.string("name"),
                        params.bool("schemaQuoted", false),
                        params.bool("nameQuoted", false),
                    ),
                )
            }
            "metaSchemas" -> withSession(id, params) { it.schemas() }
            "metaObjects" -> withSession(id, params) { it.objects(params.string("schema")) }
            "metaColumns" -> withSession(id, params) { it.columns(params.string("schema"), params.string("table")) }
            else -> replyError(id, ErrorInfo.of("Unknown method: $method"))
        }
    }

    private fun withSession(id: Long, params: Map<String, Any?>, work: (Session) -> Map<String, Any?>) {
        val session = sessions[params.string("sessionId")]
            ?: return replyError(id, ErrorInfo.of("The session is not connected."))
        session.submit { respond(id) { work(session) } }
    }

    private fun respond(id: Long, work: () -> Map<String, Any?>) {
        try {
            reply(id, work())
        } catch (e: Throwable) {
            replyError(id, ErrorInfo.from(e))
        }
    }

    private fun reply(id: Long, result: Map<String, Any?>) = send(mapOf("id" to id, "result" to result))

    private fun replyError(id: Long, error: Map<String, Any?>) = send(mapOf("id" to id, "error" to error))

    private fun send(message: Map<String, Any?>) {
        val bytes = Cbor.encode(message)
        try {
            synchronized(writeLock) { Framing.write(writer, bytes) }
        } catch (e: Exception) {
            Log.error("Could not send a response", e)
        }
    }

    private fun shutdown() {
        for (session in sessions.values) runCatching { session.close() }
        sessions.clear()
        workers.shutdownNow()
    }
}
