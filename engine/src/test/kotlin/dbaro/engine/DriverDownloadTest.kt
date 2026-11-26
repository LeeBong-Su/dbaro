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

    @Test
    fun downloadsCompanionsButGoesOnWithoutThem() {
        val dir = Files.createTempDirectory("drivers")
        val registry = registry(dir)
        val spec = DriverRegistry.Spec(
            "demo.Driver",
            DriverRegistry.Artifact("org.example", "demo", "1.0"),
            listOf(DriverRegistry.Artifact("org.example", "nls", "1.0")),
        )
        // 함께 쓰는 jar도 받는다
        assertEquals(listOf("demo-1.0.jar", "nls-1.0.jar"), registry.builtInJars(spec).map { it.fileName.toString() })

        // 함께 쓰는 jar를 받지 못하면(체크섬이 틀림·폐쇄망) 빼고 계속한다. 같은 실행에서는 다시 받으려 하지 않는다
        val partial = registry(Files.createTempDirectory("drivers"))
        val main = partial.download(spec.artifact!!)
        checksum = "0".repeat(40)
        assertEquals(listOf(main), partial.builtInJars(spec))
        val attempts = downloads
        assertEquals(listOf(main), partial.builtInJars(spec))
        assertEquals(attempts, downloads, "no second attempt")
    }

    @Test
    fun userJarsTakeAListAndOraclesNlsJar() {
        val dir = Files.createTempDirectory("jdbc")
        val ojdbc = Files.write(dir.resolve("ojdbc8.jar"), ByteArray(1))
        val nls = Files.write(dir.resolve("orai18n.jar"), ByteArray(1))
        val other = Files.write(Files.createTempDirectory("jdbc").resolve("extra.jar"), ByteArray(1))
        val registry = registry(Files.createTempDirectory("drivers"))
        // Oracle: 같은 폴더의 orai18n.jar를 함께
        assertEquals(listOf(ojdbc, nls).map { it.toAbsolutePath().normalize() }, registry.userJars("oracle", ojdbc.toString()))
        // 다른 DB는 지정한 것만, ';'로 여러 개
        assertEquals(listOf(ojdbc, other).map { it.toAbsolutePath().normalize() }, registry.userJars("tibero", "$ojdbc ; $other"))
        assertFailsWith<IOException> { registry.userJars("oracle", dir.resolve("missing.jar").toString()) }
    }

    private fun sha1(bytes: ByteArray) = MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }
}
