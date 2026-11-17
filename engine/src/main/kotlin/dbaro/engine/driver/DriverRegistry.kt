package dbaro.engine.driver

import dbaro.engine.Log
import java.io.IOException
import java.net.URLClassLoader
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.sql.Driver
import java.util.concurrent.ConcurrentHashMap

/**
 * JDBC 드라이버를 찾아 불러온다.
 * 1. 연결 설정에 jar 경로가 있으면 그 jar (Tibero는 벤더 독점 드라이버라 항상 이 방식)
 * 2. 엔진 클래스패스에 이미 있으면 그대로 (테스트)
 * 3. 없으면 Maven Central에서 내려받아 driverDir에 저장하고 SHA-1을 확인한다
 * 드라이버는 jar마다 별도 클래스 로더로 불러온다. DriverManager를 거치지 않고 Driver.connect를 직접 부른다.
 */
class DriverRegistry(
    private val driverDir: Path,
    private val repositoryUrl: String = MAVEN_CENTRAL,
) {
    data class Artifact(val group: String, val name: String, val version: String) {
        val fileName: String get() = "$name-$version.jar"
        val path: String get() = "${group.replace('.', '/')}/$name/$version/$fileName"
    }

    data class Spec(val driverClass: String, val artifact: Artifact?)

    companion object {
        const val MAVEN_CENTRAL = "https://repo1.maven.org/maven2"

        val BUILT_IN: Map<String, Spec> = mapOf(
            "oracle" to Spec("oracle.jdbc.OracleDriver", Artifact("com.oracle.database.jdbc", "ojdbc11", "23.4.0.24.05")),
            "tibero" to Spec("com.tmax.tibero.jdbc.TbDriver", null),
            "mariadb" to Spec("org.mariadb.jdbc.Driver", Artifact("org.mariadb.jdbc", "mariadb-java-client", "3.4.1")),
            "mysql" to Spec("com.mysql.cj.jdbc.Driver", Artifact("com.mysql", "mysql-connector-j", "8.4.0")),
            "postgres" to Spec("org.postgresql.Driver", Artifact("org.postgresql", "postgresql", "42.7.4")),
        )
    }

    private val loaders = ConcurrentHashMap<Path, ClassLoader>()
    private val downloadLock = Any()

    fun driver(type: String, driverPath: String?, driverClass: String?): Driver {
        val spec = BUILT_IN[type]
        val className = driverClass ?: spec?.driverClass
            ?: throw IllegalArgumentException("Unknown database type: $type")

        if (driverPath != null) {
            val jar = Paths.get(driverPath)
            if (!Files.isRegularFile(jar)) throw IOException("JDBC driver file not found: $jar")
            return instantiate(loaderFor(jar), className)
        }

        loadFromClasspath(className)?.let { return it }

        val artifact = spec?.artifact ?: throw IllegalArgumentException(
            if (type == "tibero") "Select the Tibero JDBC driver (.jar) in the connection settings."
            else "No JDBC driver is configured for $type."
        )
        return instantiate(loaderFor(download(artifact)), className)
    }

    /** Maven Central에서 드라이버 jar를 내려받는다. 이미 있으면 그 파일을 쓴다. */
    fun download(artifact: Artifact): Path {
        synchronized(downloadLock) { return downloadLocked(artifact) }
    }

    private fun downloadLocked(artifact: Artifact): Path {
        val target = driverDir.resolve(artifact.fileName)
        if (Files.isRegularFile(target)) return target

        Files.createDirectories(driverDir)
        val url = "$repositoryUrl/${artifact.path}"
        Log.info("Downloading JDBC driver $url")
        val bytes = Http.get(url)
        val expected = String(Http.get("$url.sha1"), Charsets.US_ASCII).trim().take(40).lowercase()
        val actual = sha1(bytes)
        if (expected != actual) throw IOException("Checksum mismatch for ${artifact.fileName} (expected $expected, got $actual)")

        val temp = Files.createTempFile(driverDir, artifact.name, ".part")
        Files.write(temp, bytes)
        Files.move(temp, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        Log.info("Saved ${artifact.fileName} (${bytes.size / 1024} KB)")
        return target
    }

    private fun loadFromClasspath(className: String): Driver? = try {
        instantiate(DriverRegistry::class.java.classLoader, className)
    } catch (e: ClassNotFoundException) {
        null
    }

    private fun loaderFor(jar: Path): ClassLoader =
        loaders.computeIfAbsent(jar.toAbsolutePath().normalize()) {
            URLClassLoader(arrayOf(it.toUri().toURL()), DriverRegistry::class.java.classLoader)
        }

    private fun instantiate(loader: ClassLoader, className: String): Driver =
        Class.forName(className, true, loader).getDeclaredConstructor().newInstance() as Driver

    private fun sha1(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }
}
