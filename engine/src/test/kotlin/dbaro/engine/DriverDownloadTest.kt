package dbaro.engine

import com.sun.net.httpserver.HttpServer
import dbaro.engine.driver.DriverRegistry
import java.io.IOException
import java.net.InetSocketAddress
import java.nio.file.Files
import java.security.MessageDigest
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse

/** 로컬 HTTP 서버를 Maven 저장소처럼 띄워 드라이버 다운로드와 체크섬 확인을 검사한다 (외부 접속 없음). */
class DriverDownloadTest {
    private val jar = "not really a jar".toByteArray()
    private var checksum = sha1(jar)
    private var downloads = 0
    private lateinit var server: HttpServer

    @BeforeTest
    fun start() {
        server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        server.createContext("/") { exchange ->
            val path = exchange.requestURI.path
            val body = when {
                path.endsWith(".jar") -> jar.also { downloads++ }
                path.endsWith(".jar.sha1") -> "$checksum  demo-1.0.jar".toByteArray()
                else -> null
            }
            if (body == null) {
                exchange.sendResponseHeaders(404, -1)
            } else {
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.use { it.write(body) }
            }
            exchange.close()
        }
        server.start()
    }

    @AfterTest
    fun stop() = server.stop(0)

    private fun registry(dir: java.nio.file.Path) = DriverRegistry(dir, "http://127.0.0.1:${server.address.port}")

    @Test
    fun downloadsVerifiesAndCaches() {
        val dir = Files.createTempDirectory("drivers")
        val artifact = DriverRegistry.Artifact("org.example", "demo", "1.0")
        val path = registry(dir).download(artifact)
        assertContentEquals(jar, Files.readAllBytes(path))
        assertEquals("demo-1.0.jar", path.fileName.toString())
        registry(dir).download(artifact) // 두 번째는 저장된 파일을 쓴다
        assertEquals(1, downloads)
    }

    @Test
    fun rejectsChecksumMismatch() {
        checksum = "0".repeat(40)
        val dir = Files.createTempDirectory("drivers")
        assertFailsWith<IOException> { registry(dir).download(DriverRegistry.Artifact("org.example", "demo", "1.0")) }
        assertFalse(Files.exists(dir.resolve("demo-1.0.jar")))
    }

    private fun sha1(bytes: ByteArray) = MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }
}
